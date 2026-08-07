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
#include <time.h>          /* struct timeval */
#ifdef __MVS__
#include <sockets.h>       /* recv, send, select, fd_set */
#else
#include <sys/socket.h>    /* recv, send */
#include <sys/select.h>    /* select, fd_set */
#endif
#include "nfsd.h"

/* ------------------------------------------------------------------ */
/* Internal helpers: receive / send all bytes with retries              */
/*                                                                      */
/* recv() and send() here are BLOCKING and the server is                */
/* single-threaded, so a peer that goes quiet part-way through a        */
/* message would stall EVERYTHING: every other client, the idle-flush   */
/* sweep (leaving buffered members unstowed), and the operator-command  */
/* poll -- so even "F NFSD,STOP" would stop working and CANCEL would be */
/* the only way out, which also loses the log tail (JCC's fflush is a   */
/* no-op).                                                              */
/*                                                                      */
/* The select loop in nfsd.c only guarantees that the FIRST recv() of a */
/* message has data waiting.  Nothing guarantees the REST arrives:      */
/*                                                                      */
/*   - a record mark that over-declares its length (one that still fits */
/*     maxlen, so the check in rpc_recv passes) leaves us waiting for   */
/*     bytes the sender was never going to send;                        */
/*   - a peer that dies without a FIN -- killed VM, pulled cable,       */
/*     dropped NAT entry -- leaves TCP with nothing to report;          */
/*   - a client that simply stalls mid-message.                         */
/*                                                                      */
/* send() has the mirror image: a client that stops draining its socket */
/* fills our send buffer and blocks us there instead.                   */
/*                                                                      */
/* TCP does not rescue us: SO_KEEPALIVE is never set on these sockets,  */
/* and even where keepalive is enabled by default it is measured in     */
/* hours.  So each transfer is gated on a select() with a timeout.      */
/* ------------------------------------------------------------------ */

/* Seconds without progress on a PARTIALLY transferred message before the
   connection is abandoned.
   This is an INTER-SEGMENT timeout, not a whole-message one: every recv()
   or send() that moves even one byte restarts the clock, so a slow but
   advancing client is never dropped and only genuine silence trips it.
   Chosen comfortably longer than any real stall on a healthy link, and
   shorter than the ~60s RPC timeout a client applies, so we give up on the
   connection before the client gives up on us.  JCC's select() honours
   whole seconds only, which is exactly the granularity wanted here. */
#define RPC_IO_TIMEOUT_SECONDS  30

/* Wait until fd is ready, or the timeout expires.
   for_write == 0 waits for readability, non-zero for writability.
   Returns 0 when ready, -1 on timeout or a select() failure.  Timeouts are
   NOT logged here -- the caller reports them with the byte counts, which is
   the part that says what actually went wrong. */
static int rpc_wait_io(int fd, int for_write)
{
    fd_set         fds;
    struct timeval tv;
    int            n;

    for (;;) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec  = RPC_IO_TIMEOUT_SECONDS;
        tv.tv_usec = 0;

        if (for_write)
            n = select(fd + 1, NULL, &fds, NULL, &tv);
        else
            n = select(fd + 1, &fds, NULL, NULL, &tv);

        if (n > 0)  return 0;     /* ready */
        if (n == 0) return -1;    /* timed out -- caller reports it */

#ifndef __MVS__
        /* A signal is not a failure: reissue the wait. */
        if (errno == EINTR) continue;
#endif
        logmsg_error("NFSRP010E", "rpc: select() failed on fd=%d while waiting to %s",
                  fd, for_write ? "send" : "receive");
        return -1;
    }
}

