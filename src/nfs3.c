/*
 * nfs3.c - NFSv3 procedure implementations (RFC 1813).
 *
 * Implemented: NULL, GETATTR, SETATTR, LOOKUP, ACCESS, READ, WRITE,
 *              CREATE, REMOVE, RENAME, READDIR, READDIRPLUS, FSSTAT,
 *              FSINFO, PATHCONF, COMMIT.
 *
 * RENAME is restricted to renaming a member within its own PDS; a
 * cross-PDS (different directory) request returns NFS3ERR_XDEV.
 *
 * Not implemented (return NFS3ERR_NOTSUPP):
 *   READLINK, MKDIR, SYMLINK, MKNOD, RMDIR, LINK.
 *
 * Static read/write buffers are safe because the server is
 * single-threaded (one request in flight at a time).
 */

#include <string.h>   /* strlen, strncpy, strcmp, strrchr, memcpy */
#include <stdio.h>    /* snprintf */
#include <errno.h>
#include <time.h>     /* clock(), clock_t */
#include "nfsd.h"
#include "ebcdic.h"
#include "logger.h"
#include "mvsprf.h"

/* Static I/O buffers -- one READ and one WRITE buffer */
static uint8_t g_read_buf [MAX_READ_SIZE];
static uint8_t g_write_buf[MAX_WRITE_SIZE];

/*
 * Per-entry worst-case wire size for READDIRPLUS (excluding the name):
 *   value_follows(4) + fileid(8) + name_len_hdr(4) + name_padded(var) +
 *   cookie(8) + post_op_attr(4+84) + name_handle_present(4) +
 *   fh_opaque_len(4) + fh_data(OUR_FHSIZE)
 *
 * MUST track OUR_FHSIZE: this bounds the reply against the client's
 * maxcount, so under-estimating it overruns what the client asked for.
 * (It was previously hard-coded at 140, which silently became a 44-byte
 * per-entry under-estimate when the handle grew from 16 to 60 bytes.)
 */
#define READDIRPLUS_ENTRY_OVERHEAD (124u + (uint32_t)OUR_FHSIZE)

/* ================================================================== */
/* XDR helpers for NFS3 types                                          */
/* ================================================================== */

/*
 * xdr_write_fattr3: encode the 13-field fattr3 structure (84 bytes).
 * See RFC 1813 section 2.6.
 */
void xdr_write_fattr3(xdr_t *x, const vfs_stat_t *st)
{
    xdr_write_uint32(x, st->ftype);
    xdr_write_uint32(x, st->mode);
    xdr_write_uint32(x, st->nlink);
    xdr_write_uint32(x, st->uid);
    xdr_write_uint32(x, st->gid);
    xdr_write_uint64(x, st->size);
    xdr_write_uint64(x, st->used);
    xdr_write_uint32(x, st->rdev_maj);  /* specdata1 */
    xdr_write_uint32(x, st->rdev_min);  /* specdata2 */
    xdr_write_uint64(x, st->fsid);
    xdr_write_uint64(x, st->fileid);
    xdr_write_uint32(x, st->atime_sec);  xdr_write_uint32(x, st->atime_nsec);
    xdr_write_uint32(x, st->mtime_sec);  xdr_write_uint32(x, st->mtime_nsec);
    xdr_write_uint32(x, st->ctime_sec);  xdr_write_uint32(x, st->ctime_nsec);
}

/*
 * xdr_write_post_op_attr: optional fattr3.
 * If present!=0 and st!=NULL, writes 1 + fattr3; otherwise writes 0.
 */
void xdr_write_post_op_attr(xdr_t *x, const vfs_stat_t *st, int present)
{
    if (present && st) {
        xdr_write_uint32(x, 1u);
        xdr_write_fattr3(x, st);
    } else {
        xdr_write_uint32(x, 0u);
    }
}

/*
 * xdr_write_wcc_data: write pre-op wcc_attr (optional) followed by
 * post_op_attr.  wcc_attr contains only size + mtime + ctime.
 */
void xdr_write_wcc_data(xdr_t *x,
                         const vfs_stat_t *pre,  int has_pre,
                         const vfs_stat_t *post, int has_post)
{
    /* pre_op_attr: optional wcc_attr */
    if (has_pre && pre) {
        xdr_write_uint32(x, 1u);            /* present */
        xdr_write_uint64(x, pre->size);
        xdr_write_uint32(x, pre->mtime_sec);
        xdr_write_uint32(x, pre->mtime_nsec);
        xdr_write_uint32(x, pre->ctime_sec);
        xdr_write_uint32(x, pre->ctime_nsec);
    } else {
        xdr_write_uint32(x, 0u);
    }
    /* post_op_attr */
    xdr_write_post_op_attr(x, post, has_post);
}

/*
 * xdr_read_fhandle3: read an NFS3 file handle from the XDR stream,
 * decode it into *fh, and set *ok=1 on success or *ok=0 on error.
 */
void xdr_read_fhandle3(xdr_t *x, our_fhandle_t *fh, int *ok)
{
    uint8_t  bytes[NFS3_FHSIZE];
    uint32_t len = 0;

    xdr_read_opaque(x, bytes, &len, NFS3_FHSIZE);
    if (x->error || len != OUR_FHSIZE || fh_decode(bytes, len, fh) < 0) {
        *ok = 0;
        return;
    }
    *ok = 1;
}

/*
 * xdr_read_sattr3: decode an sattr3 from the XDR stream.
 * Each field is preceded by a 'check' uint32 (0=absent, 1=present).
 * For times, the set_it value is SET_DONT_CHANGE / SET_TO_SERVER_TIME /
 * SET_TO_CLIENT_TIME.
 */
