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

#include "ebcdic.h"
#include "mvsfid.h"
#include "mvsio.h"
#include "mvspdir.h"
#include "mvsprw.h"
#include "nfsd.h"
#include "hexdump.h"
#include "mvsfsz.h"
#include "mvsprf.h"
#include "mvsvfs.h"
#include "mvsdol.h"
#include "logger.h"

#define PATH_SEPARATOR_ASCII  (char)0x2f  
#define PATH_SEPARATOR_EBCDIC (char)0x61 

/* -------------------------------------------------------------------- */
/* vfs_stat: fill a vfs_stat_t as appropriate for a PDS dataset         */
/* or a PDS member.                                                     */
/* Returns 0 on success, -1 on error (errno set).                       */
/* -------------------------------------------------------------------- */
static uint64_t vfs_stat_member_size_calc(
    pds_dataset_t *ds,
    int            member_size)
{
    mvs_dcb_info_t  *dcb;
    uint64_t         calc_estimated_size;

    dcb = &ds->dcbinfo;

    if (dcb->recfm & MVS_DCB_RECFM_F ) {
        calc_estimated_size = member_size * (1 + dcb->lrecl);
    } else if (dcb->recfm & MVS_DCB_RECFM_V ) {
        calc_estimated_size = member_size * (1 + dcb->lrecl / 2);
    } else {
        calc_estimated_size = 4096;
    }

    return calc_estimated_size;
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

    /* Split the path into dataset and member name. */
    mvs_get_pds_dsn_and_member(path, pds_dsname, pds_member_name, export_idx);

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

    /* If above fails ... then we'll estimate the size, but this is problematic for the NFS client */
    /* which will attempt to read a file based on the file size that was given in a getattr or     */
    /* lookup operation.                                                                           */
    if ( rc2 != 0 )
        set_size = vfs_stat_member_size_calc(ds, member_entry->size);

    vs->ftype = NF3REG;
    vs->mode = 0777; /* read/write/execute permissions for everyone */
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

static int vfs_stat_dataset(const char *path, int export_idx,
                            int dataset_idx, vfs_stat_t *vs)
{
    pds_dataset_t *ds;

    (void)path;

    ds = export_dataset_get(export_idx, dataset_idx);
    if (ds == NULL) {
        errno = ENOENT;
        return -1;
    }

    (void)vfs_dir_epoch();   /* ensure g_dir_epoch is initialised */

    vs->ftype = NF3DIR;
    vs->mode = 0777; /* read/write/execute permissions for everyone */
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

    // Now the accessed/modified/created date/times.
    // These MUST be stable across calls (see g_dir_epoch above): a varying
    // directory mtime/ctime makes the NFS client restart readdir from cookie 0.
    // nsec is forced to 0 to avoid any sub-second jitter being reported.
    vs->atime_sec = (uint32_t)g_dir_epoch;
    vs->atime_nsec = 0;
    vs->mtime_sec = (uint32_t)g_dir_epoch;
    vs->mtime_nsec = 0;
    vs->ctime_sec = (uint32_t)g_dir_epoch;
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
    vs->mode = 0777;
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

    log_debug("vfs_stat: Result for path=%s ending retcode=%d",
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

        log_debug("vfs_stat:      vs->mode      = %04o",             vs->mode);
        log_debug("vfs_stat:      vs->size      = %lld",             vs->size);
        log_debug("vfs_stat:      vs->used      = %lld",             vs->used);
        log_debug("vfs_stat:      vs->fsid      = %lld",             vs->fsid);
        log_debug("vfs_stat:      vs->fileid    = 0x%016X",          vs->fileid);
        log_debug("vfs_stat:      vs->raw_ino   = 0x%08X",           vs->raw_ino);
        log_debug("vfs_stat:      vs->atime_sec = 0x%08X (%s UTC)",  vs->atime_sec, atime_buf);
        log_debug("vfs_stat:      vs->mtime_sec = 0x%08X (%s UTC)",  vs->mtime_sec, mtime_buf);
        log_debug("vfs_stat:      vs->ctime_sec = 0x%08X (%s UTC)",  vs->ctime_sec, ctime_buf);
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

    log_debug("vfs_stat: path=%s", log_ascii(path));

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
    log_debug("vfs_stat: Result error ... returning -1");
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


    log_debug("vfs_pread: path=%s, count=%d, offset=%lld", 
        log_ascii(path), count, offset);

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Is this path to a directory or a member of a PDS */
    path_type = mvs_path_type(ebcdic_path, &export_idx, NULL);
    log_debug("vfs_pread: mvs_path_type returned type = %d, export_idx = %d",
        path_type, export_idx);
    if (path_type == MVS_PATH_TYPE_DATASET || path_type == MVS_PATH_TYPE_ROOT) {
        errno = EACCES;
        return -1;
    } else if ( path_type != MVS_PATH_TYPE_PDS_MEMBER ){
        errno = ENOENT;
        return -1;
    }

    /* Split the path into dataset and member name. */
    mvs_get_pds_dsn_and_member(ebcdic_path, pds_dsname, pds_member_name, export_idx);

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

    log_debug("vfs_pread: Completed path=%s, nread=%d, eof=%d", 
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
    errno = EACCES;
    return -1;

#if 0
    int     fd;
    ssize_t n;
    int     saved_errno;
 
    fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
 
    n = pwrite(fd, buf, (size_t)count, (off_t)offset);
    saved_errno = errno;          /* preserve before fsync/close can overwrite */
    if (n == (ssize_t)count) fsync(fd);
    close(fd);
    errno = saved_errno;
 
    return (n == (ssize_t)count) ? 0 : -1;
#endif
}
 
/* -------------------------------------------------------------------- */
/* vfs_create: create a new empty file (or truncate if it exists).      */
/* mode is the permission bits (e.g. 0644).                             */
/* Returns 0 on success, -1 on error.                                   */
/* -------------------------------------------------------------------- */
int vfs_create(const char *path, uint32_t mode)
{
    errno = EACCES;
    return -1;

#if 0
    int fd = open(path,
                  O_CREAT | O_WRONLY | O_TRUNC,
                  (mode_t)mode);
    if (fd < 0) return -1;
    close(fd);
    return 0;
#endif
}
 
/* -------------------------------------------------------------------- */
/* vfs_remove: delete a regular file.                                   */
/* -------------------------------------------------------------------- */
int vfs_remove(const char *path)
{
    errno = EACCES;
    return -1;

#if 0
    return unlink(path);
#endif
}
 
/* -------------------------------------------------------------------- */
/* vfs_rename: rename / move a file.                                    */
/* -------------------------------------------------------------------- */
int vfs_rename(const char *from, const char *to)
{
    errno = EACCES;
    return -1;

#if 0
    return rename(from, to);
#endif
}
 
/* -------------------------------------------------------------------- */
/* vfs_truncate: change file size to 'size' bytes.                      */
/* -------------------------------------------------------------------- */
int vfs_truncate(const char *path, uint64_t size)
{
    errno = EACCES;
    return -1;

#if 0
    return truncate(path, (off_t)size);
#endif
}
 
/* -------------------------------------------------------------------- */
/* vfs_set_times: set atime and/or mtime on path.                       */
/* set_atime / set_mtime use the SET_* constants from nfsd.h:           */
/*   SET_DONT_CHANGE      - leave this timestamp unchanged              */
/*   SET_TO_SERVER_TIME   - set to the server's current time            */
/*   SET_TO_CLIENT_TIME   - set to the supplied sec/nsec values         */
/*                                                                      */
/* Uses utimensat(2) with UTIME_OMIT / UTIME_NOW so each timestamp is   */
/* handled independently without having to stat the file first.         */
/* -------------------------------------------------------------------- */
int vfs_set_times(const char *path,
                  int set_atime, uint32_t atime_sec, uint32_t atime_nsec,
                  int set_mtime, uint32_t mtime_sec, uint32_t mtime_nsec)
{
    errno = EACCES;
    return -1;

#if 0
    struct timespec ts[2];
 
    ts[0].tv_sec  = (set_atime == SET_TO_CLIENT_TIME) ? (time_t)atime_sec : 0;
    ts[0].tv_nsec = (set_atime == SET_DONT_CHANGE)   ? UTIME_OMIT  :
                    (set_atime == SET_TO_SERVER_TIME) ? UTIME_NOW   :
                                                        (long)atime_nsec;
 
    ts[1].tv_sec  = (set_mtime == SET_TO_CLIENT_TIME) ? (time_t)mtime_sec : 0;
    ts[1].tv_nsec = (set_mtime == SET_DONT_CHANGE)   ? UTIME_OMIT  :
                    (set_mtime == SET_TO_SERVER_TIME) ? UTIME_NOW   :
                                                        (long)mtime_nsec;
 
    return utimensat(AT_FDCWD, path, ts, 0);
#endif
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
 
/* -------------------------------------------------------------------- */
/* vfs_errno_to_nfs3: map a POSIX errno to an NFSv3 error code.         */
/* -------------------------------------------------------------------- */
uint32_t vfs_errno_to_nfs3(int err)
{
    switch (err) {
        case 0:             return NFS3_OK;
//        case EPERM:         return NFS3ERR_PERM;
        case ENOENT:        return NFS3ERR_NOENT;
        case EIO:           return NFS3ERR_IO;
//        case ENXIO:         return NFS3ERR_NXIO;
        case EACCES:        return NFS3ERR_ACCES;
        case EEXIST:        return NFS3ERR_EXIST;
//        case EXDEV:         return NFS3ERR_XDEV;
//        case ENODEV:        return NFS3ERR_NODEV;
        case ENOTDIR:       return NFS3ERR_NOTDIR;
        case EISDIR:        return NFS3ERR_ISDIR;
        case EINVAL:        return NFS3ERR_INVAL;
//        case EFBIG:         return NFS3ERR_FBIG;
        case ENOSPC:        return NFS3ERR_NOSPC;
//        case EROFS:         return NFS3ERR_ROFS;
//        case EMLINK:        return NFS3ERR_MLINK;
        case ENAMETOOLONG:  return NFS3ERR_NAMETOOLONG;
        case ENOTEMPTY:     return NFS3ERR_NOTEMPTY;
        case EDQUOT:        return NFS3ERR_DQUOT;
        default:            return NFS3ERR_IO;
    }
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

    strncpy(file_name_buffer, member_info->name, buflen);
    for (i = 0; i < strlen(file_name_buffer); i++)
        file_name_buffer[i] = tolower(file_name_buffer[i]);

    if ( ds != NULL && strlen(ds->file_ext) > 0 ) {
        strncat(file_name_buffer, ".", buflen);
        strncat(file_name_buffer, ds->file_ext, buflen);
    }

    ebcdic_to_ascii(file_name_buffer, file_name_buffer, strlen(file_name_buffer));
}

/* -------------------------------------------------------------------- */
/* vfs_opendir: open a directory for iteration.                         */
/* Returns a handle from the static pool, or NULL on error.             */
/*                                                                      */
/* Fast path: if a non-expired pool slot already holds a complete       */
/* member cache (end_of_dir_read=1) for this PDS, return it directly   */
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

    log_debug("vfs_opendir: Starting with path=%s cookie=0x%016llX",
        log_ascii(path), cookie);

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    path_type = mvs_path_type(ebcdic_path, &export_idx, &dataset_idx);
    log_debug("vfs_opendir: path %s, type = %d", log_ascii(path), path_type);

    /* ---- Export root: a virtual directory listing the PDS dirs. ---- */
    if ( path_type == MVS_PATH_TYPE_ROOT ) {
        dir_entry = dir_openlist_find_free();
        if ( !dir_entry ) {
            log_error("vfs_opendir: no free slot for root of export %d", export_idx);
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
            log_debug("vfs_opendir: Full cache hit for PDS %s, %d members cached",
                ds->dsname_ebcdic, dir_entry->member_list.number_in_list);
            return dir_entry;
        }
        /* Partial cache: re-open file handle and reset for fresh read  */
        dir_entry->member_list.number_in_list = 0;
        dir_entry->end_of_dir_read            = 0;
        retcode = mvs_open_pds_dir(ds->dsname_ebcdic, export_idx, &dir_entry->pds_fh);
        if ( retcode == 0 && dir_entry->pds_fh ) {
            log_debug("vfs_opendir: Partial cache hit for PDS %s, reopened", ds->dsname_ebcdic);
            return dir_entry;
        }
        /* Re-open failed; fall through to allocate a fresh slot        */
        dir_entry = NULL;
    }

    /* Slow path: allocate a free (or LRU-evicted) pool slot.           */
    dir_entry = dir_openlist_find_free();
    if ( !dir_entry ) {
        log_error("vfs_opendir: no free open dir entry for %s", ds->dsname_ebcdic);
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
        log_error("vfs_opendir: Unable to open PDS %s for directory read", ds->dsname_ebcdic);
        dir_openlist_free(dir_entry);
        goto error;
    }

    log_debug("vfs_opendir: Opened PDS %s, pds_fh = 0x%08X",
        ds->dsname_ebcdic, dir_entry->pds_fh);

    return dir_entry;

error:
    log_debug("vfs_opendir: path=%s ending with error %s",
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

    log_debug("root_readdir_next: entry %d = %s", index, ds->dirname_ebcdic);
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

    //log_debug("vfs_readdir_next: path=%s cookie='%-8.8s' (0x%016llX)",
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
            log_debug("vfs_readdir_next: end of directory on path=%s", dir_entry->pds_dsname_ebcdic);
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
            log_debug("vfs_readdir_next: mvs_read_pds_dir returned error %d", retcode);
            retcode = -1;
            goto error_exit;
        }
    }

    /* Now we've loaded directory member info (or possible, nothing) search what we have */
    retcode = dir_openlist_search_members(
        dir_entry, member_name, SEARCH_OP_GT, &member_info);
    if ( retcode ) {
        log_debug("vfs_readdir_next: dir_openlist_search_members returned error %d", retcode);
        retcode = -1;
        goto error_exit;
    }

    if ( !member_info ) {
        /* No member found */
        log_debug("vfs_readdir_next: member not found");
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

    log_debug("vfs_readdir_next: Ending and returning filename %s for path %s", 
        log_ascii(name), dir_entry->pds_dsname_ebcdic);
        
    return 0;

error_exit:

    log_debug("vfs_readdir_next: Returning error ... retcode %d on path=%s", 
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
    log_debug("vfs_closedir: path=%s", dir_entry->pds_dsname_ebcdic);

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
/* Returns 0 on success, -1 if the pool entry was not found (or has    */
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
        log_debug("mvsvfs_find_cached_member: No valid vfs_dir_t pool entry found for PDS %s", pds_dsname);
        mvsprf_record(PERF_MVSPOOL_MISS, clock() - t_start);
        return -1;
    }

    retcode = dir_openlist_search_members(
        dir, member_name, SEARCH_OP_EQ, member_entry);
    if ( retcode != 0 ) {
        log_debug("mvsvfs_find_cached_member: Member entry not found for PDS %s member %s", pds_dsname, member_name);
        mvsprf_record(PERF_MVSPOOL_MISS, clock() - t_start);
    } else {
        log_debug("mvsvfs_find_cached_member: Cached member entry found for PDS %s member %s", pds_dsname, member_name);
        mvsprf_record(PERF_MVSPOOL_HIT, clock() - t_start);
    }
    return retcode;
}
