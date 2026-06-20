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
 * The struct vfs_dir type is defined here (opaque to the rest of the
 * code) so that directory iteration can use OS-native constructs.
 *
 * The static dir_pool avoids malloc() for maximum portability.
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
#include "mvsvfs.h"
#include "logger.h"

#define PATH_SEPARATOR_ASCII  (char)0x2f  
#define PATH_SEPARATOR_EBCDIC (char)0x61 

#define MVSVFS_DIR_OPENLIST_USED             1
#define MVSVFS_DIR_OPENLIST_FREE             0

/* -------------------------------------------------------------------- */
/* Directory handle type (opaque outside this file)                     */
/* -------------------------------------------------------------------- */
struct vfs_dir {
    uint8_t             status; 
    int                 export_idx;
    FILE               *pds_fh;                /* File handle for open dir */
    char                pds_dsname_ebcdic[45]; /* EBCDIC dataset name, from path */   
    uint64_t            next_cookie;           /* 1-based index of the next entry to return */
    int                 pds_entries_cached;    /* Number of valid entries in the PDS member cache */      
    pds_member_entry_t  members[MVSVFS_PDS_DIR_CACHE_SIZE]; /* Pre-read PDS member entries */
    int                 end_of_dir_read;       /* 1 if we've hit the end of the directory */
};
 
/* Static pool of directory handles -- no malloc required */
static vfs_dir_t g_dir_pool[MAX_OPEN_DIRS];