void xdr_read_sattr3(xdr_t *x, sattr3_t *a)
{
    uint32_t check;

    memset(a, 0, sizeof(*a));

    check = xdr_read_uint32(x);
    a->has_mode = (int)check;
    if (check) a->mode = xdr_read_uint32(x);

    check = xdr_read_uint32(x);
    a->has_uid = (int)check;
    if (check) a->uid = xdr_read_uint32(x);

    check = xdr_read_uint32(x);
    a->has_gid = (int)check;
    if (check) a->gid = xdr_read_uint32(x);

    check = xdr_read_uint32(x);
    a->has_size = (int)check;
    if (check) a->size = xdr_read_uint64(x);

    /* atime: set_it (0/1/2), if 2: sec + nsec */
    a->set_atime = (int)xdr_read_uint32(x);
    if (a->set_atime == (int)SET_TO_CLIENT_TIME) {
        a->atime_sec  = xdr_read_uint32(x);
        a->atime_nsec = xdr_read_uint32(x);
    }

    /* mtime: same */
    a->set_mtime = (int)xdr_read_uint32(x);
    if (a->set_mtime == (int)SET_TO_CLIENT_TIME) {
        a->mtime_sec  = xdr_read_uint32(x);
        a->mtime_nsec = xdr_read_uint32(x);
    }
}

/* ================================================================== */
/* Procedure handlers                                                   */
/* ================================================================== */

/*
 * name_is_valid: return 1 if name is safe to use as a path component.
 *
 * Rejects empty names, "." (would alias the directory), ".." (path
 * traversal above the export root), and names containing '/' (which
 * would split into multiple components under snprintf).  RFC 1813
 * requires NFS3ERR_INVAL for names containing NUL or slash.
 */
static int name_is_valid(const char *name)
{
#ifdef __MVS__
    char xname[MAX_PATH];
    ascii_to_ebcdic(xname, name, sizeof(xname));
#else
    char *xname = name;
#endif

    if (xname[0] == '\0') return 0;
    if (xname[0] == '.' && xname[1] == '\0') return 0;
    if (xname[0] == '.' && xname[1] == '.' && xname[2] == '\0') return 0;
    if (strchr(xname, '/') != NULL) return 0;
    return 1;
}

/*
 * check_access: evaluate NFS3 ACCESS bits against POSIX mode, uid, gid.
 * Returns the subset of 'requested' that is actually permitted.
 *
 * Read-only enforcement: the write bits (MODIFY/EXTEND/DELETE) are stripped
 * for an object on a read-only export AFTER the permission logic -- including
 * after the root branch.  This is essential, not decorative: root (uid 0)
 * otherwise gets write access regardless of the mode, and a Linux client
 * mounting as root sends uid 0.  A read-only *filesystem* is not a permission
 * and no uid may override it (design §4.2 / §6.1).  Getting this wrong would
 * pass every Windows test (Windows sends uid -2) and be wide open on Linux.
 */
static uint32_t check_access(const vfs_stat_t *st, uint32_t uid,
                              uint32_t gid, uint32_t requested)
{
    uint32_t granted = 0;
    uint32_t mode    = st->mode;

    if (uid == 0) {
        /* Root: read/write/delete always; execute only if any x-bit set */
        granted = ACCESS3_READ | ACCESS3_LOOKUP | ACCESS3_MODIFY
                | ACCESS3_EXTEND | ACCESS3_DELETE;
        if (mode & 0111u) granted |= ACCESS3_EXECUTE;
    } else {
        /* Select the relevant rwx triplet */
        if      (uid == st->uid) mode >>= 6;
        else if (gid == st->gid) mode >>= 3;
        /* else: other bits are already in bits 2-0 */

        if (mode & 04u) granted |= ACCESS3_READ | ACCESS3_LOOKUP;
        if (mode & 02u) granted |= ACCESS3_MODIFY | ACCESS3_EXTEND | ACCESS3_DELETE;
        if (mode & 01u) granted |= ACCESS3_EXECUTE;
    }

    /* A read-only export never grants write, for any uid. */
    if (st->fs_readonly)
        granted &= ~(ACCESS3_MODIFY | ACCESS3_EXTEND | ACCESS3_DELETE);

    return granted & requested;
}

/* ------------------------------------------------------------------ */
/* NULL                                                                 */
/* ------------------------------------------------------------------ */
static void proc_null(xdr_t *out, uint32_t xid)
{
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* GETATTR                                                              */
/* ------------------------------------------------------------------ */
static void proc_getattr(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fh;
    int           ok;
    char          path[MAX_PATH];
    vfs_stat_t    st;

    xdr_read_fhandle3(in, &fh, &ok);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        logmsg_debug("NFSOP010D", "nfs3.proc_getattr: Returning error NFS3ERR_BADHANDLE");
        return;
    }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        logmsg_debug("NFSOP020D", "nfs3.proc_getattr: Returning error NFS3ERR_STALE");
        return;
    }

    logmsg_debug("NFSOP030D", "nfs3.proc_getattr: Starting for path %s",
        log_ascii(path));

    if (vfs_stat(path, &st) < 0) {
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno)); return;
    }

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_fattr3(out, &st);
}

/* ------------------------------------------------------------------ */
/* SETATTR                                                              */
/* ------------------------------------------------------------------ */
static void proc_setattr(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fh;
    sattr3_t      a;
    int           ok;
    uint32_t      guard_check;
    char          path[MAX_PATH];
    vfs_stat_t    pre, post;
    int           has_pre, has_post;
    uint32_t      status = NFS3_OK;

    xdr_read_fhandle3(in, &fh, &ok);
    xdr_read_sattr3(in, &a);
    guard_check = xdr_read_uint32(in);   /* sattrguard check */
    if (guard_check) xdr_skip(in, 8);   /* skip ctime if guard present */

    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }

    logmsg_debug("NFSOP040D", "nfs3.proc_setattr: Starting for path %s",
        log_ascii(path));

    has_pre  = (vfs_stat(path, &pre) == 0);

    /* mode, uid, gid changes are not supported; they are silently ignored.
     * Clients that call chmod/chown over NFS will get NFS3_OK but no
     * change on disk -- acceptable for a minimal read/write server. */
    if (a.has_size && vfs_truncate(path, a.size) < 0)
        status = vfs_errno_to_nfs3(errno);

    if (status == NFS3_OK &&
        (a.set_atime != (int)SET_DONT_CHANGE ||
         a.set_mtime != (int)SET_DONT_CHANGE)) {
        if (vfs_set_times(path,
                          a.set_atime, a.atime_sec, a.atime_nsec,
                          a.set_mtime, a.mtime_sec, a.mtime_nsec) < 0)
            status = vfs_errno_to_nfs3(errno);
    }

    has_post = (vfs_stat(path, &post) == 0);

    xdr_write_uint32(out, status);
    xdr_write_wcc_data(out, &pre, has_pre, &post, has_post);
}

