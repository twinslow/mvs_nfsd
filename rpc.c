/*
 * rpc.c - RPC over TCP framing (RFC 5531).
 *
 * TCP record marking: each RPC message is preceded by a 4-byte header.
 * Bit 31 = "last fragment" flag.  Bits 30-0 = fragment byte count.
 * We always send and expect single-fragment messages.
 *
 * On MVS: replace the recv()/send() calls here with whatever socket
 * API is exposed by the Hercules TCP/IP instruction interface.
 */

#include <string.h>        /* memset */
#include <sys/socket.h>    /* recv, send */
#include "nfsd.h"

/* ------------------------------------------------------------------ */
/* Internal helpers: receive / send all bytes with retries              */
/* ------------------------------------------------------------------ */

static int recv_all(int fd, uint8_t *buf, uint32_t len)
{
    uint32_t done = 0;
    int n;

    while (done < len) {
        n = (int)recv(fd, buf + done, (size_t)(len - done), 0);
        if (n <= 0) return -1;   /* connection closed or error */
        done += (uint32_t)n;
    }
    return 0;
}

static int send_all(int fd, const uint8_t *buf, uint32_t len)
{
    uint32_t done = 0;
    int n;

    while (done < len) {
        n = (int)send(fd, buf + done, (size_t)(len - done), 0);
        if (n <= 0) return -1;
        done += (uint32_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* rpc_recv: read one complete RPC message from fd into buf.            */
/*                                                                      */
/* Handles multi-fragment messages by reassembling into a single        */
/* buffer.  Sets *msglen to the total reassembled byte count.           */
/* Returns 0 on success, -1 if the connection should be closed.         */
/* ------------------------------------------------------------------ */
int rpc_recv(int fd, uint8_t *buf, uint32_t maxlen, uint32_t *msglen)
{
    uint8_t  mark[4];
    uint32_t total = 0;
    uint32_t rm;
    uint32_t flen;
    int      last;

    do {
        if (recv_all(fd, mark, 4) < 0) return -1;

        rm   = ((uint32_t)mark[0] << 24)
             | ((uint32_t)mark[1] << 16)
             | ((uint32_t)mark[2] <<  8)
             |  (uint32_t)mark[3];
        last = (int)((rm >> 31) & 1u);
        flen = rm & 0x7FFFFFFFu;

        if (total + flen > maxlen) return -1;
        if (recv_all(fd, buf + total, flen) < 0) return -1;
        total += flen;
    } while (!last);

    *msglen = total;
    return 0;
}

/* ------------------------------------------------------------------ */
/* rpc_send: send buf as a single-fragment RPC record over fd.          */
/* Prepends the 4-byte record mark (last-fragment bit set).             */
/* ------------------------------------------------------------------ */
int rpc_send(int fd, const uint8_t *buf, uint32_t len)
{
    uint8_t  mark[4];
    uint32_t rm = 0x80000000u | len;

    mark[0] = (uint8_t)(rm >> 24);
    mark[1] = (uint8_t)(rm >> 16);
    mark[2] = (uint8_t)(rm >>  8);
    mark[3] = (uint8_t)(rm);

    if (send_all(fd, mark, 4)  < 0) return -1;
    if (send_all(fd, buf,  len) < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* rpc_parse_call: decode the RPC CALL header from an xdr_t buffer.    */
/*                                                                      */
/* Reads: xid, msg_type(CALL), rpcvers(2), prog, vers, proc,           */
/* credentials (AUTH_NULL or AUTH_UNIX), verifier.                      */
/* Fills call->auth_uid / auth_gid from AUTH_UNIX if present.          */
/* Returns 0 on success, -1 if the message is not a valid CALL.         */
/* ------------------------------------------------------------------ */
int rpc_parse_call(xdr_t *x, rpc_call_t *call)
{
    uint32_t mtype;
    uint32_t rpcvers;
    uint32_t flavor;
    uint32_t cred_len;
    uint32_t body_end;
    uint32_t mlen;
    uint32_t verf_len;
    uint32_t ngids;

    call->auth_uid = 0;
    call->auth_gid = 0;

    call->xid  = xdr_read_uint32(x);
    mtype      = xdr_read_uint32(x);
    rpcvers    = xdr_read_uint32(x);
    call->prog = xdr_read_uint32(x);
    call->vers = xdr_read_uint32(x);
    call->proc = xdr_read_uint32(x);

    if (x->error || mtype != RPC_CALL || rpcvers != 2) return -1;

    /* --- Credentials --- */
    flavor   = xdr_read_uint32(x);
    cred_len = xdr_read_uint32(x);
    if (x->error) return -1;

    if (flavor == AUTH_FLAVOR_UNIX && cred_len >= 12
        && cred_len <= x->capacity - x->pos) {
        /* stamp(4) + machinename_len(4) + machinename + uid(4) + gid(4) */
        body_end = x->pos + ((cred_len + 3u) & ~3u);

        xdr_skip(x, 4);                         /* stamp             */
        mlen = xdr_read_uint32(x);              /* machinename len   */
        xdr_skip(x, (mlen + 3u) & ~3u);         /* machinename data  */
        call->auth_uid = xdr_read_uint32(x);
        call->auth_gid = xdr_read_uint32(x);

        /* skip extra gids if present */
        if (!x->error && x->pos < body_end) {
            ngids = xdr_read_uint32(x);
            xdr_skip(x, ngids * 4u);
        }
        /* realign to body end */
        if (!x->error) x->pos = body_end;
    } else {
        xdr_skip(x, (cred_len + 3u) & ~3u);
    }

    /* --- Verifier (always skip) --- */
    xdr_skip(x, 4);                             /* verf flavor       */
    verf_len = xdr_read_uint32(x);
    xdr_skip(x, (verf_len + 3u) & ~3u);

    return x->error ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* rpc_write_accept_hdr: write the RPC reply header for an accepted     */
/* message up to and including accept_stat.                             */
/* The caller then writes the procedure-specific result fields.         */
/*                                                                      */
/* Common accept_stat values:                                           */
/*   RPC_SUCCESS(0), PROG_UNAVAIL(1), PROG_MISMATCH(2),                */
/*   PROC_UNAVAIL(3), GARBAGE_ARGS(4)                                   */
/* ------------------------------------------------------------------ */
void rpc_write_accept_hdr(xdr_t *x, uint32_t xid, uint32_t accept_stat)
{
    xdr_write_uint32(x, xid);
    xdr_write_uint32(x, RPC_REPLY);
    xdr_write_uint32(x, MSG_ACCEPTED);
    xdr_write_uint32(x, AUTH_FLAVOR_NULL); /* reply verifier flavor */
    xdr_write_uint32(x, 0u);               /* reply verifier length */
    xdr_write_uint32(x, accept_stat);
}

/* ------------------------------------------------------------------ */
/* rpc_write_prog_mismatch: send a PROG_MISMATCH rejection.             */
/* ------------------------------------------------------------------ */
void rpc_write_prog_mismatch(xdr_t *x, uint32_t xid,
                              uint32_t lo, uint32_t hi)
{
    rpc_write_accept_hdr(x, xid, PROG_MISMATCH);
    xdr_write_uint32(x, lo);
    xdr_write_uint32(x, hi);
}

/* ------------------------------------------------------------------ */
/* rpc_write_proc_unavail: send a PROC_UNAVAIL rejection.               */
/* ------------------------------------------------------------------ */
void rpc_write_proc_unavail(xdr_t *x, uint32_t xid)
{
    rpc_write_accept_hdr(x, xid, PROC_UNAVAIL);
}
