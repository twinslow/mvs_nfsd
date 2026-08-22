/*
 * vfs.c - Virtual filesystem abstraction layer (POSIX implementation).
 *
 * THIS IS THE ONLY FILE THAT TOUCHES OS-SPECIFIC FILESYSTEM CALLS.
 *
 * To port to MVS 3.8j, replace this entire file with an implementation
 * that uses:
 *   - BPXWDYN / SVC 99 for dataset allocation
 *   - EXCP / BSAM for PDS directory reads (DIRECTORY block format)
 *   - ISPF statistics block for mtime / member metadata
 *   - EBCDIC <-> ASCII translation wrappers around vfs_pread/vfs_pwrite
 *   - Fixed-length record to byte-stream conversion (insert \n at
 *     each record boundary)
 *
 * The struct vfs_dir type and the open-directory pool are defined in
 * mvsdol.c/mvsdol.h.  Directory iteration uses the dir_openlist_*
 * functions from that module.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <io.h>          /* JCC: _unlink() removes a PDS member */

#include "ebcdic.h"
#include "mvsfid.h"
#include "mvsio.h"
#include "mvspdir.h"
#include "mvsprw.h"
#include "mvspww.h"
#include "nfsd.h"
#include "hexdump.h"
#include "mvsfsz.h"
#include "mvsprf.h"
#include "mvsvfs.h"
#include "mvsdol.h"
#include "logger.h"

#define PATH_SEPARATOR_ASCII  (char)0x2f
#define PATH_SEPARATOR_EBCDIC (char)0x61

/* EXDEV / EROFS and the other errno values JCC omits are supplied by the
   errno compatibility block in nfsd.h, so the value this module SETS and the
   value nfserr.c TRANSLATES can never disagree. */

/* -------------------------------------------------------------------- */
/* vfs_stat: fill a vfs_stat_t as appropriate for a PDS dataset         */
/* or a PDS member.                                                     */
/* Returns 0 on success, -1 on error (errno set).                       */
/* -------------------------------------------------------------------- */

/*
 * Mode to REPORT for an object, given its configured permission bits and
 * whether it lives on a read-only export.  On a read-only export the write
 * bits are stripped, so `ls -l` and ACCESS never advertise a write we would
 * then refuse (design §4.3).  This is cosmetic honesty only -- enforcement
 * does not depend on it (see mvs_check_writable / check_access).
 */
static uint32_t vfs_report_mode(uint32_t perm, int readonly)
{
    return readonly ? (perm & ~0222u) : perm;
}

/*
 * Gate for every mutating operation.  Returns 0 if the dataset may be
 * modified, or -1 with errno=EROFS if it is on a read-only export.
 *
 * This is the enforcement point, and it is deliberately independent of uid
 * and of the reported mode bits: `ro` is a filesystem property, not a
 * permission, so no caller (not even uid 0) may override it (design §4.2).
 * Call it immediately after the dataset is resolved and BEFORE any state is
 * touched -- in particular before the pending-write pool -- so a refused
 * operation never creates or dirties a pending slot.
 */
static int mvs_check_writable(int export_idx, int dataset_idx)
{
    pds_dataset_t *ds = export_dataset_get(export_idx, dataset_idx);

    if (ds != NULL && ds->readonly) {
        errno = EROFS;
        return -1;
    }
    return 0;
}

static int vfs_stat_pds_member(const char *path, int export_idx,
                               int dataset_idx, vfs_stat_t *vs)
{
    pds_member_entry_t   mem_entry;
    pds_member_entry_t  *member_entry;
    pds_dataset_t       *ds;
    char                 pds_dsname[45];
    char                 pds_member_name[9];
    uint64_t             set_size;
    int                  retcode, rc2;
    mvsfsz_entry_t       file_size_entry;

    ds = export_dataset_get(export_idx, dataset_idx);
    if (ds == NULL) {
        errno = ENOENT;
        return -1;
    }

    /* Split the path into dataset and member name.
     *
     * The return value MUST be checked.  This call fails for a name that
     * cannot be a member of this PDS -- notably a wrong file extension, such
     * as vi's "foo.jcllib~" backup -- but it has ALREADY filled in
     * dsname/member from the leading part of the name before it rejects the
     * extension.  Ignoring the failure and falling through would let
     * "foo.jcllib~" match the pending-write buffer of "foo.jcllib" in
     * pww_find() below, so stat would report one object's attributes (and
     * identity) for a completely different name. */
    if (mvs_get_pds_dsn_and_member(path, pds_dsname, pds_member_name,
                                   export_idx) < 0)
        return -1;    /* errno set (ENOENT for wrong extension / bad name) */

    /* A member being written is not yet stowed in the PDS directory, so it
       must still be visible to stat.  Report its in-progress size from the
       pending-write pool. */
    {
        pending_member_t *pm = pww_find(pds_dsname, pds_member_name);
        if (pm != NULL) {
            time_t now_t = time(NULL);
            vs->ftype = NF3REG;
            vs->mode = vfs_report_mode(ds->memperm, ds->readonly);
            vs->fs_readonly = ds->readonly ? 1u : 0u;
            vs->nlink = 1;
            vs->uid = 0;
            vs->gid = 0;
            vs->size = pm->high_water;
            vs->used = pm->high_water;
            vs->rdev_maj = 0;
            vs->rdev_min = 0;
            vs->fsid    = (uint64_t)export_idx + 1;
            vs->fileid  = mvs_fid_hash(pds_dsname, pds_member_name);
            vs->raw_ino = mvs_fid_ino32(pds_dsname, pds_member_name);
            vs->raw_dev = (uint32_t)export_idx + 1;
            vs->atime_sec = (uint32_t)now_t; vs->atime_nsec = 0;
            vs->mtime_sec = (uint32_t)now_t; vs->mtime_nsec = 0;
            vs->ctime_sec = (uint32_t)now_t; vs->ctime_nsec = 0;
            return 0;
        }
    }

    /* First look to see if we have the info cached */
    retcode = mvsvfs_find_cached_member(pds_dsname, pds_member_name, &member_entry);
    if ( retcode < 0 ) {
        /* Nope, so we will get it from the PDS dir */
        member_entry = mvs_pds_get_member_entry(pds_dsname, pds_member_name, export_idx, &mem_entry);
        if (member_entry == NULL) {
            errno = ENOENT;
            return -1;
        }
    }

    /* Get the member size from the cache, or if required it will read the member and save in cache */
    rc2 = mvsfsz_get_member_size(
        pds_dsname, pds_member_name,
        member_entry, &set_size);

    vs->ftype = NF3REG;
    vs->mode = vfs_report_mode(ds->memperm, ds->readonly);
    vs->fs_readonly = ds->readonly ? 1u : 0u;
    vs->nlink = 1;   /* convention for files: one link from parent directory */
    vs->uid = 0;     /* root-owned */
    vs->gid = 0;     /* root-owned */
    vs->size = set_size;
    vs->used = set_size;
    vs->rdev_maj = 0;
    vs->rdev_min = 0;
    vs->fsid = (uint64_t)export_idx + 1; /* unique filesystem ID based on export index */
    vs->fileid = mvs_fid_hash(pds_dsname, pds_member_name); /* generate fileid based on dataset and member name */
    vs->raw_ino = mvs_fid_ino32(pds_dsname, pds_member_name); /* generate raw_ino based on dataset and member name */
    vs->raw_dev = (uint32_t)export_idx + 1; /* use export index as raw_dev for simplicity */

    vs->atime_sec = member_entry->chgdate; /* Access time as modified time */
    vs->atime_nsec = 0; // ISPF statistics do not have sub-second precision
    vs->mtime_sec = member_entry->chgdate;
    vs->mtime_nsec = 0; // ISPF statistics do not have sub-second precision
    vs->ctime_sec = member_entry->crdate;
    vs->ctime_nsec = 0; // ISPF statistics do not have sub-second precision

    return 0;
}