/* ------------------------------------------------------------------ */
/* LOOKUP                                                               */
/* ------------------------------------------------------------------ */
static void proc_lookup(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t dir_fh, obj_fh;
    int           ok;
    char          name[MAX_NAME];
    char          dir_path[MAX_PATH];
    char          obj_path[MAX_PATH];
    vfs_stat_t    obj_st, dir_st;
    int           has_dir;

    xdr_read_fhandle3(in, &dir_fh, &ok);
    xdr_read_string(in, name, MAX_NAME);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok || in->error) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_post_op_attr(out, NULL, 0);
        logmsg_debug("NFSOP050D", "nfs3.proc_lookup: Returning error NFS3ERR_BADHANDLE");
        return;
    }
    if (!name_is_valid(name)) {
        xdr_write_uint32(out, NFS3ERR_INVAL);
        xdr_write_post_op_attr(out, NULL, 0);
        logmsg_debug("NFSOP060D", "nfs3.proc_lookup: Returning error NFS3ERR_INVAL");
        return;
    }
    if (fh_resolve(&dir_fh, dir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_post_op_attr(out, NULL, 0);
        logmsg_debug("NFSOP070D", "nfs3.proc_lookup: Returning error NFS3ERR_STALE");
        return;
    }

    snprintf(obj_path, MAX_PATH, "%s%c%s",
        dir_path, ebcdic_to_ascii_c('/'), name);

    logmsg_debug("NFSOP080D", "nfs3.proc_lookup: Starting for obj_path %s",
        log_ascii(obj_path));

    has_dir = (vfs_stat(dir_path, &dir_st) == 0);

    if (vfs_stat(obj_path, &obj_st) < 0) {
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno));
        xdr_write_post_op_attr(out, &dir_st, has_dir);
        return;
    }

    /* The handle names the object, so it needs no cache entry.  This can
       only fail if the path is outside every export, which cannot happen
       after the vfs_stat above succeeded. */
    if (fh_from_path(obj_path, &obj_fh) < 0) {
        xdr_write_uint32(out, NFS3ERR_SERVERFAULT);
        xdr_write_post_op_attr(out, &dir_st, has_dir);
        logmsg_warn("NFSOP090W", "nfs3.proc_lookup: cannot build handle for %s",
                 log_ascii(obj_path));
        return;
    }

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_fhandle(out, &obj_fh);
    xdr_write_post_op_attr(out, &obj_st,  1);
    xdr_write_post_op_attr(out, &dir_st, has_dir);
}

/* ------------------------------------------------------------------ */
/* ACCESS                                                               */
/* ------------------------------------------------------------------ */
static void proc_access(xdr_t *in, xdr_t *out, uint32_t xid,
                        uint32_t auth_uid, uint32_t auth_gid)
{
    our_fhandle_t fh;
    int           ok;
    uint32_t      requested;
    char          path[MAX_PATH];
    vfs_stat_t    st;
    int           has_st;
    uint32_t      granted;

    xdr_read_fhandle3(in, &fh, &ok);
    requested = xdr_read_uint32(in);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_post_op_attr(out, NULL, 0);
        xdr_write_uint32(out, 0);
        logmsg_debug("NFSOP100D", "Proc ACCESS returning error NFS3ERR_BADHANDLE");
        return;
    }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_post_op_attr(out, NULL, 0);
        xdr_write_uint32(out, 0);
        logmsg_debug("NFSOP110D", "Proc ACCESS returning error NFS3ERR_STALE");
        return;
    }

    logmsg_trace("NFSOP120T", "Proc ACCESS starting for path %s, requested 0x%08X, auth_uid=%d, auth_gid=%d",
        log_ascii(path), requested, auth_uid, auth_gid);

    has_st = (vfs_stat(path, &st) == 0);

    granted = has_st
                ? check_access(&st, auth_uid, auth_gid, requested)
                : 0u;

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &st, has_st);
    xdr_write_uint32(out, granted);

    logmsg_trace("NFSOP130D", "Proc ACCESS returned granted 0x%08X", granted);
}

/* ------------------------------------------------------------------ */
/* READ                                                                 */
/* ------------------------------------------------------------------ */
static void proc_read(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fh;
    int           ok;
    uint64_t      offset;
    uint32_t      count;
    char          path[MAX_PATH];
    vfs_stat_t    st;
    int           has_st;
    uint32_t      nread = 0;
    int           eof   = 0;

    xdr_read_fhandle3(in, &fh, &ok);
    offset = xdr_read_uint64(in);
    count  = xdr_read_uint32(in);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_post_op_attr(out, NULL, 0);
        logmsg_debug("NFSOP140D", "nfs3.proc_read: Returning error NFS3ERR_BADHANDLE");
        return;
    }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_post_op_attr(out, NULL, 0);
        logmsg_debug("NFSOP150D", "nfs3.proc_read: Returning error NFS3ERR_STALE");
        return;
    }

    if (count > MAX_READ_SIZE) count = MAX_READ_SIZE;

    logmsg_trace("NFSOP160T", "Proc READ starting %s, offset=%llu, count=%u",
        log_ascii(path), offset, count);

    has_st = (vfs_stat(path, &st) == 0);

    if (vfs_pread(path, g_read_buf, count, offset, &nread, &eof) < 0) {
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno));
        xdr_write_post_op_attr(out, &st, has_st);
        return;
    }

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &st, has_st);
    xdr_write_uint32(out, nread);
    xdr_write_uint32(out, (uint32_t)eof);
    xdr_write_opaque(out, g_read_buf, nread);
}