static int recv_all(int fd, uint8_t *buf, uint32_t len)
{
    uint32_t done = 0;
    int n;

    while (done < len) {
        if (rpc_wait_io(fd, 0) < 0) {
            logmsg_error("NFSRP020E", "rpc_recv: fd=%d stalled after %u of %u byte(s) --"
                      " no data for %d seconds; dropping the connection",
                      fd, done, len, RPC_IO_TIMEOUT_SECONDS);
            return -1;
        }
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
        if (rpc_wait_io(fd, 1) < 0) {
            logmsg_error("NFSRP030E", "rpc_send: fd=%d blocked after %u of %u byte(s) --"
                      " peer stopped reading for %d seconds; dropping the"
                      " connection", fd, done, len, RPC_IO_TIMEOUT_SECONDS);
            return -1;
        }
        n = (int)send(fd, (void *)(buf + done), (size_t)(len - done), 0);
        if (n <= 0) return -1;
        done += (uint32_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Receive-corruption self-check (temporary; see spill_corruption_open) */
/*                                                                      */
/* An intermittent bug corrupts a written PDS member: a whole inbound   */
/* RPC request image (its own file handle + AUTH_UNIX cred) ends up      */
/* embedded in the middle of the WRITE payload, with the file data       */
/* restarting from offset 0 after it.  The receive -> store chain is     */
/* provably a faithful copy, so the corruption is already present in the */
/* reassembled buffer when rpc_recv returns.  This check catches it at   */
/* that exact point.                                                    */
/*                                                                      */
/* Every our_fhandle_t carries the ASCII magic "NFS3" (0x4E465333).      */
/*                                                                      */
/* The check is applied ONLY to NFS WRITE calls, which carry exactly one */
/* file handle, so a second occurrence means a foreign request image bled */
/* into the buffer.  It must not be applied to every request: RENAME and */
/* LINK legitimately carry TWO handles (from-dir and to-dir), and scanning */
/* those produced a false positive -- two hits 80 bytes apart, which is   */
/* just handle(60) + a padded filename, exactly the RENAME wire layout.  */
/*                                                                      */
/* ROOT CAUSE (found 2026-07-25): a partial recv() on this MVS socket    */
/* layer can REPLAY the message from byte 0 instead of continuing.  A    */
/* 636-byte WRITE arrived as M[0..255] followed by M[0..379]: same xid,  */
/* one fragment, and the record mark agreed with the message's own       */
/* fields, so the framing was right and the BYTES were duplicated.       */
/* recv_all() writes to buf+done, so the destination was correct -- the  */
/* source restarted.  Not a MVS NFSD bug; the stack lied to us.          */
/*                                                                      */
/* This check therefore does more than report: it REJECTS the message    */
/* (rpc_recv returns -1 -> the connection closes -> the client resends), */
/* so corrupt data can never reach a PDS member.  Rejection is on PROOF  */
/* only -- a failed length invariant, or a structurally complete RPC     */
/* CALL header embedded in the payload.  Two "NFS3" magics alone is NOT  */
/* sufficient, because file data may legitimately contain those bytes    */
/* and dropping a good connection would be its own corruption.          */
/*                                                                      */
/* Silent unless corruption is seen, so it is safe to leave enabled.     */
/* ------------------------------------------------------------------ */
#define RPC_MAX_FRAGS     32     /* fragment offsets/lengths recorded   */

/* Deliberately STATIC, not automatic.  These were locals in rpc_recv, which
   added 256 bytes to the stack frame of the hottest function in the server --
   and the corruption under investigation stopped reproducing the moment that
   build went in (~100 clean passes against a historical ~2-in-10 failure
   rate).  Growing a frame in the receive path is a textbook way to MASK a
   stack overflow rather than fix it, so keep the frame at its original size
   while retaining the diagnostic.  Safe as statics: the server is
   single-threaded and these are consumed before rpc_recv returns. */
static uint32_t g_rpc_frag_off[RPC_MAX_FRAGS];
static uint32_t g_rpc_frag_len[RPC_MAX_FRAGS];

/* Fixed offsets in an RPC CALL header: xid(0) mtype(4) rpcvers(8)       */
/* prog(12) vers(16) proc(20).                                          */
#define RPC_CALL_PROG_OFF   12u
#define RPC_CALL_PROC_OFF   20u
#define RPC_PROG_NFS        100003u
#define RPC_NFS3_PROC_WRITE 7u

static uint32_t rpc_peek_uint32(const uint8_t *buf, uint32_t off)
{
    return ((uint32_t)buf[off]     << 24)
         | ((uint32_t)buf[off + 1] << 16)
         | ((uint32_t)buf[off + 2] <<  8)
         |  (uint32_t)buf[off + 3];
}

/* XDR pads every opaque/string up to a 4-byte boundary. */
#define RPC_ROUND4(n)  (((n) + 3u) & ~3u)

/* ------------------------------------------------------------------ */
/* rpc_write_expected_len: a WRITE call's length is fully determined by  */
/* its own fields, so it can be recomputed and compared against the     */
/* length the record mark claimed.  Walks the CALL header, the AUTH     */
/* credential and verifier, then the WRITE arguments                    */
/* (fh, offset, count, stable, data), and returns the byte position     */
/* just past the padded data.                                           */
/*                                                                      */
/* This is the discriminator between the two candidate root causes:      */
/*   expected < total  -> the record mark over-declared the length, so   */
/*                        recv_all ran on into the NEXT message and      */
/*                        appended it to this payload (framing desync).  */
/*   expected == total -> the framing was right and the BYTES were bad   */
/*                        (a short/stale copy inside recv()).            */
/*                                                                      */
/* Returns 0 and sets *expected, or -1 if the message is too short to    */
/* walk (in which case no conclusion is drawn).                          */
/* ------------------------------------------------------------------ */
static int rpc_write_expected_len(const uint8_t *buf, uint32_t total,
                                  uint32_t *expected)
{
    uint32_t pos = 24u;         /* past xid..proc */
    uint32_t len;

    /* credential: flavor + length + padded body */
    if (pos + 8u > total) return -1;
    pos += 4u;
    len  = rpc_peek_uint32(buf, pos);
    pos += 4u;
    if (len > total || pos + RPC_ROUND4(len) > total) return -1;
    pos += RPC_ROUND4(len);

    /* verifier: flavor + length + padded body */
    if (pos + 8u > total) return -1;
    pos += 4u;
    len  = rpc_peek_uint32(buf, pos);
    pos += 4u;
    if (len > total || pos + RPC_ROUND4(len) > total) return -1;
    pos += RPC_ROUND4(len);

    /* file handle: length + padded body */
    if (pos + 4u > total) return -1;
    len  = rpc_peek_uint32(buf, pos);
    pos += 4u;
    if (len > total || pos + RPC_ROUND4(len) > total) return -1;
    pos += RPC_ROUND4(len);

    /* offset (8) + count (4) + stable (4) */
    if (pos + 16u > total) return -1;
    pos += 16u;

    /* data: length + padded body */
    if (pos + 4u > total) return -1;
    len  = rpc_peek_uint32(buf, pos);
    pos += 4u;
    if (len > total) return -1;

    *expected = pos + RPC_ROUND4(len);
    return 0;
}

//#define QUICK_CHECK

static int rpc_recv_selfcheck(const uint8_t *buf, uint32_t total, int nfrag,
                               const uint32_t *frag_off,
                               const uint32_t *frag_len)
{
    uint32_t i;
    uint32_t hits = 0;
    uint32_t off1 = 0;
    uint32_t off2 = 0;
    uint32_t expected = 0;
    int      bad_len = 0;
    int      confirmed = 0;   /* embedded CALL header proven */
    int      k;
    uint32_t base2;         /* inferred start of the embedded message      */
    uint32_t xid_outer;
    uint32_t xid_inner;

    if (total < RPC_CALL_PROC_OFF + 4u) return 0;

    /* WRITE calls only -- see the note above on RENAME/LINK. */
    if (rpc_peek_uint32(buf, RPC_CALL_PROG_OFF) != RPC_PROG_NFS)        return 0;
    if (rpc_peek_uint32(buf, RPC_CALL_PROC_OFF) != RPC_NFS3_PROC_WRITE) return 0;

    /* Length invariant: a WRITE's own fields must account for exactly the
       bytes the record mark delivered.  Checked on every WRITE, because a
       mismatch is conclusive on its own even when no second handle shows. */
    if (rpc_write_expected_len(buf, total, &expected) == 0 && expected != total) {
        bad_len = 1;
        logmsg_error("NFSRP040E", "rpc_recv: WRITE LENGTH DESYNC -- record mark(s) delivered"
                  " %u bytes but the call's own fields account for %u"
                  " (%s by %u bytes)", total, expected,
                  (total > expected) ? "over" : "under",
                  (total > expected) ? (total - expected)
                                     : (expected - total));
    }

#ifdef QUICK_CHECK
    /* Quick check is that we look for the RPC NFS file handle at the two
     * locations they show up. First location is the correct place and second
     * location indicates corruption of receive.
     *
     * These are at offset 116 (correct)
     * and at offset 196, which is part of the receive bufer has been copied
     * a second time.
     */
    if ( total >= 200 ) {
        for (i = 116; i + 4 <= total;  i += 16) {
            if (buf[i]     == 0x4Eu && buf[i + 1] == 0x46u &&
                buf[i + 2] == 0x53u && buf[i + 3] == 0x33u) {
                if      (hits == 0) off1 = i;
                else if (hits == 1) off2 = i;
                hits++;
            }
        }
    }
#else
    /* Scan the WHOLE message, whatever its size.
     *
     * This was briefly limited to messages <= 8 KB, on the theory that a full
     * pass over every 64 KB WRITE might perturb timing enough to mask the bug.
     * That gate was a mistake twice over.  It was hedging against a masking
     * theory that has since been disproved -- the corruption reproduced with
     * all of this instrumentation in place -- and, far worse, it BLINDED the
     * check on exactly the writes that fail most often: a 64 KB WRITE is a
     * ~65,700-byte message, so the scan never ran, nothing was reported, and
     * the reject below never fired.  Corrupt data reached the PDS while the
     * detector sat silent.
     *
     * Cost is one pass over the payload; correctness of the safety net is
     * worth far more than the cycles. */
    for (i = 0; i + 4 <= total; i++) {
        if (buf[i]     == 0x4Eu && buf[i + 1] == 0x46u &&
            buf[i + 2] == 0x53u && buf[i + 3] == 0x33u) {
            if      (hits == 0) off1 = i;
            else if (hits == 1) off2 = i;
            hits++;
        }
    }
#endif

    /* normal: a WRITE has exactly one handle and a consistent length */
    if (hits <= 1 && !bad_len) return 0;

    logmsg_error("NFSRP050E", "rpc_recv: CORRUPT RECEIVE BUFFER -- WRITE call with 'NFS3'"
              " handle magic x%u (first two at off %u, %u) in a %u-byte"
              " message reassembled from %d fragment(s):",
              hits, off1, off2, total, nfrag);
    for (k = 0; k < nfrag && k < RPC_MAX_FRAGS; k++)
        logmsg_error("NFSRP060E", "rpc_recv:   frag %d: buf_off=%u len=%u",
                  k, frag_off[k], frag_len[k]);
    if (nfrag > RPC_MAX_FRAGS)
        logmsg_error("NFSRP070E", "rpc_recv:   ... %d fragments total, only first %d shown",
                  nfrag, RPC_MAX_FRAGS);

    /* Identify the embedded message by XID -- the field that says WHICH
       request bled in, and so which mechanism produced it:
         same XID      -> the SAME request captured twice (a client
                          retransmission, or our own code re-reading it),
         different XID -> a LATER request pulled in (framing desync: the
                          record mark over-declared and recv_all ran on into
                          the next message).
       off1 is the handle's offset within this (well-formed) message, so the
       embedded copy -- same client, hence same credential length -- should
       start at off2 - off1.  Verify that structurally before believing it:
       a genuine RPC CALL header has mtype==0, rpcvers==2, prog==100003. */
    if (off2 > off1) {
        base2 = off2 - off1;
        if (base2 + RPC_CALL_PROC_OFF + 4u <= total &&
            rpc_peek_uint32(buf, base2 + 4u)  == 0u &&
            rpc_peek_uint32(buf, base2 + 8u)  == 2u &&
            rpc_peek_uint32(buf, base2 + RPC_CALL_PROG_OFF) == RPC_PROG_NFS) {
            xid_outer = rpc_peek_uint32(buf, 0);
            xid_inner = rpc_peek_uint32(buf, base2);
            logmsg_error("NFSRP080E", "rpc_recv:   embedded RPC CALL confirmed at off %u:"
                      " xid=x%08X proc=%u  (this message xid=x%08X)",
                      base2, xid_inner,
                      rpc_peek_uint32(buf, base2 + RPC_CALL_PROC_OFF),
                      xid_outer);
            logmsg_error("NFSRP090E", "rpc_recv:   -> %s",
                      (xid_inner == xid_outer)
                        ? "SAME xid: this request captured TWICE"
                        : "DIFFERENT xid: a later request bled in"
                          " (framing desync)");
            confirmed = 1;      /* structurally proven, not a guess */
        } else {
            logmsg_error("NFSRP100E", "rpc_recv:   no RPC CALL header at the inferred"
                      " embedded start (off %u) -- the second handle is not"
                      " a whole message copied from its beginning", base2);
        }
    }

    /* Reject only on PROOF, never on suspicion.
     *
     * Two 'NFS3' magics alone is NOT enough: file data may legitimately
     * contain those four bytes, and dropping a connection for that would
     * corrupt a perfectly good transfer.  We reject only when the damage is
     * structurally certain -- either the length invariant failed, or a
     * complete RPC CALL header (mtype=0, rpcvers=2, prog=100003) was found
     * embedded inside the payload.  Neither can occur in valid data.
     *
     * Returning -1 makes handle_connection close the socket.  The stream is
     * unrecoverable at this point anyway: bytes were replayed, so whatever
     * follows is out of step with the record marks.  An NFS client treats a
     * dropped TCP connection as a transient error, reconnects and resends --
     * so the cost is a hiccup, and the benefit is that corrupt data can never
     * be written to a PDS member. */
    if (bad_len || confirmed) {
        logmsg_error("NFSRP110E", "rpc_recv: DROPPING CONNECTION -- refusing to process a"
                  " corrupt message (the client will reconnect and resend;"
                  " see spill_corruption_open: a partial recv() on this"
                  " socket layer can replay data from the start)");
        return -1;
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
    int      nfrag = 0;

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
        if (nfrag < RPC_MAX_FRAGS) {
            g_rpc_frag_off[nfrag] = total;
            g_rpc_frag_len[nfrag] = flen;
        }
        total += flen;
        nfrag++;
    } while (!last);

    *msglen = total;
    if (rpc_recv_selfcheck(buf, total, nfrag,
                           g_rpc_frag_off, g_rpc_frag_len) < 0)
        return -1;      /* corrupt: drop the connection */
    return 0;
}

/* ------------------------------------------------------------------ */
/* rpc_send: send buf as a single-fragment RPC record over fd.          */
/*                                                                      */
/* frame must point to a buffer whose first 4 bytes are reserved        */
/* headroom for the record mark, with the XDR response body at          */
/* frame[4..4+len-1].  rpc_send fills in the mark and then sends the   */
/* entire (4 + len) byte frame in a single send_all() call, so the     */
/* mark and body always travel together in one TCP write.               */
/*                                                                      */
/* The caller must initialise its XDR writer at frame+4 (not frame),   */
/* and pass frame (not frame+4) here.  See handle_connection() in       */
/* nfsd.c.                                                              */
/* ------------------------------------------------------------------ */
int rpc_send(int fd, uint8_t *frame, uint32_t len)
{
    uint32_t rm = 0x80000000u | len;

    frame[0] = (uint8_t)(rm >> 24);
    frame[1] = (uint8_t)(rm >> 16);
    frame[2] = (uint8_t)(rm >>  8);
    frame[3] = (uint8_t)(rm);

    return send_all(fd, frame, 4u + len);
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
