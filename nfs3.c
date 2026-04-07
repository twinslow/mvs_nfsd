/*
 * nfs3.c - NFSv3 procedure implementations (RFC 1813).
 *
 * Implemented: NULL, GETATTR, SETATTR, LOOKUP, ACCESS, READ, WRITE,
 *              CREATE, REMOVE, READDIR, READDIRPLUS, FSSTAT, FSINFO,
 *              PATHCONF, COMMIT.
 *
 * Not implemented (return NFS3ERR_NOTSUPP):
 *   READLINK, MKDIR, SYMLINK, MKNOD, RMDIR, RENAME, LINK.
 *
 * Static read/write buffers are safe because the server is
 * single-threaded (one request in flight at a time).
 */

#include <string.h>   /* strlen, strncpy, strcmp, strrchr, memcpy */
#include <stdio.h>    /* snprintf */
#include <errno.h>
#include "nfsd.h"

/* Static I/O buffers -- one READ and one WRITE buffer */
static uint8_t g_read_buf [MAX_READ_SIZE];
static uint8_t g_write_buf[MAX_WRITE_SIZE];

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
        a->atime_sec = xdr_read_uint32(x);
        xdr_skip(x, 4);  /* nsec */
    }

    /* mtime: same */
    a->set_mtime = (int)xdr_read_uint32(x);
    if (a->set_mtime == (int)SET_TO_CLIENT_TIME) {
        a->mtime_sec = xdr_read_uint32(x);
        xdr_skip(x, 4);  /* nsec */
    }
}

/* ================================================================== */
/* Procedure handlers                                                   */
/* ================================================================== */

/* Helper: build the relative path of a child given its parent relpath */
static void make_child_relpath(const char *parent_rel, const char *name,
                                char *child_rel, size_t maxlen)
{
    if (parent_rel[0] == '\0') {
        strncpy(child_rel, name, maxlen - 1);
        child_rel[maxlen - 1] = '\0';
    } else {
        snprintf(child_rel, maxlen, "%s/%s", parent_rel, name);
    }
}

/* Helper: strip last component from relpath to get parent relpath */
static void parent_relpath(const char *relpath, char *parent, size_t maxlen)
{
    const char *slash = strrchr(relpath, '/');
    if (!slash) {
        parent[0] = '\0';   /* parent is export root */
    } else {
        size_t len = (size_t)(slash - relpath);
        if (len >= maxlen) len = maxlen - 1;
        memcpy(parent, relpath, len);
        parent[len] = '\0';
    }
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

    if (!ok) { xdr_write_uint32(out, NFS3ERR_BADHANDLE); return; }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE); return;
    }
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

    has_pre  = (vfs_stat(path, &pre) == 0);

    if (a.has_size && vfs_truncate(path, a.size) < 0)
        status = vfs_errno_to_nfs3(errno);

    if (status == NFS3_OK &&
        (a.set_atime != (int)SET_DONT_CHANGE ||
         a.set_mtime != (int)SET_DONT_CHANGE)) {
        if (vfs_set_times(path, a.set_atime, a.atime_sec,
                                a.set_mtime, a.mtime_sec) < 0)
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
    char          dir_rel[MAX_PATH];
    char          obj_rel[MAX_PATH];
    vfs_stat_t    obj_st, dir_st;
    int           has_dir;

    xdr_read_fhandle3(in, &dir_fh, &ok);
    xdr_read_string(in, name, MAX_NAME);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok || in->error) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_post_op_attr(out, NULL, 0);
        return;
    }
    if (fh_resolve(&dir_fh, dir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_post_op_attr(out, NULL, 0);
        return;
    }

    snprintf(obj_path, MAX_PATH, "%s/%s", dir_path, name);
    has_dir = (vfs_stat(dir_path, &dir_st) == 0);

    if (vfs_stat(obj_path, &obj_st) < 0) {
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno));
        xdr_write_post_op_attr(out, &dir_st, has_dir);
        return;
    }

    fh_make(&obj_fh, dir_fh.export_id, obj_st.raw_dev, obj_st.raw_ino);

    /* Cache the object's relative path */
    dir_rel[0] = '\0';
    fh_cache_lookup(dir_fh.export_id, dir_fh.dev, dir_fh.ino,
                    dir_rel, MAX_PATH);
    make_child_relpath(dir_rel, name, obj_rel, MAX_PATH);
    fh_cache_insert(dir_fh.export_id, obj_st.raw_dev, obj_st.raw_ino,
                    obj_rel);

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_fhandle(out, &obj_fh);
    xdr_write_post_op_attr(out, &obj_st,  1);
    xdr_write_post_op_attr(out, &dir_st, has_dir);
}