/* ------------------------------------------------------------------ */
/* WRITE                                                                */
/* ------------------------------------------------------------------ */
static void proc_write(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fh;
    int           ok;
    uint64_t      offset;
    uint32_t      count;
    uint32_t      data_len = 0;
    uint32_t      stable;
    uint32_t      committed;
    char          path[MAX_PATH];
    vfs_stat_t    pre, post;
    int           has_pre, has_post;
    uint32_t      status;

    xdr_read_fhandle3(in, &fh, &ok);
    offset = xdr_read_uint64(in);
    count  = xdr_read_uint32(in);
    stable = xdr_read_uint32(in);   /* stable_how requested by the client */
    xdr_read_opaque(in, g_write_buf, &data_len, MAX_WRITE_SIZE);

    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }

    logmsg_debug("NFSOP170D", "nfs3.proc_write: Starting for path %s with count %d and offset %llu",
        log_ascii(path), count, offset);

    has_pre  = (vfs_stat(path, &pre) == 0);
    if (data_len < count) count = data_len;

    if (vfs_pwrite(path, g_write_buf, count, offset) < 0) {
        status    = vfs_errno_to_nfs3(errno);
        committed = UNSTABLE;
    } else {
        status = NFS3_OK;
        /* The data is buffered in the pending-write pool and the member is
           STOWed exactly once -- at COMMIT, or the idle sweep -- NEVER on
           every write.  A PDS member rewrite writes new blocks at the end of
           the dataset and abandons the old ones, so re-stowing per write
           would fill the PDS with dead space and force frequent compresses.
           We echo the client's requested stability: an UNSTABLE client then
           issues COMMIT (our stow trigger); a FILE_SYNC client (e.g. Windows)
           is satisfied and does not delete the file.  Reads and stats of the
           not-yet-stowed member are served from the buffer (see vfs_pread /
           vfs_stat), so there is no visibility gap before the stow. */
        committed = stable;
    }

    has_post = (vfs_stat(path, &post) == 0);

    xdr_write_uint32(out, status);
    xdr_write_wcc_data(out, &pre, has_pre, &post, has_post);

    if (status == NFS3_OK) {
        xdr_write_uint32(out, count);
        xdr_write_uint32(out, committed);
        xdr_write_raw(out, g_write_verifier, 8);
    }
}

/* ------------------------------------------------------------------ */
/* CREATE                                                               */
/* ------------------------------------------------------------------ */
static void proc_create(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t dir_fh, obj_fh;
    int           ok;
    char          name[MAX_NAME];
    uint32_t      createmode;
    sattr3_t      a;
    char          dir_path[MAX_PATH];
    char          obj_path[MAX_PATH];
    vfs_stat_t    dir_pre, dir_post, obj_st;
    int           has_dir_pre, has_dir_post, has_obj;
    uint32_t      status;

    xdr_read_fhandle3(in, &dir_fh, &ok);
    xdr_read_string(in, name, MAX_NAME);
    createmode = xdr_read_uint32(in);

    memset(&a, 0, sizeof(a));
    if (createmode == CREATE_UNCHECKED || createmode == CREATE_GUARDED) {
        xdr_read_sattr3(in, &a);
    } else {
        /* EXCLUSIVE: skip 8-byte createverf (not supported) */
        xdr_skip(in, 8);
    }

    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok || in->error) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_uint32(out, 0);    /* no obj fh */
        xdr_write_post_op_attr(out, NULL, 0);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }
    if (!name_is_valid(name)) {
        xdr_write_uint32(out, NFS3ERR_INVAL);
        xdr_write_uint32(out, 0);
        xdr_write_post_op_attr(out, NULL, 0);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }
    if (fh_resolve(&dir_fh, dir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_uint32(out, 0);
        xdr_write_post_op_attr(out, NULL, 0);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }

    snprintf(obj_path, MAX_PATH, "%s%c%s",
        dir_path, ebcdic_to_ascii_c('/'), name);

        logmsg_debug("NFSOP180D", "nfs3.proc_create: Starting for path %s",
        log_ascii(obj_path));

    has_dir_pre = (vfs_stat(dir_path, &dir_pre) == 0);

    /* GUARDED: fail if the file already exists */
    if (createmode == CREATE_GUARDED) {
        vfs_stat_t tmp;
        if (vfs_stat(obj_path, &tmp) == 0) {
            has_dir_post = (vfs_stat(dir_path, &dir_post) == 0);
            xdr_write_uint32(out, NFS3ERR_EXIST);
            xdr_write_uint32(out, 0);
            xdr_write_post_op_attr(out, NULL, 0);
            xdr_write_wcc_data(out, &dir_pre, has_dir_pre,
                               &dir_post, has_dir_post);
            return;
        }
    }

    /* EXCLUSIVE mode: we do not store the verifier, so we treat it as
     * GUARDED (fail if file exists).  This is safe -- no existing file
     * is truncated -- but it means a retried EXCLUSIVE create returns
     * NFS3ERR_EXIST instead of the original handle.  Full RFC 1813
     * verifier semantics would require persisting the 8-byte createverf. */
    if (createmode == CREATE_EXCLUSIVE) {
        vfs_stat_t tmp;
        if (vfs_stat(obj_path, &tmp) == 0) {
            has_dir_post = (vfs_stat(dir_path, &dir_post) == 0);
            xdr_write_uint32(out, NFS3ERR_EXIST);
            xdr_write_uint32(out, 0);
            xdr_write_post_op_attr(out, NULL, 0);
            xdr_write_wcc_data(out, &dir_pre, has_dir_pre,
                               &dir_post, has_dir_post);
            return;
        }
    }

    {
        uint32_t mode = a.has_mode ? a.mode : 0644u;
        status = (vfs_create(obj_path, mode) < 0)
                     ? vfs_errno_to_nfs3(errno) : NFS3_OK;
    }

    has_dir_post = (vfs_stat(dir_path, &dir_post) == 0);

    if (status != NFS3_OK) {
        xdr_write_uint32(out, status);
        xdr_write_uint32(out, 0);
        xdr_write_post_op_attr(out, NULL, 0);
        xdr_write_wcc_data(out, &dir_pre, has_dir_pre,
                           &dir_post, has_dir_post);
        return;
    }

    has_obj = (vfs_stat(obj_path, &obj_st) == 0);
    if (has_obj && fh_from_path(obj_path, &obj_fh) < 0)
        has_obj = 0;   /* cannot name it -> just omit the optional handle */

    xdr_write_uint32(out, NFS3_OK);
    if (has_obj) {
        xdr_write_uint32(out, 1u);       /* obj fh present */
        xdr_write_fhandle(out, &obj_fh);
    } else {
        xdr_write_uint32(out, 0u);
    }
    xdr_write_post_op_attr(out, &obj_st, has_obj);
    xdr_write_wcc_data(out, &dir_pre, has_dir_pre, &dir_post, has_dir_post);
}