/*
 * Stable timestamp for synthetic directory (PDS) attributes.
 *
 * A PDS has no single mtime/ctime of its own, but the value we report
 * MUST be stable across calls.  The Linux NFS client uses a directory's
 * mtime/ctime as its readdir cache-change indicator: if they change
 * between successive READDIRPLUS calls, the client assumes the directory
 * was modified mid-scan, discards its readdir cache, and restarts from
 * cookie 0 -- an infinite loop for any directory that spans more than one
 * reply.  Captured once on first use and reused for the server's lifetime.
 */
static time_t g_dir_epoch = 0;

/* Lazily capture a stable, server-lifetime timestamp for synthetic
   directory attributes (see g_dir_epoch above). */
static uint32_t vfs_dir_epoch(void)
{
    struct timeval tv;
    if (g_dir_epoch == 0) {
        gettimeofday(&tv, NULL);
        g_dir_epoch = tv.tv_sec;
    }
    return (uint32_t)g_dir_epoch;
}

/*
 * Minimum seconds between out-of-band directory-change checks for a given
 * dataset.  Bounds the cost of the signature read to at most one PDS
 * directory read per this many seconds per dataset, regardless of how many
 * directory GETATTRs arrive.  Also the upper bound on how long an
 * out-of-band change (e.g. an IEBGENER add) takes to become visible.
 * Keep it comfortably larger than a client's READDIRPLUS enumeration time
 * so a genuine change rarely lands mid-scan (which would cost one harmless
 * cookie-0 restart on the Linux client).
 */
#define DIR_REFRESH_THROTTLE_SECS  10u

