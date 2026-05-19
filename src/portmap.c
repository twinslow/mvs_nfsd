/*
 * portmap.c - ONC/RPC Portmapper protocol (RFC 1833), program 100000 v2.
 *
 * Handles PMAPPROC_NULL and PMAPPROC_GETPORT.
 * All other procedures return port=0 (not registered).
 *
 * We respond to GETPORT with fixed port numbers:
 *   NFS  (prog=100003, vers=3) -> PORT_NFS   (2049)
 *   MOUNT(prog=100005, vers=3) -> PORT_MOUNT (20048)
 *   PMAP (prog=100000, vers=2) -> PORT_PORTMAP (111)
 *
 * Note: To use this portmapper, the system rpcbind/portmap daemon
 * must NOT be running (it already holds port 111).
 * Stop it with: sudo systemctl stop rpcbind  (or equivalent).
 *
 * Clients that specify explicit ports can skip portmapper entirely:
 *   mount -t nfs -o nfsvers=3,port=2049,mountport=20048,nolock \
 *         server:/export/foo /mnt/foo
 */

#include "nfsd.h"

/* ------------------------------------------------------------------ */
/* handle_portmap: dispatch portmapper calls                            */
/* ------------------------------------------------------------------ */
void handle_portmap(int fd, rpc_call_t *call, xdr_t *in, xdr_t *out)
{
    uint32_t prog;
    uint32_t vers;
    uint32_t prot;
    uint32_t port;

    (void)fd;   /* not used; reply is written into out */

    /* Check program version */
    if (call->vers != VERS_PORTMAP) {
        rpc_write_prog_mismatch(out, call->xid,
                                VERS_PORTMAP, VERS_PORTMAP);
        return;
    }

    switch (call->proc) {

    case PMAPPROC_NULL:
        /* NULL procedure: accept and return empty reply */
        rpc_write_accept_hdr(out, call->xid, RPC_SUCCESS);
        break;

    case PMAPPROC_GETPORT:
        /*
         * GETPORT args: prog(uint32), vers(uint32),
         *               prot(uint32: 6=TCP, 17=UDP), port(uint32 ignored)
         */
        prog = xdr_read_uint32(in);
        vers = xdr_read_uint32(in);
        prot = xdr_read_uint32(in);
        (void)xdr_read_uint32(in);  /* port (ignored in query) */

        port = 0;   /* default: not registered */

        if (prot == IPPROTO_TCP_PMAP) {
            if (prog == PROG_NFS   && vers == VERS_NFS)
                port = (uint32_t)g_port_nfs;
            else if (prog == PROG_MOUNT && vers == VERS_MOUNT)
                port = (uint32_t)g_port_mount;
            else if (prog == PROG_PORTMAP && vers == VERS_PORTMAP)
                port = (uint32_t)g_port_pmap;
        }

        rpc_write_accept_hdr(out, call->xid, RPC_SUCCESS);
        xdr_write_uint32(out, port);
        break;

    default:
        rpc_write_proc_unavail(out, call->xid);
        break;
    }
}