/* ------------------------------------------------------------------ */
/* REMOVE                                                               */
/* ------------------------------------------------------------------ */
static void proc_remove(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t dir_fh;
    int           ok;
    char          name[MAX_NAME];
    char          dir_path[MAX_PATH];
    char          obj_path[MAX_PATH];
    vfs_stat_t    dir_pre, dir_post;
    int           has_pre, has_post;
    uint32_t      status;

    xdr_read_fhandle3(in, &dir_fh, &ok);
    xdr_read_string(in, name, MAX_NAME);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }
    if (!name_is_valid(name)) {
        xdr_write_uint32(out, NFS3ERR_INVAL);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }
    if (fh_resolve(&dir_fh, dir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }

    snprintf(obj_path, MAX_PATH, "%s%c%s",
        dir_path, ebcdic_to_ascii_c('/'), name);

    logmsg_debug("NFSOP190D", "nfs3.proc_remove: Starting for path %s",
        log_ascii(obj_path));

    has_pre = (vfs_stat(dir_path, &dir_pre) == 0);

    status = (vfs_remove(obj_path) < 0)
                 ? vfs_errno_to_nfs3(errno) : NFS3_OK;

    has_post = (vfs_stat(dir_path, &dir_post) == 0);

    xdr_write_uint32(out, status);
    xdr_write_wcc_data(out, &dir_pre, has_pre, &dir_post, has_post);
}

/* ------------------------------------------------------------------ */
/* READDIR                                                            */
/* ------------------------------------------------------------------ */
static void proc_readdir(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t dir_fh;
    int           ok;
    uint64_t      cookie;
    uint32_t      maxcount;
    char          dir_path[MAX_PATH];
    vfs_stat_t    dir_st;
    int           has_dir;
    vfs_dir_t    *dp;
    char          ename[MAX_NAME];
    uint64_t      efileid;
    uint64_t      ecookie;
    uint32_t      nfs3_start;
    uint32_t      before;
    uint32_t      name_len;
    uint32_t      entry_sz;
    int           eof;
    int           wrote_one;
    static const uint8_t zero8[8] = { 0 };

    xdr_read_fhandle3(in, &dir_fh, &ok);
    cookie   = xdr_read_uint64(in);
    xdr_skip(in, 8);              /* cookieverf: ignored */
    maxcount = xdr_read_uint32(in);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_post_op_attr(out, NULL, 0);
        return;
    }
    if (fh_resolve(&dir_fh, dir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_post_op_attr(out, NULL, 0);
        return;
    }

    logmsg_debug("NFSOP200D", "nfs3.proc_readdir: Starting for path %s, cookie = '%-8.8s' (0x%016llX)",
        log_ascii(dir_path), (char *)&cookie, cookie);

    has_dir = (vfs_stat(dir_path, &dir_st) == 0);

    dp = vfs_opendir(dir_path, cookie);
    if (!dp) {
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno));
        xdr_write_post_op_attr(out, &dir_st, has_dir);
        return;
    }

    nfs3_start = xdr_get_pos(out);     /* NFS3 payload starts here (status field) */
    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &dir_st, has_dir);
    xdr_write_raw(out, zero8, 8);   /* cookieverf */

    /* vfs_seekdir_to(dp, cookie);  -- now called from vfs_opendir if appropriate. */
    eof = 1; wrote_one = 0;
    ecookie = cookie;

    while (vfs_readdir_next(dp, ename, MAX_NAME, &efileid, &ecookie) == 0) {
        name_len = (uint32_t)strlen(ename);
        /* Estimate: value_follows(4) + fileid(8) + name_len_hdr(4) +
           name_padded + cookie(8) */
        entry_sz = 4u + 8u + 4u + ((name_len + 3u) & ~3u) + 8u;

        before = xdr_get_pos(out);
        /* Compare total NFS3 payload (from status to end) against maxcount.
           +8u accounts for the end-of-list(4) + eof(4) trailer. */
        if ((before - nfs3_start) + entry_sz + 8u > maxcount
            && wrote_one) {
            eof = 0;
            break;
        }

        xdr_write_uint32(out, 1u);           /* value_follows */
        xdr_write_uint64(out, efileid);
        xdr_write_string(out, ename, name_len);
        xdr_write_uint64(out, ecookie);
        wrote_one = 1;
    }

    xdr_write_uint32(out, 0u);              /* end of list */
    xdr_write_uint32(out, (uint32_t)eof);
    vfs_closedir(dp);

    logmsg_debug("NFSOP210D", "nfs3.proc_readdir: Ending for path %s, wrote_one=%d eof=%d",
       log_ascii(dir_path), wrote_one, eof);

}