static int vfs_stat_dataset(const char *path, int export_idx,
                            int dataset_idx, vfs_stat_t *vs)
{
    pds_dataset_t *ds;
    uint32_t       dmtime;
    uint32_t       now_secs;
    uint32_t       sig;

    (void)path;

    ds = export_dataset_get(export_idx, dataset_idx);
    if (ds == NULL) {
        errno = ENOENT;
        return -1;
    }

    /* Out-of-band change detection (throttled).  Members added, removed, or
       replaced directly on MVS (IEBGENER, ISPF, ...) never pass through the
       NFS write path, so nothing bumps dir_mtime for them.  On a throttled
       schedule, re-read the PDS directory, fold it into a signature, and
       bump dir_mtime (and drop any cached listing) when it changes so the
       client re-reads.  The first check only establishes the baseline. */
    now_secs = (uint32_t)time(NULL);
    if (ds->dir_sig_check == 0 ||
        now_secs - ds->dir_sig_check >= DIR_REFRESH_THROTTLE_SECS) {
        if (mvs_pds_dir_signature(ds->dsname_ebcdic, export_idx, &sig) == 0) {
            if (ds->dir_sig_check != 0 && sig != ds->dir_sig) {
                ds->dir_mtime = now_secs;
                dir_openlist_invalidate(ds->dsname_ebcdic);
                logmsg_debug("NFSVF010D", "vfs_stat_dataset: %s changed out-of-band;"
                          " bumped dir_mtime",
                          log_ascii(ds->dsname_ebcdic));
            }
            ds->dir_sig = sig;
        }
        ds->dir_sig_check = now_secs;
    }

    /* Directory mtime: the per-dataset value once a member has been stowed
       (via the NFS write path) or an out-of-band change was detected above,
       otherwise the stable server epoch.  It is STABLE between modifications
       (so the client's readdir does not loop) but BUMPS when the directory
       changes (so the client invalidates its cached listing). */
    dmtime = (ds->dir_mtime != 0) ? ds->dir_mtime : vfs_dir_epoch();

    vs->ftype = NF3DIR;
    vs->mode = vfs_report_mode(ds->dirperm, ds->readonly);
    vs->fs_readonly = ds->readonly ? 1u : 0u;
    vs->nlink = 2;   /* convention for directories: link from parent and self-link */
    vs->uid = 0;     /* root-owned */
    vs->gid = 0;     /* root-owned */
    vs->size = 4096; /* arbitrary non-zero size for the directory itself */
    vs->used = 4096; /* arbitrary non-zero disk usage for the directory itself */
    vs->rdev_maj = 0;
    vs->rdev_min = 0;
    vs->fsid = (uint64_t)(export_idx + 1); /* unique filesystem ID based on export index */
    /* fileid / raw_ino are derived from the REAL dsname (not the lower-case
       path component) so they are stable and match the member fileids. */
    vs->fileid  = mvs_fid_hash(ds->dsname_ebcdic, NULL);
    vs->raw_ino = mvs_fid_ino32(ds->dsname_ebcdic, NULL);
    vs->raw_dev = (uint32_t)export_idx + 1;

    // Accessed/modified/created times: the per-dataset dir_mtime (see above).
    // Stable between member changes (no readdir loop), bumps on STOW (client
    // refreshes).  nsec forced to 0 to avoid any sub-second jitter.
    vs->atime_sec = dmtime;
    vs->atime_nsec = 0;
    vs->mtime_sec = dmtime;
    vs->mtime_nsec = 0;
    vs->ctime_sec = dmtime;
    vs->ctime_nsec = 0;

    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_stat_root: attributes for the export root (a virtual directory   */
/* whose entries are the PDS directories of the export).                */
/* -------------------------------------------------------------------- */
static int vfs_stat_root(const char *path, int export_idx, vfs_stat_t *vs)
{
    export_t *exp;
    uint32_t  epoch;

    (void)path;

    exp = exports_get(export_idx);
    if (exp == NULL) {
        errno = ENOENT;
        return -1;
    }

    epoch = vfs_dir_epoch();

    vs->ftype = NF3DIR;
    /* The export root is ALWAYS read-only for ACCESS -- it cannot be modified
       through NFS (MKDIR/RMDIR are NOTSUPP), so this is a fact, not policy
       (design §4.4).  rootperm only tunes the reported r/x bits. */
    vs->mode = vfs_report_mode(exp->rootperm, 1);
    vs->fs_readonly = 1u;
    vs->nlink = 2;
    vs->uid = 0;
    vs->gid = 0;
    vs->size = 4096;
    vs->used = 4096;
    vs->rdev_maj = 0;
    vs->rdev_min = 0;
    vs->fsid = (uint64_t)(export_idx + 1);
    /* Root identity is derived from the export path, distinct from any PDS. */
    vs->fileid  = mvs_fid_hash(exp->export_path_ebcdic, NULL);
    vs->raw_ino = mvs_fid_ino32(exp->export_path_ebcdic, NULL);
    vs->raw_dev = (uint32_t)export_idx + 1;

    vs->atime_sec = epoch; vs->atime_nsec = 0;
    vs->mtime_sec = epoch; vs->mtime_nsec = 0;
    vs->ctime_sec = epoch; vs->ctime_nsec = 0;

    return 0;
}

void dump_stat_result(const char *path, int rc, vfs_stat_t *vs) {
    char       atime_buf[32];
    char       mtime_buf[32];
    char       ctime_buf[32];
    time_t     t;
    struct tm *tm_p;

    // Suppress output for rc == 0 for now
    if (rc == 0)
        return;

    logmsg_debug("NFSVF020D", "vfs_stat: Result for path=%s ending retcode=%d",
        log_ascii(path), rc);
    if (rc == 0) {

        t    = (time_t)vs->atime_sec;
        tm_p = gmtime(&t);
        if (tm_p != NULL)
            strftime(atime_buf, sizeof(atime_buf), "%Y-%m-%d %H:%M:%S", tm_p);
        else
            atime_buf[0] = '\0';

        t    = (time_t)vs->mtime_sec;
        tm_p = gmtime(&t);
        if (tm_p != NULL)
            strftime(mtime_buf, sizeof(mtime_buf), "%Y-%m-%d %H:%M:%S", tm_p);
        else
            mtime_buf[0] = '\0';

        t    = (time_t)vs->ctime_sec;
        tm_p = gmtime(&t);
        if (tm_p != NULL)
            strftime(ctime_buf, sizeof(ctime_buf), "%Y-%m-%d %H:%M:%S", tm_p);
        else
            ctime_buf[0] = '\0';

        logmsg_debug("NFSVF030D", "vfs_stat:      vs->mode      = %04o",             vs->mode);
        //logmsg_debug("NFSVF040D", "vfs_stat:      vs->size      = %lld",             vs->size);
        //logmsg_debug("NFSVF050D", "vfs_stat:      vs->used      = %lld",             vs->used);
        //logmsg_debug("NFSVF060D", "vfs_stat:      vs->fsid      = %lld",             vs->fsid);
        //logmsg_debug("NFSVF070D", "vfs_stat:      vs->fileid    = 0x%016X",          vs->fileid);
        //logmsg_debug("NFSVF080D", "vfs_stat:      vs->raw_ino   = 0x%08X",           vs->raw_ino);
        //logmsg_debug("NFSVF090D", "vfs_stat:      vs->atime_sec = 0x%08X (%s UTC)",  vs->atime_sec, atime_buf);
        logmsg_debug("NFSVF100D", "vfs_stat:      vs->mtime_sec = 0x%08X (%s UTC)",  vs->mtime_sec, mtime_buf);
        //logmsg_debug("NFSVF110D", "vfs_stat:      vs->ctime_sec = 0x%08X (%s UTC)",  vs->ctime_sec, ctime_buf);
    }
}

int vfs_stat(const char *path, vfs_stat_t *vs)
{
    int     export_idx;
    int     dataset_idx;
    char    ebcdic_path[MAX_PATH_LEN];
    int     path_type;
    int     retcode;
    clock_t t_start;

    logmsg_debug("NFSVF120D", "vfs_stat: path=%s", log_ascii(path));

    t_start = clock();

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Classify: export root, a PDS directory, or a PDS member. */
    path_type = mvs_path_type(ebcdic_path, &export_idx, &dataset_idx);

    if (path_type == MVS_PATH_TYPE_PDS_MEMBER) {
        retcode = vfs_stat_pds_member(ebcdic_path, export_idx, dataset_idx, vs);
        dump_stat_result(path, retcode, vs);
        mvsprf_record(PERF_VFS_STAT, clock() - t_start);
        return retcode;
    } else if (path_type == MVS_PATH_TYPE_DATASET) {
        retcode = vfs_stat_dataset(ebcdic_path, export_idx, dataset_idx, vs);
        dump_stat_result(path, retcode, vs);
        mvsprf_record(PERF_VFS_STAT, clock() - t_start);
        return retcode;
    } else if (path_type == MVS_PATH_TYPE_ROOT) {
        retcode = vfs_stat_root(ebcdic_path, export_idx, vs);
        dump_stat_result(path, retcode, vs);
        mvsprf_record(PERF_VFS_STAT, clock() - t_start);
        return retcode;
    }
    errno = ENOENT;
    logmsg_debug("NFSVF130D", "vfs_stat: Result error ... returning -1");
    return -1;
}

/* -------------------------------------------------------------------- */
/* vfs_pread: positional read from path.                                */
/* Opens, reads count bytes at offset, closes.                          */
/* Sets *nread to bytes actually read; *eof to 1 if at end of file.     */
/* Returns 0 on success, -1 on error.                                   */
/* -------------------------------------------------------------------- */

int vfs_pread(const char *path, void *buf, uint32_t count,
              uint64_t offset, uint32_t *nread, int *eof)
{
    int         saved_errno;
    int         rc, rc2, saved;
    char        ebcdic_path[MAX_PATH_LEN];
    char        pds_dsname[45];
    char        pds_member_name[9];
    int         export_idx;
    int         path_type;
    uint64_t    actual_file_size;
    pds_member_entry_t *member_entry;
    vfs_stat_t  vs;


    logmsg_debug("NFSVF140D", "vfs_pread: path=%s, count=%d, offset=%lld",
        log_ascii(path), count, offset);

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Is this path to a directory or a member of a PDS */
    path_type = mvs_path_type(ebcdic_path, &export_idx, NULL);
    logmsg_debug("NFSVF150D", "vfs_pread: mvs_path_type returned type = %d, export_idx = %d",
        path_type, export_idx);
    if (path_type == MVS_PATH_TYPE_DATASET || path_type == MVS_PATH_TYPE_ROOT) {
        errno = EACCES;
        return -1;
    } else if ( path_type != MVS_PATH_TYPE_PDS_MEMBER ){
        errno = ENOENT;
        return -1;
    }

    /* Split the path into dataset and member name.  The return value MUST be
       checked -- see the note in vfs_stat: on failure (e.g. a wrong file
       extension) dsname/member are already populated, so falling through
       would let a name like "foo.jcllib~" read "foo.jcllib"'s pending
       buffer below. */
    if (mvs_get_pds_dsn_and_member(ebcdic_path, pds_dsname,
                                   pds_member_name, export_idx) < 0)
        return -1;    /* errno set (ENOENT for wrong extension / bad name) */

    /* If the member is being written (buffered, not yet stowed), serve the
       read from the pending content so the not-yet-stowed data is visible.
       pww_read_range fetches from the in-memory buffer or the spill dataset as
       appropriate; the content is already ASCII, so no EBCDIC translation. */
    {
        pending_member_t *pm = pww_find(pds_dsname, pds_member_name);
        if (pm != NULL) {
            if (offset >= (uint64_t)pm->high_water) {
                *nread = 0;
                *eof   = 1;
            } else {
                uint32_t avail = pm->high_water - (uint32_t)offset;
                uint32_t n     = (count < avail) ? count : avail;
                if (pww_read_range(pm, (uint32_t)offset,
                                   (uint8_t *)buf, n) != 0)
                    return -1;    /* errno set (EIO on a spill read error) */
                *nread = n;
                *eof   = ((uint32_t)offset + n >= pm->high_water);
            }
            logmsg_debug("NFSVF160D", "vfs_pread: served %u bytes from pending %s(%s)",
                *nread, pds_dsname, pds_member_name);
            return 0;
        }
    }

    /* We've confirmed that we have a dataset and member name, so we can read. */
    /* The below read routine handles file open, caching last op info and fclose. */
    rc = mvs_pds_member_read(
        pds_dsname,
        pds_member_name,
        export_idx,
        offset,
        count,
        buf,
        nread,
        eof);

    // Translate the data read from the host (EBCDIC) to ASCII for the
    // NFS client.
    if ( rc == 0 && *nread > 0 ) {
        ebcdic_to_ascii(buf, buf, (size_t)*nread);
    }

    logmsg_debug("NFSVF170D", "vfs_pread: Completed path=%s, nread=%d, eof=%d",
        log_ascii(path), *nread, *eof);

    return rc;
}


/* -------------------------------------------------------------------- */
/* vfs_pwrite: positional write to path.                                */
/* Opens for writing, writes count bytes at offset, fsyncs, closes.     */
/* Returns 0 on success, -1 on error.                                   */
/* -------------------------------------------------------------------- */
int vfs_pwrite(const char *path, const void *buf,
               uint32_t count, uint64_t offset)
{
    char ebcdic_path[MAX_PATH_LEN];
    char pds_dsname[45];
    char pds_member_name[9];
    int  export_idx;
    int  dataset_idx;
    int  path_type;

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    path_type = mvs_path_type(ebcdic_path, &export_idx, &dataset_idx);
    if (path_type != MVS_PATH_TYPE_PDS_MEMBER) {
        errno = (path_type == MVS_PATH_TYPE_ROOT ||
                 path_type == MVS_PATH_TYPE_DATASET) ? EISDIR : ENOENT;
        return -1;
    }
    if (mvs_check_writable(export_idx, dataset_idx) < 0)
        return -1;    /* errno = EROFS -> NFS3ERR_ROFS */
    if (mvs_get_pds_dsn_and_member(ebcdic_path, pds_dsname,
                                   pds_member_name, export_idx) < 0)
        return -1;

    return pww_write(export_idx, dataset_idx, pds_dsname, pds_member_name,
                     (const uint8_t *)buf, count, offset);
}

/* -------------------------------------------------------------------- */
/* vfs_commit: flush a pending member to the PDS (NFS COMMIT).           */
/* Returns 0 on success (including nothing-to-commit), -1 on error.     */
/* -------------------------------------------------------------------- */
int vfs_commit(const char *path)
{
    char ebcdic_path[MAX_PATH_LEN];
    char pds_dsname[45];
    char pds_member_name[9];
    int  export_idx;
    int  dataset_idx;
    int  path_type;

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    path_type = mvs_path_type(ebcdic_path, &export_idx, &dataset_idx);
    if (path_type != MVS_PATH_TYPE_PDS_MEMBER)
        return 0;    /* nothing to commit for a directory / root */

    if (mvs_get_pds_dsn_and_member(ebcdic_path, pds_dsname,
                                   pds_member_name, export_idx) < 0)
        return 0;

    return pww_flush_member(pds_dsname, pds_member_name);
}

/* -------------------------------------------------------------------- */
/* vfs_create: create a new empty PDS member (pending, stowed on commit).*/
/* mode is ignored (PDS members have no POSIX permission bits).          */
/* Returns 0 on success, -1 on error.                                   */
/* -------------------------------------------------------------------- */
int vfs_create(const char *path, uint32_t mode)
{
    char ebcdic_path[MAX_PATH_LEN];
    char pds_dsname[45];
    char pds_member_name[9];
    int  export_idx;
    int  dataset_idx;
    int  path_type;

    (void)mode;

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    path_type = mvs_path_type(ebcdic_path, &export_idx, &dataset_idx);
    if (path_type != MVS_PATH_TYPE_PDS_MEMBER) {
        /* Can only create a member (not the root or a PDS directory). */
        errno = EACCES;
        return -1;
    }
    if (mvs_check_writable(export_idx, dataset_idx) < 0)
        return -1;    /* errno = EROFS -> NFS3ERR_ROFS */
    if (mvs_get_pds_dsn_and_member(ebcdic_path, pds_dsname,
                                   pds_member_name, export_idx) < 0)
        return -1;

    return pww_create(export_idx, dataset_idx, pds_dsname, pds_member_name);
}

/* -------------------------------------------------------------------- */
/* vfs_remove: delete a PDS member (NFS REMOVE).                        */
/*                                                                      */
/* Resolves the path to a member, drops any buffered (pending) write so */
/* a later flush can't recreate it, then removes the member with JCC's  */
/* _unlink().  A member that only ever existed in the write buffer is    */
/* not on disk, so _unlink's ENOENT is treated as success in that case. */
/* Finally bumps the directory mtime and drops the cached listing so the */
/* member disappears from the next readdir (same as the STOW path).      */
/* -------------------------------------------------------------------- */
int vfs_remove(const char *path)
{
    char ebcdic_path[MAX_PATH_LEN];
    char pds_dsname[45];
    char pds_member_name[9];
    int  export_idx;
    int  dataset_idx;
    int  had_pending;
    int  rc;
    char dsn_path[6 + 44 + 1 + 8 + 1 + 1];   /* //DSN:dsname(member)\0 */

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path,
                    MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Only a PDS member is a REMOVE target; a PDS directory / export root
       is not (that would be RMDIR). */
    if (mvs_path_type(ebcdic_path, &export_idx, &dataset_idx)
            != MVS_PATH_TYPE_PDS_MEMBER) {
        errno = EISDIR;
        return -1;
    }
    /* Refuse before touching the pending pool, so a rejected remove cannot
       discard a buffered write. */
    if (mvs_check_writable(export_idx, dataset_idx) < 0)
        return -1;    /* errno = EROFS -> NFS3ERR_ROFS */
    if (mvs_get_pds_dsn_and_member(ebcdic_path, pds_dsname,
                                   pds_member_name, export_idx) < 0)
        return -1;    /* errno set (ENOENT for wrong extension / bad name) */

    /* Drop any buffered write first, so a later flush cannot recreate the
       member we are about to delete. */
    had_pending = pww_discard(pds_dsname, pds_member_name);

    strcpy(dsn_path, "//DSN:");
    strcat(dsn_path, pds_dsname);
    strcat(dsn_path, "(");
    strcat(dsn_path, pds_member_name);
    strcat(dsn_path, ")");
    errno = 0;
    rc = _unlink(dsn_path);

    if (rc != 0) {
        /* If it only lived in the write buffer, discarding it WAS the
           removal; otherwise report the real failure. */
        if (had_pending && errno == ENOENT)
            rc = 0;
        else
            return -1;
    }

    /* Directory changed: bump its mtime so clients refresh, and drop our
       cached scan so the next readdir omits the removed member. */
    export_dataset_touch(export_idx, dataset_idx);
    dir_openlist_invalidate(pds_dsname);

    logmsg_debug("NFSVF180D", "vfs_remove: removed %s(%s)", pds_dsname, pds_member_name);
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_rename: rename a PDS member within its dataset.                   */
/*                                                                      */
/* A PDS member cannot be moved to a different dataset, so the source   */
/* and target must resolve to the SAME PDS; a cross-PDS request is       */
/* rejected with XDEV so the client can fall back to copy+delete.        */
/* -------------------------------------------------------------------- */
int vfs_rename(const char *from, const char *to)
{
    char ebcdic_from[MAX_PATH_LEN];
    char ebcdic_to[MAX_PATH_LEN];
    char from_dsname[45];
    char from_member[9];
    char to_dsname[45];
    char to_member[9];
    int  from_export_idx;
    int  from_dataset_idx;
    int  to_export_idx;
    int  to_dataset_idx;
    int  rc;
    char from_path[6 + 44 + 1 + 8 + 1 + 1];   /* //DSN:dsname(member)\0 */
    char to_path[6 + 44 + 1 + 8 + 1 + 1];

    ascii_to_ebcdic((uint8_t *)ebcdic_from, (const uint8_t *)from,
                    MAX_PATH_LEN - 1);
    ebcdic_from[MAX_PATH_LEN - 1] = '\0';
    ascii_to_ebcdic((uint8_t *)ebcdic_to, (const uint8_t *)to,
                    MAX_PATH_LEN - 1);
    ebcdic_to[MAX_PATH_LEN - 1] = '\0';

    /* Both endpoints must be PDS members; a PDS directory / export root is
       not a rename target here (that would be a directory rename). */
    if (mvs_path_type(ebcdic_from, &from_export_idx, &from_dataset_idx)
            != MVS_PATH_TYPE_PDS_MEMBER) {
        errno = EISDIR;
        return -1;
    }
    if (mvs_path_type(ebcdic_to, &to_export_idx, &to_dataset_idx)
            != MVS_PATH_TYPE_PDS_MEMBER) {
        errno = EISDIR;
        return -1;
    }
    if (mvs_get_pds_dsn_and_member(ebcdic_from, from_dsname,
                                   from_member, from_export_idx) < 0)
        return -1;    /* errno set (ENOENT for wrong extension / bad name) */
    if (mvs_get_pds_dsn_and_member(ebcdic_to, to_dsname,
                                   to_member, to_export_idx) < 0)
        return -1;

    /* BOTH ends must be writable (design §5).  Same-PDS is enforced below, so
       these normally name the same dataset, but the two-export-of-one-PDS
       edge is covered by checking each. */
    if (mvs_check_writable(from_export_idx, from_dataset_idx) < 0 ||
        mvs_check_writable(to_export_idx, to_dataset_idx) < 0)
        return -1;    /* errno = EROFS -> NFS3ERR_ROFS */

    /* A member can only be renamed within its own dataset -- there is no
       cross-PDS member move.  Report XDEV so the client falls back to
       copy+delete (see the errno note at the top of this file). */
    if (strcmp(from_dsname, to_dsname) != 0) {
        errno = EXDEV;
        return -1;
    }

    /* Settle buffered writes before touching the directory: flush the source
       so its on-disk member is complete (a member written but not yet stowed
       would otherwise be lost, or the rename would not find it), and drop any
       buffer for the target, which the rename is about to replace. */
    if (pww_flush_member(from_dsname, from_member) < 0)
        return -1;    /* errno set by the STOW */
    pww_discard(to_dsname, to_member);

    strcpy(from_path, "//DSN:");
    strcat(from_path, from_dsname);
    strcat(from_path, "(");
    strcat(from_path, from_member);
    strcat(from_path, ")");

    strcpy(to_path, "//DSN:");
    strcat(to_path, to_dsname);
    strcat(to_path, "(");
    strcat(to_path, to_member);
    strcat(to_path, ")");

    /* JCC rename() renames the member within the PDS.  NOTE: behaviour when
       the target member already exists is JCC-defined and unverified here;
       the common client case (rename to a new, unused name) does not hit it. */
    errno = 0;
    rc = rename(from_path, to_path);
    if (rc != 0)
        return -1;    /* errno set by rename() */

    /* The source's (now clean) pending slot still references the OLD name;
       drop it so it cannot shadow the renamed-away member in vfs_stat. */
    pww_discard(from_dsname, from_member);

    /* Directory changed: bump its mtime and drop the cached scan so clients
       refresh and the next readdir reflects the new member name. */
    export_dataset_touch(from_export_idx, from_dataset_idx);
    dir_openlist_invalidate(from_dsname);

    logmsg_debug("NFSVF190D", "vfs_rename: renamed %s(%s) -> (%s)",
              from_dsname, from_member, to_member);
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_truncate: set a member's size (NFS SETATTR size / O_TRUNC).       */
/* Routed to the pending-write pool; the common case is truncate-to-0    */
/* before a rewrite.  Returns 0 on success, -1 on error.                 */
/* -------------------------------------------------------------------- */
int vfs_truncate(const char *path, uint64_t size)
{
    char ebcdic_path[MAX_PATH_LEN];
    char pds_dsname[45];
    char pds_member_name[9];
    int  export_idx;
    int  dataset_idx;
    int  path_type;

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    path_type = mvs_path_type(ebcdic_path, &export_idx, &dataset_idx);
    if (path_type != MVS_PATH_TYPE_PDS_MEMBER) {
        errno = EISDIR;
        return -1;
    }
    if (mvs_check_writable(export_idx, dataset_idx) < 0)
        return -1;    /* errno = EROFS -> NFS3ERR_ROFS */
    if (mvs_get_pds_dsn_and_member(ebcdic_path, pds_dsname,
                                   pds_member_name, export_idx) < 0)
        return -1;

    if (size > (uint64_t)PWW_MAX_MEMBER_BYTES) {
        errno = ENOSPC;
        return -1;
    }

    /*
     * SHRINKING a member that is ALREADY STOWED, with nothing buffered for
     * it, would mean rewriting it on disk -- which this server does not do.
     * pww_truncate() used to accept that silently and change nothing, which
     * lies to the client: it is told the file is now N bytes while the next
     * read still returns the old, longer content.  Report it instead, with
     * the same EIO -> NFS3ERR_IO a random write at a non-zero offset already
     * gets (mvspww.c), so the two unsupported shapes answer alike.
     *
     * ONLY A SHRINK IS REFUSED.  The two other cases are normal traffic and
     * must be accepted, or ordinary editing breaks:
     *
     *   same size -- clients confirm the size after writing (Windows sends
     *     WRITE -> COMMIT -> SETATTR -> COMMIT), and once the idle sweep has
     *     released the slot that trailing SETATTR arrives here.  It asks for
     *     no change, so it is the no-op it looks like.
     *
     *   GROW -- this is PREALLOCATION: CREATE, SETATTR(size), then WRITE
     *     the content.  Note that SOME clients do this and some do not --
     *     Windows Notepad preallocates, vim does not -- so getting it
     *     wrong breaks a SUBSET of editors, which is harder to diagnose
     *     than breaking all of them: it reads as a client-specific fault.
     *
     *     If the client pauses between the CREATE and the save for longer
     *     than the idle sweep, the empty member is stowed and the slot
     *     dropped, so the SETATTR lands here with nothing buffered.  Six
     *     seconds of typing in Notepad was enough (2026-08-20), and every
     *     save then reported a failure to the user -- asking them to pick
     *     another location -- while the WRITE and COMMIT that followed
     *     succeeded and the content was correct.
     *
     *     The writes that follow define the content, so accepting the grow
     *     costs nothing.  Residual: a grow never followed by writes leaves
     *     the member at its old length rather than zero-extended.  Nothing
     *     is lost -- the client only asked for zeros it never wrote -- and
     *     fixing it properly needs the on-disk content read back into a
     *     slot, which this server does not do.
     */
    if (size != 0 && pww_find(pds_dsname, pds_member_name) == NULL) {
        vfs_stat_t st;

        if (vfs_stat(path, &st) < 0)
            return -1;               /* errno from vfs_stat, e.g. ENOENT */

        if (size < st.size) {
            logmsg_warn("NFSVF410W", "vfs_truncate: %s(%s) shrink %lu -> %lu"
                        " refused -- the member is already stowed and"
                        " rewriting it is not supported",
                        pds_dsname, pds_member_name,
                        (unsigned long)st.size, (unsigned long)size);
            errno = EIO;             /* -> NFS3ERR_IO */
            return -1;
        }
        return 0;                    /* same size, or a grow: nothing to do */
    }

    return pww_truncate(export_idx, dataset_idx, pds_dsname, pds_member_name,
                        (uint32_t)size);
}

/* ---------------------------------------------------------------------- */
/* vfs_set_times: set atime and/or mtime on path.                         */
/* set_atime / set_mtime use the SET_* constants from nfsd.h:             */
/*   SET_DONT_CHANGE      - leave this timestamp unchanged                */
/*   SET_TO_SERVER_TIME   - set to the server's current time              */
/*   SET_TO_CLIENT_TIME   - set to the supplied sec/nsec values           */
/*                                                                        */
/* A PDS member has one settable timestamp -- the ISPF "changed" date.    */
/* We map the requested modification time onto it (preferring mtime,      */
/* falling back to atime) and update the member's ISPF stats in place     */
/* via pww_touch_stats (which no-ops unless the member already has stats  */
/* and the time actually changed).  The content is NOT rewritten and the  */
/* modification level is NOT bumped -- a time change is not a content     */
/* change.  Returns 0 for the normal case (SETATTR must not fail, as      */
/* clients issue it during the write sequence), except on a read-only     */
/* export, where it fails with EROFS.                                     */
/* ---------------------------------------------------------------------- */
int vfs_set_times(const char *path,
                  int set_atime, uint32_t atime_sec, uint32_t atime_nsec,
                  int set_mtime, uint32_t mtime_sec, uint32_t mtime_nsec)
{
    char   ebcdic_path[MAX_PATH_LEN];
    char   pds_dsname[45];
    char   pds_member_name[9];
    int    export_idx;
    int    dataset_idx;
    time_t new_time;

    (void)atime_nsec; (void)mtime_nsec;   /* ISPF resolution is one second */

    /* Which requested time drives the ISPF changed date?  Prefer mtime.
       Both a client-supplied time and time() are UTC epoch; the local-time
       conversion happens later when the stats are encoded. */
    if (set_mtime == (int)SET_TO_CLIENT_TIME)
        new_time = (time_t)mtime_sec;
    else if (set_mtime == (int)SET_TO_SERVER_TIME)
        new_time = time(NULL);
    else if (set_atime == (int)SET_TO_CLIENT_TIME)
        new_time = (time_t)atime_sec;
    else if (set_atime == (int)SET_TO_SERVER_TIME)
        new_time = time(NULL);
    else
        return 0;                         /* no time being set */

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path,
                    MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Only a PDS member carries ISPF stats; anything else is a no-op. */
    if (mvs_path_type(ebcdic_path, &export_idx, &dataset_idx)
            != MVS_PATH_TYPE_PDS_MEMBER)
        return 0;
    /* A time change rewrites ISPF stats via STOW, so it is a mutation and is
       refused on a read-only export (the one case where SETATTR does fail). */
    if (mvs_check_writable(export_idx, dataset_idx) < 0)
        return -1;    /* errno = EROFS -> NFS3ERR_ROFS */
    if (mvs_get_pds_dsn_and_member(ebcdic_path, pds_dsname,
                                   pds_member_name, export_idx) < 0)
        return 0;                         /* unresolvable -- accept, no change */

    return pww_touch_stats(export_idx, dataset_idx,
                           pds_dsname, pds_member_name, new_time);
}

/* -------------------------------------------------------------------- */
/* vfs_fsstat: fill vfs_fsstat_t from statvfs().                        */
/* -------------------------------------------------------------------- */
int vfs_fsstat(const char *path, vfs_fsstat_t *fs)
{
    errno = EACCES;
    return -1;

#if 0
    struct statvfs sv;

    if (statvfs(path, &sv) < 0) return -1;

    fs->total_bytes = (uint64_t)sv.f_blocks * (uint64_t)sv.f_frsize;
    fs->free_bytes  = (uint64_t)sv.f_bfree  * (uint64_t)sv.f_frsize;
    fs->avail_bytes = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
    fs->total_files = (uint64_t)sv.f_files;
    fs->free_files  = (uint64_t)sv.f_ffree;
    fs->avail_files = (uint64_t)sv.f_favail;
    fs->invarsec    = 0;
    return 0;
#endif
}

/*

nfs3.c proc_readdir() does the following --

Notes:

1) I think we can use the EBCDIC member name as the cookie value as it
   is 8 bytes / uint64_t. That will make the restart of a directory
   read easier, as we will have the actual member name.
2) mvspdir.c deals ONLY in EBCDIC.
3) nfs3.c deals ONLY in ASCII
4) mvsvfs.c does the translations between ASCII and EBCDIC for --
    * Path names
    * Member names being returned as file names
    * Read buffers
    * Write buffers

vfs_opendir
    Translate path from ASCII to EBCDIC
    Check path represents a PDS
    Get EBCDIC dataset name from path

    Find a free "opendir" entry : mvspdir_openlist_find_free(...)
    Allocate and open the PDS : mvs_open_pds_dir
    Setup the "opendir" entry : mvspdir_openlist_set_init(...)
    With --
        PDS dataset name
        Cookie -- can be init as 0x01 (or 0x4040404040404040 if we wanted a character value)
        PDS fopen file handle
    Update the "opendir" entry : mvspdir_openlist_clear_memlist(...)
        Clear and previously read/cached members in "opendir"
        Set the cookie, which is last (prior) EBCDIC member name or null/blank.

loop until return buffer is full
    vfs_readdir_next : mvs_readnext_pds_dir(...)
        If no members, or out of members, read from MVS PDS directory
            The starting member name is always greater than the cookie value.
            We can see if there is a member in the list GT cookie value
            If not, fetch more : mvs_pds_member_list(...)
        Return the next member info... or end of directory.
            Translate member name to ASCII
            Set cookie to EBCDIC member name
            Generate fileid value for the dataset/member

vfs_closedir
    Close and free PDS
    Free the "opendir" entry : mvs_close_pds_dir(...)


*/





/* -------------------------------------------------------------------- */
/* Convert the EBCDIC character string (null terminated) to a cookie    */
/* value, which is the EBCDIC chars padded with spaces if required.     */
/* -------------------------------------------------------------------- */
uint64_t to_cookie(const char *member_name) {
    uint64_t cookie = 0;
    char    *cookie_char = (char *)&cookie;

    int i;

    strncpy(cookie_char, member_name, sizeof(cookie));
    for (i = sizeof(cookie) - 1; i > 0; i--) {
        if ( cookie_char[i] == '\0' )
            cookie_char[i] = 0x40;  /* EBCDIC space */
        else
            break;
    }

    return cookie;
}

/* -------------------------------------------------------------------- */
/* Convert the cookie value (EBCDIC chars padded with spaces) to        */
/* a null terminated character string (in EBCDIC)                       */
/* -------------------------------------------------------------------- */
void from_cookie(const uint64_t cookie, char *pds_member_name) {
    uint64_t cookie_val = cookie;
    char *cookie_char = (char *)&cookie_val;
    int i;

    for (i = 0; i < sizeof(cookie_val); i++) {
        if (cookie_char[i] != 0x40)
            pds_member_name[i] = cookie_char[i];
        else {
            pds_member_name[i] = '\0';
            break;
        }
    }
    if ( cookie_char[7] != 0x40 )
        pds_member_name[8] = '\0';
}


/* -------------------------------------------------------------------- */
/* Generate the file name from the member name and file extension       */
/* in the export config.                                                */
/* -------------------------------------------------------------------- */
void generate_file_name(
    int                  export_idx,
    int                  dataset_idx,
    pds_member_entry_t  *member_info,
    char                *file_name_buffer,
    int                  buflen)
{
    int            i;
    pds_dataset_t *ds;

    ds = export_dataset_get(export_idx, dataset_idx);

    /* strncpy does NOT terminate when the source fills the buffer, so leave
       room for the NUL and write it explicitly. */
    strncpy(file_name_buffer, member_info->name, (size_t)(buflen - 1));
    file_name_buffer[buflen - 1] = '\0';
    for (i = 0; file_name_buffer[i] != '\0'; i++)
        file_name_buffer[i] = tolower(file_name_buffer[i]);

    /* strncat's 3rd argument is the maximum number of characters taken FROM
       THE SOURCE, not the size of the destination: passing buflen permits a
       write of strlen(dst) + buflen + 1 bytes.  Pass the room that is
       actually left instead. */
    if ( ds != NULL && strlen(ds->file_ext) > 0 ) {
        size_t used = strlen(file_name_buffer);
        size_t room = (used + 1 < (size_t)buflen)
                    ? (size_t)buflen - used - 1 : 0;
        strncat(file_name_buffer, ".", room);

        used = strlen(file_name_buffer);
        room = (used + 1 < (size_t)buflen)
             ? (size_t)buflen - used - 1 : 0;
        strncat(file_name_buffer, ds->file_ext, room);
    }

    ebcdic_to_ascii(file_name_buffer, file_name_buffer, strlen(file_name_buffer));
}

/* -------------------------------------------------------------------- */
/* vfs_opendir: open a directory for iteration.                         */
/* Returns a handle from the static pool, or NULL on error.             */
/*                                                                      */
/* Fast path: if a non-expired pool slot already holds a complete       */
/* member cache (end_of_dir_read=1) for this PDS, return it directly    */
/* without re-reading the disk.  This prevents pool exhaustion when the */
/* NFS client issues multiple READDIRPLUS calls for the same directory. */
/* -------------------------------------------------------------------- */
vfs_dir_t *vfs_opendir(const char *path, uint64_t cookie)
{
    vfs_dir_t     *dir_entry;
    char           ebcdic_path[MAX_PATH_LEN];
    pds_dataset_t *ds;
    int            export_idx;
    int            dataset_idx;
    int            retcode;
    int            path_type;

    logmsg_debug("NFSVF200D", "vfs_opendir: Starting with path=%s cookie=0x%016llX",
        log_ascii(path), cookie);

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    path_type = mvs_path_type(ebcdic_path, &export_idx, &dataset_idx);
    logmsg_debug("NFSVF210D", "vfs_opendir: path %s, type = %d", log_ascii(path), path_type);

    /* ---- Export root: a virtual directory listing the PDS dirs. ---- */
    if ( path_type == MVS_PATH_TYPE_ROOT ) {
        dir_entry = dir_openlist_find_free();
        if ( !dir_entry ) {
            logmsg_error("NFSVF220E", "vfs_opendir: no free slot for root of export %d", export_idx);
            goto error;
        }
        dir_entry->status      = MVSVFS_DIR_OPENLIST_USED;
        dir_entry->dir_level   = MVS_PATH_TYPE_ROOT;
        dir_entry->export_idx  = export_idx;
        dir_entry->dataset_idx = -1;
        strncpy(dir_entry->pds_dsname_ebcdic, ebcdic_path,
            sizeof(dir_entry->pds_dsname_ebcdic) - 1);
        dir_entry->pds_dsname_ebcdic[sizeof(dir_entry->pds_dsname_ebcdic) - 1] = '\0';
        dir_entry->next_cookie                = 0;
        dir_entry->member_list.number_in_list = 0;
        dir_entry->end_of_dir_read            = 0;
        dir_entry->pds_fh                     = NULL;
        return dir_entry;
    }

    if ( path_type != MVS_PATH_TYPE_DATASET ) {
        errno = ENOENT;
        goto error;
    }

    /* ---- A PDS directory: resolve the real dsname and open it. ---- */
    ds = export_dataset_get(export_idx, dataset_idx);
    if ( ds == NULL ) {
        errno = ENOENT;
        goto error;
    }

    /* Fast path: reuse an existing non-expired slot for this PDS.      */
    /* If the full member list is already cached we skip disk I/O.      */
    /* If the cache is partial, reinitialise it for a fresh read.       */
    dir_entry = dir_openlist_find_by_dsname(ds->dsname_ebcdic);
    if ( dir_entry != NULL ) {
        if ( dir_entry->end_of_dir_read ) {
            /* Full cache hit -- all members known, no disk read needed */
            logmsg_debug("NFSVF230D", "vfs_opendir: Full cache hit for PDS %s, %d members cached",
                ds->dsname_ebcdic, dir_entry->member_list.number_in_list);
            return dir_entry;
        }
        /* Partial cache: re-open file handle and reset for fresh read.
           Close any handle still held first -- opening straight into
           pds_fh would overwrite it, losing the FILE and the DCB and
           allocation behind it.  It is normally NULL (vfs_closedir closed
           it), but that is nfs3.c's invariant, not one this function can
           see, so do not rely on it. */
        if (dir_entry->pds_fh != NULL) {
            logmsg_debug("NFSVF420D", "vfs_opendir: closing handle left open"
                      " on %s before re-opening", ds->dsname_ebcdic);
            mvs_close_pds_dir(dir_entry->pds_fh);
            dir_entry->pds_fh = NULL;
        }
        dir_entry->member_list.number_in_list = 0;
        dir_entry->end_of_dir_read            = 0;
        retcode = mvs_open_pds_dir(ds->dsname_ebcdic, export_idx, &dir_entry->pds_fh);
        if ( retcode == 0 && dir_entry->pds_fh ) {
            logmsg_debug("NFSVF240D", "vfs_opendir: Partial cache hit for PDS %s, reopened", ds->dsname_ebcdic);
            return dir_entry;
        }
        /* Re-open failed; fall through to allocate a fresh slot        */
        dir_entry = NULL;
    }

    /* Slow path: allocate a free (or LRU-evicted) pool slot.           */
    dir_entry = dir_openlist_find_free();
    if ( !dir_entry ) {
        logmsg_error("NFSVF250E", "vfs_opendir: no free open dir entry for %s", ds->dsname_ebcdic);
        goto error;
    }

    dir_entry->status      = MVSVFS_DIR_OPENLIST_USED;
    dir_entry->dir_level   = MVS_PATH_TYPE_DATASET;
    dir_entry->export_idx  = export_idx;
    dir_entry->dataset_idx = dataset_idx;
    strncpy(dir_entry->pds_dsname_ebcdic, ds->dsname_ebcdic,
        sizeof(dir_entry->pds_dsname_ebcdic));
    dir_entry->next_cookie                = 0;
    dir_entry->member_list.number_in_list = 0;
    dir_entry->end_of_dir_read            = 0;

    /* Open the PDS for the directory read */
    retcode = mvs_open_pds_dir(ds->dsname_ebcdic, export_idx, &dir_entry->pds_fh);
    if ( retcode < 0 || !dir_entry->pds_fh ) {
        logmsg_error("NFSVF260E", "vfs_opendir: Unable to open PDS %s for directory read", ds->dsname_ebcdic);
        dir_openlist_free(dir_entry);
        goto error;
    }

    logmsg_debug("NFSVF270D", "vfs_opendir: Opened PDS %s, pds_fh = 0x%08X",
        ds->dsname_ebcdic, dir_entry->pds_fh);

    return dir_entry;

error:
    logmsg_debug("NFSVF280D", "vfs_opendir: path=%s ending with error %s",
        log_ascii(path), strerror(errno));
    return NULL;
}

/* -------------------------------------------------------------------- */
/* root_readdir_next: enumerate the PDS directories of an export.       */
/*                                                                      */
/* The cookie is the ordinal index of the NEXT dataset to return, so    */
/* entry i returns cookie (i+1) and a cookie >= the dataset count means */
/* end of directory.  No disk I/O -- entries come from the dataset      */
/* provider (export_dataset_*), so a future dynamic list can be used.   */
/* -------------------------------------------------------------------- */
static int root_readdir_next(vfs_dir_t *dir_entry,
                             char *name, uint32_t maxname,
                             uint64_t *fileid, uint64_t *cookie)
{
    int            index;
    int            count;
    pds_dataset_t *ds;

    index = (int)(*cookie);        /* next dataset to return */
    count = export_dataset_count(dir_entry->export_idx);

    if (index < 0 || index >= count)
        return -1;                 /* end of directory */

    ds = export_dataset_get(dir_entry->export_idx, index);
    if (ds == NULL)
        return -1;

    strncpy(name, ds->dirname_ascii, maxname - 1);
    name[maxname - 1] = '\0';

    *fileid = mvs_fid_hash(ds->dsname_ebcdic, NULL);
    *cookie = (uint64_t)(index + 1);

    logmsg_debug("NFSVF290D", "root_readdir_next: entry %d = %s", index, ds->dirname_ebcdic);
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_readdir_next: read the next directory entry.                     */
/*                                                                      */
/* Fills name (NUL-terminated), *fileid (inode), and *cookie (1-based   */
/* position of this entry, for use as the NFS READDIR cookie).          */
/* Returns 0 on success, -1 at end of directory or error.               */
/* -------------------------------------------------------------------- */
int vfs_readdir_next(vfs_dir_t *dir_entry,
                     char *name, uint32_t maxname,
                     uint64_t *fileid,
                     uint64_t *cookie)
{
    uint8_t             load_from_dir = 0;
    char                member_name[9];
    pds_member_entry_t *last_cached;
    int                 retcode;
    pds_member_entry_t *member_info;

    /* The export root enumerates PDS directories, not PDS members. */
    if (dir_entry->dir_level == MVS_PATH_TYPE_ROOT)
        return root_readdir_next(dir_entry, name, maxname, fileid, cookie);

    //logmsg_debug("NFSVF300D", "vfs_readdir_next: path=%s cookie='%-8.8s' (0x%016llX)",
    //    dir_entry->pds_dsname_ebcdic, (char *)cookie, *cookie);

    /* Convert cookie back to EBCDIC string with null terminator */
    //from_cookie(dir_entry->next_cookie, member_name);
    from_cookie(*cookie, member_name);

    /* Decide whether or not we need to load more members from PDS DIR */
    if ( dir_entry->member_list.number_in_list == 0 ) {
        load_from_dir = 1;
    }
    /* We have cached members -- do we already hold the next one requested? */
    else {
        /* If the "cookie" is >= the last cached member name, then we need to read more */
        last_cached = &(dir_entry->member_list.list[
                            dir_entry->member_list.number_in_list - 1]);
        if (strcmp(member_name, last_cached->name) >= 0) {
            load_from_dir = 1;
        }
    }

    /* Load more member info from the PDS dir if needed */
    if ( load_from_dir ) {
        if (dir_entry->end_of_dir_read) {
            /* We already reached end of PDS directory ... nothing to load */
            logmsg_debug("NFSVF310D", "vfs_readdir_next: end of directory on path=%s", dir_entry->pds_dsname_ebcdic);
            retcode = -1;
            goto error_exit_no_log;
        }

        /* Rebuild the cache from member_name onward.  Empty it first so the
           append-based read cannot duplicate any earlier-cached entries. */
        dir_entry->member_list.number_in_list = 0;

        retcode = mvs_read_pds_dir(
            dir_entry->pds_fh,
            /* start-member */ member_name,
            /* member-list  */ &dir_entry->member_list,
            &(dir_entry->end_of_dir_read) );
        if ( retcode ) {
            logmsg_debug("NFSVF320D", "vfs_readdir_next: mvs_read_pds_dir returned error %d", retcode);
            retcode = -1;
            goto error_exit;
        }
    }

    /* Now we've loaded directory member info (or possible, nothing) search what we have */
    retcode = dir_openlist_search_members(
        dir_entry, member_name, SEARCH_OP_GT, &member_info);
    if ( retcode ) {
        logmsg_debug("NFSVF330D", "vfs_readdir_next: dir_openlist_search_members returned error %d", retcode);
        retcode = -1;
        goto error_exit;
    }

    if ( !member_info ) {
        /* No member found */
        logmsg_debug("NFSVF340D", "vfs_readdir_next: member not found");
        retcode = -1;
        goto error_exit;
    }

    /* Return the file name, generated from the member name found */
    /* This will translate the file name to ASCII                 */
    generate_file_name(dir_entry->export_idx, dir_entry->dataset_idx,
                       member_info, name, maxname);

    /* Return the generated fileId value */
    *fileid = mvs_fid_hash(
        dir_entry->pds_dsname_ebcdic,
        member_info->name ); /* generate fileid based on dataset and member name */

    /* Return and save the next cookie value */
    *cookie = to_cookie(member_info->name);
    dir_entry->next_cookie = *cookie;

    logmsg_debug("NFSVF350D", "vfs_readdir_next: Ending and returning filename %s for path %s",
        log_ascii(name), dir_entry->pds_dsname_ebcdic);

    return 0;

error_exit:

    logmsg_debug("NFSVF360D", "vfs_readdir_next: Returning error ... retcode %d on path=%s",
        retcode, dir_entry->pds_dsname_ebcdic);

error_exit_no_log:

    return retcode;

}

/* -------------------------------------------------------------------- */
/* vfs_seekdir_to: seek so the next vfs_readdir_next call returns the   */
/* entry AFTER the one that had the given cookie value.                 */
/* cookie=0 means start from the very beginning.                        */
/*                                                                      */
/* Implementation note: we rewind and read linearly through 'cookie'    */
/* entries.  This is O(n) per resumed READDIR page.  POSIX seekdir()    */
/* / telldir() would allow O(1) seeking but only if the DIR* handle     */
/* is kept open between calls, which conflicts with the static-pool     */
/* no-malloc design.  For the typical case (small directories or        */
/* single-page listings) this cost is negligible.                       */
/* -------------------------------------------------------------------- */
void vfs_seekdir_to(vfs_dir_t *d, uint64_t cookie)
{
    uint64_t skip = cookie;
    d->next_cookie = cookie;
}

/* -------------------------------------------------------------------- */
/* vfs_closedir: release a directory handle back to the pool.           */
/* The pool slot remains USED so its member cache stays available for   */
/* mvsvfs_find_cached_member(); only the FILE handle is closed.         */
/* -------------------------------------------------------------------- */
void vfs_closedir(vfs_dir_t *dir_entry)
{
    logmsg_debug("NFSVF370D", "vfs_closedir: path=%s", dir_entry->pds_dsname_ebcdic);

    if ( dir_entry->pds_fh ) {
        mvs_close_pds_dir(dir_entry->pds_fh);
        dir_entry->pds_fh = NULL;
    }
}

/* -------------------------------------------------------------------- */
/* mvsvfs_find_cached_member: look up a member entry in the open        */
/* directory cache.                                                     */
/*                                                                      */
/* Parameters:                                                          */
/*   pds_dsname   -- EBCDIC dataset name to locate in the pool          */
/*   member_name  -- EBCDIC member name to find within that directory   */
/*   member_entry -- set to the address of the matching cache entry     */
/*                   on success; unchanged on failure                   */
/*                                                                      */
/* Returns 0 on success, -1 if the pool entry was not found (or has     */
/* timed out) or if no matching member exists in the cached directory.  */
/* -------------------------------------------------------------------- */
int mvsvfs_find_cached_member(
    const char          *pds_dsname,
    const char          *member_name,
    pds_member_entry_t **member_entry)
{
    vfs_dir_t *dir;
    int        retcode;
    clock_t    t_start;

    t_start = clock();
    dir = dir_openlist_find_by_dsname(pds_dsname);
    if (dir == NULL) {
        logmsg_debug("NFSVF380D", "mvsvfs_find_cached_member: No valid"
                     " vfs_dir_t pool entry found for PDS %s", pds_dsname);
        mvsprf_record(PERF_MVSPOOL_MISS, clock() - t_start);
        return -1;
    }

    retcode = dir_openlist_search_members(
        dir, member_name, SEARCH_OP_EQ, member_entry);
    if ( retcode != 0 ) {
        logmsg_debug("NFSVF390D", "mvsvfs_find_cached_member: Member entry"
                     " not found for PDS %s member %s",
                     pds_dsname, member_name);
        mvsprf_record(PERF_MVSPOOL_MISS, clock() - t_start);
    } else {
        logmsg_debug("NFSVF400D", "mvsvfs_find_cached_member: Cached member"
                     " entry found for PDS %s member %s",
                     pds_dsname, member_name);
        mvsprf_record(PERF_MVSPOOL_HIT, clock() - t_start);
    }
    return retcode;
}