/* -------------------------------------------------------------------- */
/* vfs_stat: fill a vfs_stat_t as appropriate for a PDS dataset         */
/* or a PDS member.                                                     */
/* Returns 0 on success, -1 on error (errno set).                       */
/* -------------------------------------------------------------------- */
static int vfs_stat_pds_member(const char *path, int export_idx, vfs_stat_t *vs)
{
    pds_member_entry_t  *member_entry;
    char                 pds_dsname[45];
    char                 pds_member_name[9];

    /* Split the path into dataset and member name. */
    mvs_get_pds_dsn_and_member(path, pds_dsname, pds_member_name, export_idx);


    member_entry = mvs_pds_get_member_entry(pds_dsname, pds_member_name, export_idx);
    if (member_entry == NULL) {
        errno = ENOENT;
        return -1;  
    }

    vs->ftype = NF3REG;
    vs->mode = 0444; /* read-only permissions for everyone */
    vs->nlink = 1;   /* convention for files: one link from parent directory */
    vs->uid = 0;     /* root-owned */
    vs->gid = 0;     /* root-owned */
    vs->size = member_entry->size;
    vs->used = member_entry->size; /* for simplicity, assume used space equals size */
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

static int vfs_stat_dataset(const char *path, int export_idx, vfs_stat_t *vs)
{
    struct timeval tv;

    vs->ftype = NF3DIR;
    vs->mode = 0555; /* read/execute permissions for everyone */
    vs->nlink = 2;   /* convention for directories: link from parent and self-link */
    vs->uid = 0;     /* root-owned */
    vs->gid = 0;     /* root-owned */
    vs->size = 4096; /* arbitrary non-zero size for the directory itself */
    vs->used = 4096; /* arbitrary non-zero disk usage for the directory itself */
    vs->rdev_maj = 0;
    vs->rdev_min = 0;
    vs->fsid = (uint64_t)(export_idx + 1); /* unique filesystem ID based on export index */
    // Here we generate the fileid for the dataset, based on dataset name.
    // We'll use this as the basis for the raw_dev/raw_ino fields.
    vs->fileid  = mvs_fid_hash(path, NULL);
    vs->raw_ino = mvs_fid_ino32(path, NULL);
    vs->raw_dev = (uint32_t)export_idx + 1; 

    // Now the accessed/modified/created date/times.
    // For simplicity, we'll set these all to the same value based on the current time.
    gettimeofday(&tv, NULL);
    vs->atime_sec = (uint32_t)tv.tv_sec;
    vs->atime_nsec = (uint32_t)(tv.tv_usec * 1000); // convert microseconds to nanoseconds
    vs->mtime_sec = vs->atime_sec;
    vs->mtime_nsec = vs->atime_nsec;
    vs->ctime_sec = vs->atime_sec;      
    vs->ctime_nsec = vs->atime_nsec;

    return 0;
}

void dump_stat_result(const char *path, int rc, vfs_stat_t *vs) {
    log_debug("vfs_stat: Result for path=%s ending retcode=%d", 
        log_ascii(path), rc);
    log_debug("vfs_stat:      vs->size      = %d", vs->size);
    log_debug("vfs_stat:      vs->used      = %d", vs->used);
    log_debug("vfs_stat:      vs->fsid      = %d", vs->fsid);
    log_debug("vfs_stat:      vs->fileid    = 0x%016X", vs->fileid);
    log_debug("vfs_stat:      vs->raw_ino   = 0x%08X", vs->raw_ino);
}

int vfs_stat(const char *path, vfs_stat_t *vs)
{
    int export_idx;
    char ebcdic_path[MAX_PATH_LEN];
    int path_type;
    int retcode;

    log_debug("vfs_stat: path=%s", log_ascii(path));

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Is this path a directory or a member of a PDS */
    path_type = mvs_path_type(ebcdic_path, &export_idx);

    if (path_type == MVS_PATH_TYPE_PDS_MEMBER) {
        retcode = vfs_stat_pds_member(ebcdic_path, export_idx, vs);
        dump_stat_result(path, retcode, vs);
        return retcode;
    } else if (path_type == MVS_PATH_TYPE_DATASET) {
        retcode = vfs_stat_dataset(ebcdic_path, export_idx, vs);
        dump_stat_result(path, retcode, vs);
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
    int         rc;
    char        ebcdic_path[MAX_PATH_LEN];
    char        pds_dsname[45];
    char        pds_member_name[9];
    int         export_idx; 
    int         path_type;

    log_debug("vfs_pread: path=%s, count=%d, offset=%d", 
        log_ascii(path), count, offset);

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Is this path to a directory or a member of a PDS */
    path_type = mvs_path_type(ebcdic_path, &export_idx);

    if (path_type == MVS_PATH_TYPE_DATASET) {
        errno = EACCES;
        return -1;
    } else if ( path_type != MVS_PATH_TYPE_PDS_MEMBER ){
        errno = ENOENT;
        return -1;
    }

    /* Split the path into dataset and member name. */
    mvs_get_pds_dsn_and_member(path, pds_dsname, pds_member_name, export_idx);

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
/* Initialize the vfs_dir_t pool/list                                   */
/* -------------------------------------------------------------------- */
void dir_openlist_init() {
    memset(g_dir_pool, 0, sizeof(g_dir_pool));
}

/* -------------------------------------------------------------------- */
/* Find a vfs_dir_t entry which is not being used                       */
/* -------------------------------------------------------------------- */
vfs_dir_t *dir_openlist_find_free() 
{
    int i;

    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        if (g_dir_pool[i].status == MVSVFS_DIR_OPENLIST_FREE) {
            memset(&g_dir_pool[i], 0, sizeof(vfs_dir_t));
            return &g_dir_pool[i];
        }
    }
    /* None free */
    errno = EMFILE;
    return NULL;
}

/* -------------------------------------------------------------------- */
/* Mark the given entry as free and clean it up.                        */
/* -------------------------------------------------------------------- */
void dir_openlist_free(vfs_dir_t *entry) {
    memset(entry, 0, sizeof(vfs_dir_t));
    entry->status = MVSVFS_DIR_OPENLIST_FREE;
}

/* -------------------------------------------------------------------- */
/* Binary search of member info list                                    */
/* -------------------------------------------------------------------- */

//#define SEARCH_MEMBER_LT        1
//#define SEARCH_MEMBER_LE        2
//#define SEARCH_MEMBER EQ        3
//#define SEARCH_MEMBER_GE        4
#define SEARCH_MEMBER_GT        5

int dir_openlist_search_members(
    vfs_dir_t          *dir_entry,
    const char         *member_name,
    int                 operator, /* Ignored, because we've only implemented GT at this time */
    pds_member_entry_t **member_info)
{
    int  num_in_list = dir_entry->pds_entries_cached;
    int  slice_low, slice_high, mid_point;
    int  cmp_result;

    *member_info = NULL;
    if (num_in_list == 0)
        return -1;

    slice_low  = 0;
    slice_high = num_in_list - 1;

    while (slice_low <= slice_high) {
        mid_point  = (slice_low + slice_high) / 2;
        cmp_result = strcmp(dir_entry->members[mid_point].name, member_name);

        if (cmp_result <= 0) {
            /* mid_point is <= search key: answer must be above it */
            slice_low = mid_point + 1;
        } else {
            /* mid_point is a candidate (GT); try to find a lower one */
            slice_high = mid_point - 1;
        }
    }

    /*
     * slice_low now points to the leftmost entry whose name is
     * strictly greater than member_name, or is out of range if
     * no such entry exists.
     */
    if (slice_low < num_in_list) {
        *member_info = &(dir_entry->members[slice_low]);
        return 0;
    }
    return -1;  /* All entries are <= member_name */
}
#if 0
// This is from Claude ...
/* Operator bitmask values */
#define OP_LT   1   /* <  : strictly less than    */
#define OP_EQ   2   /* =  : equal                 */
#define OP_GT   4   /* >  : strictly greater than */
#define OP_LE   (OP_LT | OP_EQ)   /* 3: <= */
#define OP_GE   (OP_GT | OP_EQ)   /* 6: >= */

int dir_openlist_search_members(
    vfs_dir_t          *dir_entry,
    const char         *member_name,
    int                 operator,
    pds_member_entry_t **member_info)
{
    int  num_in_list = dir_entry->pds_entries_cached;
    int  slice_low, slice_high, mid_point;
    int  cmp_result;

    *member_info = NULL;
    if (num_in_list == 0)
        return -1;

    slice_low  = 0;
    slice_high = num_in_list - 1;

    while (slice_low <= slice_high) {
        mid_point  = (slice_low + slice_high) / 2;
        cmp_result = strcmp(dir_entry->members[mid_point].name, member_name);

        if (cmp_result < 0) {
            slice_low = mid_point + 1;
        } else if (cmp_result > 0) {
            slice_high = mid_point - 1;
        } else {
            /* Exact match at mid_point */
            if (operator & OP_EQ) {
                *member_info = &(dir_entry->members[mid_point]);
                return 0;
            }
            /* Exact match but operator doesn't include EQ:
               nudge the search to the appropriate side */
            if (operator & OP_GT)
                slice_low  = mid_point + 1;  /* want something strictly after */
            else
                slice_high = mid_point - 1;  /* want something strictly before */
            break;
        }
    }

    /*
     * No exact match found (or exact match excluded by operator).
     * slice_low is the insertion point:
     *   members[slice_low-1].name  <  member_name
     *   members[slice_low].name    >  member_name
     */
    if (operator & OP_GT) {
        /* Leftmost entry strictly greater than member_name */
        if (slice_low < num_in_list) {
            *member_info = &(dir_entry->members[slice_low]);
            return 0;
        }
    } else if (operator & OP_LT) {
        /* Rightmost entry strictly less than member_name */
        if (slice_low > 0) {
            *member_info = &(dir_entry->members[slice_low - 1]);
            return 0;
        }
    }

    return -1;
}
#endif 

/* -------------------------------------------------------------------- */
/* Translate ASCII path name into EBCDIC and extract DSN and mem name   */
/* -------------------------------------------------------------------- */
static int path_to_dsn_member(
    const char *path,
    int expected_path_type,
    int *actual_path_type,
    char *pds_dsname,
    char *pds_member_name,
    int  *export_idx )
{
    char        ebcdic_path[MAX_PATH_LEN];
    int         path_type;

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    path_type = mvs_path_type(ebcdic_path, export_idx);
    *actual_path_type = path_type;

    if ( path_type != expected_path_type ) {
        return -1;
    }

    mvs_get_pds_dsn_and_member(ebcdic_path, pds_dsname, pds_member_name, *export_idx);
    return 0;

}

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
    pds_member_entry_t  *member_info, 
    char                *file_name_buffer,
    int                  buflen)
{
    int i;
    export_t *exp;

    exp = exports_get(export_idx);

    strncpy(file_name_buffer, member_info->name, buflen);
    for (i = 0; i < strlen(file_name_buffer); i++)
        file_name_buffer[i] = tolower(file_name_buffer[i]);

    if ( strlen(exp->file_ext) > 0 ) {
        strncat(file_name_buffer, ".", buflen);
        strncat(file_name_buffer, exp->file_ext, buflen);
    }

    ebcdic_to_ascii(file_name_buffer, file_name_buffer, strlen(file_name_buffer));
}

/* -------------------------------------------------------------------- */
/* vfs_opendir: open a directory for iteration.                         */
/* Returns a handle from the static pool, or NULL on error.             */
/* -------------------------------------------------------------------- */
vfs_dir_t *vfs_opendir(const char *path, uint64_t cookie)
{
    vfs_dir_t  *dir_entry;
    char        ebcdic_path[MAX_PATH_LEN];
    char        pds_dsname[45];
    char        pds_member_name[9];
    int         export_idx; 
    int         end_of_dir;
    int         num_of_members_returned;
    int         retcode;
    int         path_type;

    log_debug("vfs_opendir: path=%s", log_ascii(path));

    retcode = path_to_dsn_member(
        path, MVS_PATH_TYPE_DATASET,
        &path_type, pds_dsname, pds_member_name, &export_idx);

    log_debug("vfs_opendir: path %s, type = %d", log_ascii(path), path_type);

    if ( retcode < 0 ) {
        errno = ENOENT;
        goto error;
    }

    /* Get free open directory entry */
    dir_entry = dir_openlist_find_free();
    if ( !dir_entry ) {
        log_error("vfs_opendir: Unable to find free open directory entry for ", pds_dsname);
        goto error;
    }

    dir_entry->status = MVSVFS_DIR_OPENLIST_USED;
    dir_entry->export_idx = export_idx;
    strncpy(dir_entry->pds_dsname_ebcdic, pds_dsname, 
        sizeof(dir_entry->pds_dsname_ebcdic));
    dir_entry->next_cookie = 0;

    /* Open the PDS for the directory read */
    retcode = mvs_open_pds_dir(
            pds_dsname, export_idx,
            &dir_entry->pds_fh);
    if ( retcode < 0 || !dir_entry->pds_fh ) {
        log_error("vfs_opendir: Unable to open PDS %s for directory read", pds_dsname);
        dir_openlist_free(dir_entry);
        goto error;
    }

    dir_entry->pds_entries_cached = 0;
    dir_entry->end_of_dir_read = 0;

    log_debug("vfs_opendir: path=%s completed without error, pds_fh = 0x%08X",
        pds_dsname, dir_entry->pds_fh);

    return dir_entry;

error:
    log_debug("vfs_opendir: path=%s ending with error %s",
        log_ascii(path), strerror(errno));
    return NULL;
}

#if 0
    retcode = mvs_pds_member_list(
        pds_dsname, export_idx, /* start-member */ NULL,
        /* max-members */ MVSVFS_PDS_DIR_CACHE_SIZE,
        /* member-entries */ dir_openlist_entry->members,
        &num_of_members_returned,
        &end_of_dir);
#endif

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
    uint8_t             load_from_dir;
    char                member_name[9];
    pds_member_entry_t *last_cached;
    int                 retcode;
    pds_member_entry_t *member_info;

    log_debug("vfs_readdir_next: path=%s", dir_entry->pds_dsname_ebcdic);

    /* Convert cookie back to EBCDIC string with null terminator */
    from_cookie(dir_entry->next_cookie, member_name);

    /* Decide whether or not we need to load more members from PDS DIR */
    if ( dir_entry->pds_entries_cached == 0 )
        load_from_dir = 1;
    /* We have cached members but do potentially have the next one as requested? */
    else {
        last_cached = &(dir_entry->members[dir_entry->pds_entries_cached]);
        /* If the "cookie" is >= the last cached member name, then we need to read more */
        if (strcmp(member_name, last_cached->name) >= 0) {
            /* No we don't */
            load_from_dir = 1;
        }
    }

    /* Load more member info from the PDS dir if needed */    
    if ( load_from_dir ) {
        if (dir_entry->end_of_dir_read) {
            /* We already reached end of PDS directory ... nothing to load */
            return -1;
        }

        retcode = mvs_read_pds_dir(
            dir_entry->pds_fh,
            /* start-member */ member_name,
            /* max-members */ MVSVFS_PDS_DIR_CACHE_SIZE,
            /* member-entries */ dir_entry->members,
            &(dir_entry->pds_entries_cached),
            &(dir_entry->end_of_dir_read) );
        if ( retcode )
            return -1;
    }

    /* Now we've loaded directory member info (or possible, nothing) search what we have */
    retcode = dir_openlist_search_members(
        dir_entry, member_name, SEARCH_MEMBER_GT, &member_info);
    if ( retcode )
        /* Error */
        return -1;

    if ( !member_info ) {
        /* No member found */
        return -1;
    }

    /* Return the file name, generated from the member name found */
    /* This will translate the file name to ASCII                 */
    generate_file_name(dir_entry->export_idx, member_info, name, maxname);

    /* Return the generated fileId value */
    *fileid = mvs_fid_hash(
        dir_entry->pds_dsname_ebcdic, 
        member_info->name ); /* generate fileid based on dataset and member name */

    /* Return and save the next cookie value */
    *cookie = to_cookie(member_info->name);
    dir_entry->next_cookie = *cookie;

    log_debug("vfs_readdir_next: path %s, file-name %s", 
        dir_entry->pds_dsname_ebcdic, log_ascii(name));
        
    return 0;
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
/* -------------------------------------------------------------------- */
void vfs_closedir(vfs_dir_t *dir_entry)
{
    log_debug("vfs_closedir: path=%s", dir_entry->pds_dsname_ebcdic);

    mvs_close_pds_dir(dir_entry->pds_fh);
    dir_openlist_free(dir_entry);
}