/* ------------------------------------------------------------------ */
/* READDIRPLUS                                                        */
/* ------------------------------------------------------------------ */
static void proc_readdirplus(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t dir_fh, efh;
    int           ok;
    uint64_t      cookie;
    uint32_t      maxcount;
    char          dir_path[MAX_PATH];
    char          entry_path[MAX_PATH];
    vfs_stat_t    dir_st, est;
    int           has_dir, has_est;
    vfs_dir_t    *dp;
    char          ename[MAX_NAME];
    uint64_t      efileid;
    uint64_t      ecookie, lrcookie;
    uint32_t      nfs3_start;
    uint32_t      before;
    uint32_t      name_len;
    int           eof;
    int           wrote_one;
    int           entry_count;
    static const uint8_t zero8[8] = { 0 };

    xdr_read_fhandle3(in, &dir_fh, &ok);
    cookie   = xdr_read_uint64(in);
    xdr_skip(in, 8);              /* cookieverf: ignored */
    xdr_skip(in, 4);              /* dircount: ignored (we use maxcount) */
    maxcount = xdr_read_uint32(in);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_post_op_attr(out, NULL, 0);
        return;
    }
    if (fh_resolve(&dir_fh, dir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_post_op_attr(out, NULL, 0);
        return;
    }

    logmsg_debug("NFSOP220D", "nfs3.proc_readdirplus: Starting for path %s, cookie = '%-8.8s' (0x%016llX)",
        log_ascii(dir_path), (char *)&cookie, cookie);

    has_dir = (vfs_stat(dir_path, &dir_st) == 0);

    dp = vfs_opendir(dir_path, cookie);
    if (!dp) {
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno));
        xdr_write_post_op_attr(out, &dir_st, has_dir);
        return;
    }

    nfs3_start = xdr_get_pos(out);     /* NFS3 payload starts here (status field) */
    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &dir_st, has_dir);
    xdr_write_raw(out, zero8, 8);      /* cookieverf */

    /* vfs_seekdir_to(dp, cookie); -- now called as part of vfs_opendir */
    eof = 1; wrote_one = 0; entry_count = 0;
    ecookie = cookie; lrcookie = 0;

    while (vfs_readdir_next(dp, ename, MAX_NAME, &efileid, &ecookie) == 0) {
        snprintf(entry_path, MAX_PATH, "%s%c%s",
            dir_path, ebcdic_to_ascii_c('/'), ename);
        has_est = (vfs_stat(entry_path, &est) == 0);
        if (!has_est) continue;   /* skip unstat-able entries */

        name_len = (uint32_t)strlen(ename);
        /* Worst-case size: vf(4)+fileid(8)+name+cookie(8)+
           post_op_attr(4+84)+post_op_fh(4+4+OUR_FHSIZE+pad) */
        before = xdr_get_pos(out);

        /* Compare total NFS3 payload (from status to end) against maxcount.
           +8u accounts for the end-of-list(4) + eof(4) trailer. */
        if ((before - nfs3_start) + READDIRPLUS_ENTRY_OVERHEAD + name_len + 8u > maxcount
            && wrote_one) {
            eof = 0;
            break;
        }

        /* The handle names the entry, so there is nothing to cache.  An
           entry we cannot name is skipped rather than sent with a handle
           the client could not use. */
        if (fh_from_path(entry_path, &efh) < 0) {
            logmsg_warn("NFSOP230W", "nfs3.proc_readdirplus: skipping unnameable entry %s",
                     log_ascii(entry_path));
            continue;
        }

        xdr_write_uint32(out, 1u);           /* value_follows */
        xdr_write_uint64(out, est.fileid);
        xdr_write_string(out, ename, name_len);
        xdr_write_uint64(out, ecookie);
        lrcookie = ecookie;
        xdr_write_post_op_attr(out, &est, 1);
        xdr_write_uint32(out, 1u);           /* name_handle: present */
        xdr_write_fhandle(out, &efh);
        wrote_one = 1;
    }

    xdr_write_uint32(out, 0u);              /* end of entry list */
    xdr_write_uint32(out, (uint32_t)eof);
    vfs_closedir(dp);

    logmsg_trace("NFSOP240T", "Proc READDIRPLUS the last response entry cookie was = '%-8.8s' (0x%016llX)",
        (char *)&lrcookie, lrcookie);
    logmsg_trace("NFSOP250I", "Proc READDIRPLUS ending for path %s, wrote_one=%d eof=%d xdr_bytes=%u",
       log_ascii(dir_path), wrote_one, eof, xdr_get_pos(out));

}

/* ------------------------------------------------------------------ */
/* FSSTAT                                                               */
/* ------------------------------------------------------------------ */
static void proc_fsstat(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fh;
    int           ok;
    char          path[MAX_PATH];
    vfs_stat_t    st;
    vfs_fsstat_t  fs;
    int           has_st;

    xdr_read_fhandle3(in, &fh, &ok);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) { xdr_write_uint32(out, NFS3ERR_BADHANDLE); return; }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE); return;
    }

    logmsg_debug("NFSOP260D", "nfs3.proc_fsstat: Starting for path %s",
        log_ascii(path));

    has_st = (vfs_stat(path, &st) == 0);

    if (vfs_fsstat(path, &fs) < 0) {
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno));
        xdr_write_post_op_attr(out, &st, has_st);
        return;
    }

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &st, has_st);
    xdr_write_uint64(out, fs.total_bytes);
    xdr_write_uint64(out, fs.free_bytes);
    xdr_write_uint64(out, fs.avail_bytes);
    xdr_write_uint64(out, fs.total_files);
    xdr_write_uint64(out, fs.free_files);
    xdr_write_uint64(out, fs.avail_files);
    xdr_write_uint32(out, fs.invarsec);
}

/* ------------------------------------------------------------------ */
/* FSINFO                                                               */
/* ------------------------------------------------------------------ */
static void proc_fsinfo(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fh;
    int           ok;
    char          path[MAX_PATH];
    vfs_stat_t    st;
    int           has_st;

    xdr_read_fhandle3(in, &fh, &ok);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) { xdr_write_uint32(out, NFS3ERR_BADHANDLE); return; }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE); return;
    }

    logmsg_debug("NFSOP270D", "nfs3.proc_fsinfo: Starting for path %s",
        log_ascii(path));

    has_st = (vfs_stat(path, &st) == 0);

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &st, has_st);
    xdr_write_uint32(out, MAX_READ_SIZE);    /* rtmax   */
    xdr_write_uint32(out, MAX_READ_SIZE);    /* rtpref  */
    xdr_write_uint32(out, 512u);             /* rtmult  */
    xdr_write_uint32(out, MAX_WRITE_SIZE);   /* wtmax   */
    xdr_write_uint32(out, MAX_WRITE_SIZE);   /* wtpref  */
    xdr_write_uint32(out, 512u);             /* wtmult  */
    xdr_write_uint32(out, 8192u);            /* dtpref  */
    xdr_write_uint64(out, 0xFFFFFFFFu);      /* maxfilesize */
    xdr_write_uint32(out, 0u);               /* time_delta sec  */
    xdr_write_uint32(out, 1000000u);         /* time_delta nsec (1ms) */
    xdr_write_uint32(out, FSF3_HOMOGENEOUS | FSF3_CANSETTIME);
}

