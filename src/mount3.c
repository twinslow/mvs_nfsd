/*
 * mount3.c - NFS MOUNT protocol v3 (RFC 1813 Appendix I), prog 100005.
 *
 * Handles:
 *   MOUNTPROC3_NULL   - no-op
 *   MOUNTPROC3_MNT    - validate export path, return root file handle
 *   MOUNTPROC3_DUMP   - return empty list (no mount tracking)
 *   MOUNTPROC3_UMNT   - accept and ignore
 *   MOUNTPROC3_UMNTALL- accept and ignore
 *   MOUNTPROC3_EXPORT - list all configured exports
 *
 * When a client calls MNT, we:
 *   1. Validate the requested NFS path against our exports table.
 *   2. stat() the local host_path to obtain dev/ino.
 *   3. Build a root file handle (relpath="") and insert it into cache.
 *   4. Return the handle with AUTH_UNIX as the only supported flavour.
 */

#include <string.h>   /* strlen */
#include <errno.h>
#include "nfsd.h"

/* ------------------------------------------------------------------ */
/* handle_mount: dispatch MOUNT protocol calls                          */
/* ------------------------------------------------------------------ */
void handle_mount(int fd, rpc_call_t *call, xdr_t *in, xdr_t *out)
{
    char      dirpath[MAX_PATH];
    export_t *exp;
    vfs_stat_t st;
    our_fhandle_t fh;
    uint32_t  exp_id;
    int       i;
    int       nexp;

    (void)fd;

    if (call->vers != VERS_MOUNT) {
        rpc_write_prog_mismatch(out, call->xid,
                                VERS_MOUNT, VERS_MOUNT);
        return;
    }

    switch (call->proc) {

    /* ---- NULL ---- */
    case MOUNTPROC3_NULL:
        rpc_write_accept_hdr(out, call->xid, RPC_SUCCESS);
        break;

    /* ---- MNT ---- */
    case MOUNTPROC3_MNT:
        /*
         * Args: dirpath (string)
         * Reply:
         *   status: uint32 (MNT3_OK=0 or error)
         *   if OK:
         *     fhandle: opaque (our 16-byte handle)
         *     auth_flavours: counted array of uint32
         */
        if (xdr_read_string(in, dirpath, MAX_PATH) < 0 || in->error) {
            rpc_write_accept_hdr(out, call->xid, RPC_SUCCESS);
            xdr_write_uint32(out, MNT3ERR_INVAL);
            break;
        }

        rpc_write_accept_hdr(out, call->xid, RPC_SUCCESS);

        exp = exports_find_by_nfs_path(dirpath);
        if (!exp) {
            xdr_write_uint32(out, MNT3ERR_NOENT);
            break;
        }

        /* The mount root is the export's virtual directory (which lists
           the PDS directories), so stat the export path itself. */
        if (vfs_stat(exp->export_path, &st) < 0) {
            xdr_write_uint32(out, MNT3ERR_IO);
            break;
        }

        if (st.ftype != NF3DIR) {
            xdr_write_uint32(out, MNT3ERR_NOTDIR);
            break;
        }

        exp_id = (uint32_t)exports_get_id(exp);
        fh_make(&fh, exp_id, st.raw_dev, st.raw_ino);

        /* Cache the root: relpath is empty string */
        fh_cache_insert(exp_id, st.raw_dev, st.raw_ino, "");

        xdr_write_uint32(out, MNT3_OK);
        xdr_write_fhandle(out, &fh);

        /* auth_flavours: one entry = AUTH_UNIX (1) */
        xdr_write_uint32(out, 1);            /* count */
        xdr_write_uint32(out, AUTH_FLAVOR_UNIX);
        break;

    /* ---- DUMP: return empty mount list ---- */
    case MOUNTPROC3_DUMP:
        rpc_write_accept_hdr(out, call->xid, RPC_SUCCESS);
        xdr_write_uint32(out, 0);  /* value_follows=0: empty list */
        break;

    /* ---- UMNT / UMNTALL: accept and ignore ---- */
    case MOUNTPROC3_UMNT:
        /* Args: dirpath string -- read and discard */
        xdr_read_string(in, dirpath, MAX_PATH);
        rpc_write_accept_hdr(out, call->xid, RPC_SUCCESS);
        break;

    case MOUNTPROC3_UMNTALL:
        rpc_write_accept_hdr(out, call->xid, RPC_SUCCESS);
        break;

    /* ---- EXPORT: list all configured exports ---- */
    case MOUNTPROC3_EXPORT:
        /*
         * Reply: linked list of exports, each:
         *   value_follows: uint32=1
         *   ex_dir:        string (NFS export path)
         *   ex_groups:     linked list of allowed groups (we say: empty)
         *
         * Terminated by value_follows=0.
         */
        rpc_write_accept_hdr(out, call->xid, RPC_SUCCESS);
        nexp = exports_count();
        for (i = 0; i < nexp; i++) {
            exp = exports_get(i);
            if (!exp) continue;
            xdr_write_uint32(out, 1u);  /* value_follows */
            xdr_write_string(out, exp->export_path,
                             (uint32_t)strlen(exp->export_path));
            xdr_write_uint32(out, 0u);  /* no groups: value_follows=0 */
        }
        xdr_write_uint32(out, 0u);      /* end of export list */
        break;

    default:
        rpc_write_proc_unavail(out, call->xid);
        break;
    }
}