/* ------------------------------------------------------------------ */
/* ACCESS                                                               */
/* ------------------------------------------------------------------ */
static void proc_access(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t fh;
    int           ok;
    uint32_t      requested;
    char          path[MAX_PATH];
    vfs_stat_t    st;
    int           has_st;

    xdr_read_fhandle3(in, &fh, &ok);
    requested = xdr_read_uint32(in);
    rpc_write_accept_hdr(out, xid, RPC_SUCCESS);

    if (!ok) {
        xdr_write_uint32(out, NFS3ERR_BADHANDLE);
        xdr_write_post_op_attr(out, NULL, 0);
        xdr_write_uint32(out, 0);
        return;
    }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_post_op_attr(out, NULL, 0);
        xdr_write_uint32(out, 0);
        return;
    }

    has_st = (vfs_stat(path, &st) == 0);

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &st, has_st);
    /* Grant all requested access (no fine-grained access control) */
    xdr_write_uint32(out, requested);
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
        return;
    }
    if (fh_resolve(&fh, path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_post_op_attr(out, NULL, 0);
        return;
    }

    if (count > MAX_READ_SIZE) count = MAX_READ_SIZE;
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
    char          path[MAX_PATH];
    vfs_stat_t    pre, post;
    int           has_pre, has_post;
    uint32_t      status;

    xdr_read_fhandle3(in, &fh, &ok);
    offset = xdr_read_uint64(in);
    count  = xdr_read_uint32(in);
    xdr_skip(in, 4);   /* stable_how -- we always use FILE_SYNC */
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

    has_pre  = (vfs_stat(path, &pre) == 0);
    if (data_len < count) count = data_len;

    if (vfs_pwrite(path, g_write_buf, count, offset) < 0)
        status = vfs_errno_to_nfs3(errno);
    else
        status = NFS3_OK;

    has_post = (vfs_stat(path, &post) == 0);

    xdr_write_uint32(out, status);
    xdr_write_wcc_data(out, &pre, has_pre, &post, has_post);

    if (status == NFS3_OK) {
        xdr_write_uint32(out, count);
        xdr_write_uint32(out, FILE_SYNC);   /* always fully committed */
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
    char          dir_rel[MAX_PATH];
    char          obj_rel[MAX_PATH];
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
    if (fh_resolve(&dir_fh, dir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_uint32(out, 0);
        xdr_write_post_op_attr(out, NULL, 0);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }

    snprintf(obj_path, MAX_PATH, "%s/%s", dir_path, name);
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

    /* EXCLUSIVE mode: we create if not exists; treat as UNCHECKED */
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
    if (has_obj) {
        fh_make(&obj_fh, dir_fh.export_id, obj_st.raw_dev, obj_st.raw_ino);
        dir_rel[0] = '\0';
        fh_cache_lookup(dir_fh.export_id, dir_fh.dev, dir_fh.ino,
                        dir_rel, MAX_PATH);
        make_child_relpath(dir_rel, name, obj_rel, MAX_PATH);
        fh_cache_insert(dir_fh.export_id, obj_st.raw_dev,
                        obj_st.raw_ino, obj_rel);
    }

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
    if (fh_resolve(&dir_fh, dir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }

    snprintf(obj_path, MAX_PATH, "%s/%s", dir_path, name);
    has_pre = (vfs_stat(dir_path, &dir_pre) == 0);

    status = (vfs_remove(obj_path) < 0)
                 ? vfs_errno_to_nfs3(errno) : NFS3_OK;

    has_post = (vfs_stat(dir_path, &dir_post) == 0);

    xdr_write_uint32(out, status);
    xdr_write_wcc_data(out, &dir_pre, has_pre, &dir_post, has_post);
}

/* ------------------------------------------------------------------ */
/* READDIR                                                              */
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
    uint32_t      reply_start;
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

    has_dir = (vfs_stat(dir_path, &dir_st) == 0);

    dp = vfs_opendir(dir_path);
    if (!dp) {
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno));
        xdr_write_post_op_attr(out, &dir_st, has_dir);
        return;
    }

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &dir_st, has_dir);
    xdr_write_raw(out, zero8, 8);   /* cookieverf */
    reply_start = xdr_get_pos(out);

    vfs_seekdir_to(dp, cookie);
    eof = 1; wrote_one = 0;

    while (vfs_readdir_next(dp, ename, MAX_NAME, &efileid, &ecookie) == 0) {
        name_len = (uint32_t)strlen(ename);
        /* Estimate: value_follows(4) + fileid(8) + name_len_hdr(4) +
           name_padded + cookie(8) */
        entry_sz = 4u + 8u + 4u + ((name_len + 3u) & ~3u) + 8u;

        before = xdr_get_pos(out);
        if ((before - reply_start) + entry_sz + 8u > maxcount
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
}

/* ------------------------------------------------------------------ */
/* READDIRPLUS                                                          */
/* ------------------------------------------------------------------ */
static void proc_readdirplus(xdr_t *in, xdr_t *out, uint32_t xid)
{
    our_fhandle_t dir_fh, efh;
    int           ok;
    uint64_t      cookie;
    uint32_t      maxcount;
    char          dir_path[MAX_PATH];
    char          dir_rel[MAX_PATH];
    char          entry_path[MAX_PATH];
    char          entry_rel[MAX_PATH];
    char          prel[MAX_PATH];   /* parent relpath for ".." */
    vfs_stat_t    dir_st, est;
    int           has_dir, has_est;
    vfs_dir_t    *dp;
    char          ename[MAX_NAME];
    uint64_t      efileid;
    uint64_t      ecookie;
    uint32_t      reply_start;
    uint32_t      before;
    uint32_t      name_len;
    int           eof;
    int           wrote_one;
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

    has_dir = (vfs_stat(dir_path, &dir_st) == 0);

    dp = vfs_opendir(dir_path);
    if (!dp) {
        xdr_write_uint32(out, vfs_errno_to_nfs3(errno));
        xdr_write_post_op_attr(out, &dir_st, has_dir);
        return;
    }

    xdr_write_uint32(out, NFS3_OK);
    xdr_write_post_op_attr(out, &dir_st, has_dir);
    xdr_write_raw(out, zero8, 8);      /* cookieverf */
    reply_start = xdr_get_pos(out);

    /* Retrieve this directory's relpath for building child relpaths */
    dir_rel[0] = '\0';
    fh_cache_lookup(dir_fh.export_id, dir_fh.dev, dir_fh.ino,
                    dir_rel, MAX_PATH);

    vfs_seekdir_to(dp, cookie);
    eof = 1; wrote_one = 0;

    while (vfs_readdir_next(dp, ename, MAX_NAME, &efileid, &ecookie) == 0) {
        snprintf(entry_path, MAX_PATH, "%s/%s", dir_path, ename);
        has_est = (vfs_stat(entry_path, &est) == 0);
        if (!has_est) continue;   /* skip unstat-able entries */

        name_len = (uint32_t)strlen(ename);
        /* Worst-case size: vf(4)+fileid(8)+name+cookie(8)+
           post_op_attr(4+84)+post_op_fh(4+4+OUR_FHSIZE+pad) */
        before = xdr_get_pos(out);

        if ((before - reply_start) + 200u + name_len > maxcount
            && wrote_one) {
            eof = 0;
            break;
        }

        /* Determine relpath for cache insertion */
        if (strcmp(ename, ".") == 0) {
            /* "." -- same as the directory itself */
            strncpy(entry_rel, dir_rel, MAX_PATH - 1);
            entry_rel[MAX_PATH - 1] = '\0';
        } else if (strcmp(ename, "..") == 0) {
            /* ".." -- parent of the current directory */
            parent_relpath(dir_rel, prel, MAX_PATH);
            strncpy(entry_rel, prel, MAX_PATH - 1);
            entry_rel[MAX_PATH - 1] = '\0';
        } else {
            make_child_relpath(dir_rel, ename, entry_rel, MAX_PATH);
        }

        fh_cache_insert(dir_fh.export_id, est.raw_dev,
                        est.raw_ino, entry_rel);
        fh_make(&efh, dir_fh.export_id, est.raw_dev, est.raw_ino);

        xdr_write_uint32(out, 1u);           /* value_follows */
        xdr_write_uint64(out, est.fileid);
        xdr_write_string(out, ename, name_len);
        xdr_write_uint64(out, ecookie);
        xdr_write_post_op_attr(out, &est, 1);
        xdr_write_uint32(out, 1u);           /* name_handle: present */
        xdr_write_fhandle(out, &efh);
        wrote_one = 1;
    }

    xdr_write_uint32(out, 0u);              /* end of entry list */
    xdr_write_uint32(out, (uint32_t)eof);
    vfs_closedir(dp);
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

    has_pre = has_post = (vfs_stat(path, &pre) == 0);
    if (has_pre) post = pre;

    /* We always write synchronously; nothing to flush */
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
    if (fh_resolve(&fdir, fdir_path, MAX_PATH) < 0 ||
        fh_resolve(&tdir, tdir_path, MAX_PATH) < 0) {
        xdr_write_uint32(out, NFS3ERR_STALE);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        xdr_write_wcc_data(out, NULL, 0, NULL, 0);
        return;
    }

    snprintf(from_path, MAX_PATH, "%s/%s", fdir_path, fname);
    snprintf(to_path,   MAX_PATH, "%s/%s", tdir_path, tname);

    has_fpre = (vfs_stat(fdir_path, &fpre) == 0);
    has_tpre = (vfs_stat(tdir_path, &tpre) == 0);

    status = (vfs_rename(from_path, to_path) < 0)
                 ? vfs_errno_to_nfs3(errno) : NFS3_OK;

    has_fpost = (vfs_stat(fdir_path, &fpost) == 0);
    has_tpost = (vfs_stat(tdir_path, &tpost) == 0);

    xdr_write_uint32(out, status);
    xdr_write_wcc_data(out, &fpre, has_fpre, &fpost, has_fpost);
    xdr_write_wcc_data(out, &tpre, has_tpre, &tpost, has_tpost);
}

/* ------------------------------------------------------------------ */
/* NOTSUPP stub: emit NFS3ERR_NOTSUPP with a null post_op_attr          */
/* ------------------------------------------------------------------ */
static void proc_notsupp(xdr_t *out, uint32_t xid)
{
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
    (void)fd;

    if (g_verbose)
        fprintf(stderr, "nfs3: xid=%08X proc=%s uid=%u\n",
                call->xid, proc_name(call->proc), call->auth_uid);

    if (call->vers != VERS_NFS) {
        rpc_write_prog_mismatch(out, call->xid, VERS_NFS, VERS_NFS);
        return;
    }

    switch (call->proc) {
    case NFS3PROC_NULL:        proc_null(out, call->xid);            break;
    case NFS3PROC_GETATTR:     proc_getattr(in, out, call->xid);     break;
    case NFS3PROC_SETATTR:     proc_setattr(in, out, call->xid);     break;
    case NFS3PROC_LOOKUP:      proc_lookup(in, out, call->xid);      break;
    case NFS3PROC_ACCESS:      proc_access(in, out, call->xid);      break;
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
}