/* ------------------------------------------------------------------ */
/* PATHCONF                                                             */
/* ------------------------------------------------------------------ */
static void proc_pathconf(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fh;
    int           ok;
    char          path[MAX_PATH];
    vfs_stat_t    st;
    int           has_st;

    xdr_read_fhandle3(in, &fh, &ok);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) { xdr_write_uint32(out, NFS3ERR_BADHANDLE); return; }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE); return;
    }

    logmsg_debug("NFSOP280D", "nfs3.proc_pathconf: Starting for path %s",
        log_ascii(path));

    has_st = (vfs_stat(path, &st) == 0);

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &st, has_st);
    xdr_write_uint32(out, 1u);     /* linkmax: we don't support links */
    xdr_write_uint32(out, 255u);   /* name_max */
    xdr_write_uint32(out, 1u);     /* no_trunc: names not truncated   */
    xdr_write_uint32(out, 0u);     /* chown_restricted: no            */
    xdr_write_uint32(out, 0u);     /* case_insensitive: no            */
    xdr_write_uint32(out, 1u);     /* case_preserving: yes            */
}

/* ------------------------------------------------------------------ */
/* COMMIT                                                               */
/* ------------------------------------------------------------------ */
static void proc_commit(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fh;
    int           ok;
    char          path[MAX_PATH];
    vfs_stat_t    pre, post;
    int           has_pre, has_post;

    xdr_read_fhandle3(in, &fh, &ok);
    xdr_skip(in, 8);   /* offset */
    xdr_skip(in, 4);   /* count  */
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }

    logmsg_debug("NFSOP290D", "nfs3.proc_commit: Starting for path %s",
        log_ascii(path));

    has_pre = (vfs_stat(path, &pre) == 0);

    /* Flush the buffered member to the PDS (STOW). */
    if (vfs_commit(path) < 0) {
        has_post = (vfs_stat(path, &post) == 0);
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno));
        xdr_write_wcc_data(out, &pre, has_pre,
                           has_post ? &post : NULL, has_post);
        return;
    }

    has_post = (vfs_stat(path, &post) == 0);

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_wcc_data(out, &pre, has_pre, &post, has_post);
    xdr_write_raw(out, g_write_verifier, 8);
}

/* ------------------------------------------------------------------ */
/* RENAME                                                               */
/* ------------------------------------------------------------------ */
static void proc_rename(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fdir, tdir;
    int           ok1, ok2;
    char          fname[MAX_NAME], tname[MAX_NAME];
    char          fdir_path[MAX_PATH], tdir_path[MAX_PATH];
    char          from_path[MAX_PATH], to_path[MAX_PATH];
    vfs_stat_t    fpre, fpost, tpre, tpost;
    int           has_fpre, has_fpost, has_tpre, has_tpost;
    uint32_t      status;

    xdr_read_fhandle3(in, &fdir, &ok1);
    xdr_read_string(in, fname, MAX_NAME);
    xdr_read_fhandle3(in, &tdir, &ok2);
    xdr_read_string(in, tname, MAX_NAME);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok1 || !ok2) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }
    if (!name_is_valid(fname) || !name_is_valid(tname)) {
        xdr_write_uint32(out, NFS3ERR_INVAL);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }
    if (fh_resolve(&fdir, fdir_path, MAX_PATH) < 0 ||
        fh_resolve(&tdir, tdir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }

    snprintf(from_path, MAX_PATH, "%s%c%s",
        fdir_path, ebcdic_to_ascii_c('/'), fname);
    snprintf(to_path,   MAX_PATH, "%s%c%s",
        tdir_path, ebcdic_to_ascii_c('/'), tname);

    logmsg_debug("NFSOP300D", "nfs3.proc_rename: Starting for from_path %s to_path %s",
        log_ascii(from_path), log_ascii(to_path));

    has_fpre = (vfs_stat(fdir_path, &fpre) == 0);
    has_tpre = (vfs_stat(tdir_path, &tpre) == 0);

    status = (vfs_rename(from_path, to_path) < 0)
                 ? vfs_errno_to_nfs3(errno) : NFS3_OK;

    has_fpost = (vfs_stat(fdir_path, &fpost) == 0);
    has_tpost = (vfs_stat(tdir_path, &tpost) == 0);

    /* No path-cache fixup is needed: a handle names its object, so the
       renamed member's handle is simply the one derived from its new path.
       A handle held for the OLD name now names an object that no longer
       exists -- fh_resolve still yields a path, vfs_stat fails, and the
       client correctly gets a stale/absent answer for that name only. */

    xdr_write_uint32(out, status);
    xdr_write_wcc_data(out, &fpre, has_fpre, &fpost, has_fpost);
    xdr_write_wcc_data(out, &tpre, has_tpre, &tpost, has_tpost);
}

/* ------------------------------------------------------------------ */
/* NOTSUPP stub: emit NFS3ERR_NOTSUPP with a null post_op_attr          */
/* ------------------------------------------------------------------ */
static void proc_notsupp(xdr_t *out, uint32_t xid)
{
    logmsg_debug("NFSOP310D", "nfs3.proc_notsupp: Starting");

    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);
    xdr_write_uint32(out, NFS3ERR_NOTSUPP);
    xdr_write_post_op_attr(out, NULL, 0);   /* best-effort WCC/attr */
}

/* ================================================================== */
/* Dispatch                                                             */
/* ================================================================== */

static const char *proc_name(uint32_t proc)
{
    switch (proc) {
    case NFS3PROC_NULL:        return "NULL";
    case NFS3PROC_GETATTR:     return "GETATTR";
    case NFS3PROC_SETATTR:     return "SETATTR";
    case NFS3PROC_LOOKUP:      return "LOOKUP";
    case NFS3PROC_ACCESS:      return "ACCESS";
    case NFS3PROC_READLINK:    return "READLINK";
    case NFS3PROC_READ:        return "READ";
    case NFS3PROC_WRITE:       return "WRITE";
    case NFS3PROC_CREATE:      return "CREATE";
    case NFS3PROC_MKDIR:       return "MKDIR";
    case NFS3PROC_SYMLINK:     return "SYMLINK";
    case NFS3PROC_MKNOD:       return "MKNOD";
    case NFS3PROC_REMOVE:      return "REMOVE";
    case NFS3PROC_RMDIR:       return "RMDIR";
    case NFS3PROC_RENAME:      return "RENAME";
    case NFS3PROC_LINK:        return "LINK";
    case NFS3PROC_READDIR:     return "READDIR";
    case NFS3PROC_READDIRPLUS: return "READDIRPLUS";
    case NFS3PROC_FSSTAT:      return "FSSTAT";
    case NFS3PROC_FSINFO:      return "FSINFO";
    case NFS3PROC_PATHCONF:    return "PATHCONF";
    case NFS3PROC_COMMIT:      return "COMMIT";
    default:                   return "?";
    }
}

void handle_nfs3(int fd, rpc_call_t *call, xdr_t *in, xdr_t *out)
{
    clock_t     t_start;
    clock_t     t_elapsed;
    int         perf_slot;
    int         log_slot;
    log_level_t saved_level;

    (void)fd;

    if (g_verbose)
        fprintf(stderr, "nfs3: xid=%08X proc=%s uid=%u\n",
                call->xid, proc_name(call->proc), call->auth_uid);

    if (call->vers != VERS_NFS) {
        rpc_write_prog_mismatch(out, call->xid, VERS_NFS, VERS_NFS);
        return;
    }

    /* Map NFS3 procedure number to performance slot and log slot. */
    switch (call->proc) {
    case NFS3PROC_GETATTR:     perf_slot = PERF_NFS3_GETATTR;  log_slot = LOG_PROC_GETATTR;     break;
    case NFS3PROC_SETATTR:     perf_slot = PERF_NFS3_SETATTR;  log_slot = LOG_PROC_SETATTR;     break;
    case NFS3PROC_LOOKUP:      perf_slot = PERF_NFS3_LOOKUP;   log_slot = LOG_PROC_LOOKUP;      break;
    case NFS3PROC_ACCESS:      perf_slot = PERF_NFS3_ACCESS;   log_slot = LOG_PROC_ACCESS;      break;
    case NFS3PROC_READ:        perf_slot = PERF_NFS3_READ;     log_slot = LOG_PROC_READ;        break;
    case NFS3PROC_WRITE:       perf_slot = PERF_NFS3_WRITE;    log_slot = LOG_PROC_WRITE;       break;
    case NFS3PROC_CREATE:      perf_slot = PERF_NFS3_CREATE;   log_slot = LOG_PROC_CREATE;      break;
    case NFS3PROC_REMOVE:      perf_slot = PERF_NFS3_REMOVE;   log_slot = LOG_PROC_REMOVE;      break;
    case NFS3PROC_RENAME:      perf_slot = PERF_NFS3_RENAME;   log_slot = LOG_PROC_RENAME;      break;
    case NFS3PROC_READDIR:     perf_slot = PERF_NFS3_READDIR;  log_slot = LOG_PROC_READDIR;     break;
    case NFS3PROC_READDIRPLUS: perf_slot = PERF_NFS3_RDIRPLUS; log_slot = LOG_PROC_READDIRPLUS; break;
    case NFS3PROC_FSSTAT:      perf_slot = PERF_NFS3_FSSTAT;   log_slot = LOG_PROC_FSSTAT;      break;
    case NFS3PROC_FSINFO:      perf_slot = PERF_NFS3_FSINFO;   log_slot = LOG_PROC_FSINFO;      break;
    case NFS3PROC_PATHCONF:    perf_slot = PERF_NFS3_PATHCONF; log_slot = LOG_PROC_PATHCONF;    break;
    case NFS3PROC_COMMIT:      perf_slot = PERF_NFS3_COMMIT;   log_slot = LOG_PROC_COMMIT;      break;
    case NFS3PROC_NULL:        perf_slot = -1;                 log_slot = LOG_PROC_NULL;        break;
    default:                   perf_slot = -1;                 log_slot = -1;                   break;
    }

    /* Install this procedure's effective log level for the duration of the
       call, then restore the global level on the way out.  Out-of-range
       (unmapped) procedures resolve to the global level, so the save/restore
       is a harmless no-op for them. */
    saved_level = log_get_level();
    log_set_level(log_proc_get_level(log_slot));

    t_start = clock();

    switch (call->proc) {
    case NFS3PROC_NULL:        proc_null(out, call->xid);            break;
    case NFS3PROC_GETATTR:     proc_getattr(in, out, call->xid);     break;
    case NFS3PROC_SETATTR:     proc_setattr(in, out, call->xid);     break;
    case NFS3PROC_LOOKUP:      proc_lookup(in, out, call->xid);      break;
    case NFS3PROC_ACCESS:
        proc_access(in, out, call->xid, call->auth_uid, call->auth_gid);
        break;
    case NFS3PROC_READ:        proc_read(in, out, call->xid);        break;
    case NFS3PROC_WRITE:       proc_write(in, out, call->xid);       break;
    case NFS3PROC_CREATE:      proc_create(in, out, call->xid);      break;
    case NFS3PROC_REMOVE:      proc_remove(in, out, call->xid);      break;
    case NFS3PROC_RENAME:      proc_rename(in, out, call->xid);      break;
    case NFS3PROC_READDIR:     proc_readdir(in, out, call->xid);     break;
    case NFS3PROC_READDIRPLUS: proc_readdirplus(in, out, call->xid); break;
    case NFS3PROC_FSSTAT:      proc_fsstat(in, out, call->xid);      break;
    case NFS3PROC_FSINFO:      proc_fsinfo(in, out, call->xid);      break;
    case NFS3PROC_PATHCONF:    proc_pathconf(in, out, call->xid);    break;
    case NFS3PROC_COMMIT:      proc_commit(in, out, call->xid);      break;
    default:                   proc_notsupp(out, call->xid);         break;
    }

    t_elapsed = clock() - t_start;
    if (perf_slot >= 0)
        mvsprf_record(perf_slot, t_elapsed);

    /* Restore the global level regardless of which procedure ran. */
    log_set_level(saved_level);
}
