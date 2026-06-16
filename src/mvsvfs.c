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
 

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "ebcdic.h"
#include "mvsio.h"
#include "nfsd.h"
#include "hexdump.h"

#define PATH_SEPARATOR_ASCII  (char)0x2f  
#define PATH_SEPARATOR_EBCDIC (char)0x61 

#define MVSVFS_DIR_OPENLIST_USED             1
#define MVSVFS_DIR_OPENLIST_FREE             0

/* -------------------------------------------------------------------- */
/* Directory handle type (opaque outside this file)                     */
/* -------------------------------------------------------------------- */
struct vfs_dir {
    uint8_t             status; 
    char                pds_dsname_ebcdic[45]; /* EBCDIC dataset name, from path */   
    uint64_t            next_cookie;           /* 1-based index of the next entry to return */
    int                 pds_entries_cached;    /* Number of valid entries in the PDS member cache */      
    pds_member_entry_t  members[MVSVFS_PDS_DIR_CACHE_SIZE]; /* Pre-read PDS member entries */
    uint8_t             end_of_dir_read;       /* 1 if we've hit the end of the directory */
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
    pds_member_entry_t entry *member_entry;
    char pds_dsname[45];
    char pds_member_name[9];

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
    vs->ftype = NF3DIR;
    vs->mode = 0555; /* read/execute permissions for everyone */
    vs->nlink = 2;   /* convention for directories: link from parent and self-link */
    vs->uid = 0;     /* root-owned */
    vs->gid = 0;     /* root-owned */
    vs->size = 4096; /* arbitrary non-zero size for the directory itself */
    vs->used = 4096; /* arbitrary non-zero disk usage for the directory itself */
    vs->rdev_maj = 0;
    vs->rdev_min = 0;
    vs->fsid = (uint64_t)export_idx + 1; /* unique filesystem ID based on export index */
    // Here we generate the fileid for the dataset, based on dataset name.
    // We'll use this as the basis for the raw_dev/raw_ino fields.
    vs->fileid  = mvs_fid_hash(path, NULL);
    vs->raw_ino = mvs_fid_ino32(path, NULL);
    vs->raw_dev = (uint32_t)export_idx + 1; 

    // Now the accessed/modified/created date/times.
    // For simplicity, we'll set these all to the same value based on the current time.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    vs->atime_sec = (uint32_t)tv.tv_sec;
    vs->atime_nsec = (uint32_t)(tv.tv_usec * 1000); // convert microseconds to nanoseconds
    vs->mtime_sec = vs->atime_sec;
    vs->mtime_nsec = vs->atime_nsec;
    vs->ctime_sec = vs->atime_sec;      
    vs->ctime_nsec = vs->atime_nsec;

    return 0;
}

int vfs_stat(const char *path, vfs_stat_t *vs)
{
    struct stat st;
    int export_idx;
    char ebcdic_path[MAX_PATH_LEN];

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Is this path a directory or a member of a PDS */
    int path_type = mvs_path_type(ebcdic_path, &export_idx);

    if (path_type == MVS_PATH_TYPE_PDS_MEMBER) {
        return vfs_stat_pds_member(ebcdic_path, export_idx, vs);
    } else if (path_type == MVS_PATH_TYPE_DATASET) {
        return vfs_stat_dataset(ebcdic_path, export_idx, vs);
    } else {
        errno = ENOENT;
        return -1;
    }

    return 0;
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
    int         fd;
    ssize_t     n;
    struct      stat st;
    int         saved_errno;
    int         rc;
    char        ebcdic_path[MAX_PATH_LEN];
    char        pds_dsname[45];
    char        pds_member_name[9];
    int         export_idx; 


    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Is this path to a directory or a member of a PDS */
    int path_type = mvs_path_type(ebcdic_path, &export_idx);

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
        case EPERM:         return NFS3ERR_PERM;
        case ENOENT:        return NFS3ERR_NOENT;
        case EIO:           return NFS3ERR_IO;
        case ENXIO:         return NFS3ERR_NXIO;
        case EACCES:        return NFS3ERR_ACCES;
        case EEXIST:        return NFS3ERR_EXIST;
        case EXDEV:         return NFS3ERR_XDEV;
        case ENODEV:        return NFS3ERR_NODEV;
        case ENOTDIR:       return NFS3ERR_NOTDIR;
        case EISDIR:        return NFS3ERR_ISDIR;
        case EINVAL:        return NFS3ERR_INVAL;
        case EFBIG:         return NFS3ERR_FBIG;
        case ENOSPC:        return NFS3ERR_NOSPC;
        case EROFS:         return NFS3ERR_ROFS;
        case EMLINK:        return NFS3ERR_MLINK;
        case ENAMETOOLONG:  return NFS3ERR_NAMETOOLONG;
        case ENOTEMPTY:     return NFS3ERR_NOTEMPTY;
        case EDQUOT:        return NFS3ERR_DQUOT;
        default:            return NFS3ERR_IO;
    }
}

/* -------------------------------------------------------------------- */
/* dir_find_free                                                        */
/* -------------------------------------------------------------------- */

void dir_openlist_init() {
    memset(g_dir_pool, 0, sizeof(g_dir_pool));
}

vfs_dir_t *dir_openlist_find_free() 
{
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


int dir_openlist_fill_members(vfs_dir_t *dir_openlist_entry) 
{

}


/* -------------------------------------------------------------------- */
/* vfs_opendir: open a directory for iteration.                         */
/* Returns a handle from the static pool, or NULL on error.             */
/* -------------------------------------------------------------------- */
vfs_dir_t *vfs_opendir(const char *path)
{
    vfs_dir_t dir_openlist_entry;
    char        ebcdic_path[MAX_PATH_LEN];
    char        pds_dsname[45];
    char        pds_member_name[9];
    int         export_idx; 
    int         end_of_dir;
    int         num_of_members_returned;

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)path, MAX_PATH_LEN - 1);
    ebcdic_path[MAX_PATH_LEN - 1] = '\0';

    /* Is this path to a directory or a member of a PDS */
    int path_type = mvs_path_type(ebcdic_path, &export_idx);

    if ( path_type != MVS_PATH_TYPE_DATASET ){
        errno = ENOENT;
        return -1;
    }

    /* Get free open directory entry */
    dir_openlist_entry = dir_openlist_find_free();
    if ( !dir_openlist_entry )
        return NULL;

    **** MOVE THIS UP INTO dir_openlist_fill_members etc.
    **** MOVE THIS UP INTO dir_openlist_fill_members etc.
    **** MOVE THIS UP INTO dir_openlist_fill_members etc.

    rc = mvs_pds_member_list(
        pds_dsname, export_idx, /* start-member */ NULL,
        /* max-members */ MVSVFS_PDS_DIR_CACHE_SIZE,
        /* member-entries */ dir_openlist_entry->members,
        &num_of_members_returned,
        &end_of_dir);

    strncpy(dir_openlist_entry->pds_dsname_ebcdic, pds_dsname, 
        sizeof(dir_openlist_entry->pds_dsname_ebcdic));
    dir_openlist_entry->status = MVSVFS_DIR_OPENLIST_USED;
    dir_openlist_entry->next_cookie = 1;
    dir_openlist_entry->pds_entries_cached = num_of_members_read;
    dir_openlist_entry->end_of_dir_read = end_of_dir;

    return &g_dir_pool[i];
}
 
/* -------------------------------------------------------------------- */
/* vfs_readdir_next: read the next directory entry.                     */
/*                                                                      */
/* Fills name (NUL-terminated), *fileid (inode), and *cookie (1-based   */
/* position of this entry, for use as the NFS READDIR cookie).          */
/* Returns 0 on success, -1 at end of directory or error.               */
/* -------------------------------------------------------------------- */
int vfs_readdir_next(vfs_dir_t *dir_openlist_entry, 
                     char *name, uint32_t maxname, 
                     uint64_t *fileid,
                     uint64_t *cookie)
{
    int mem_num;
    char *prev_last_read;

    mem_num = dir_openlist_entry->next_cookie - 1;

    /* Have we got to end of cached directory entries? */
    if (mem_num >= dir_openlist_entry->pds_entries_cached) {
        /* get more if we didn't get to end of directory */
        if (dir_openlist_entry->end_of_dir_read) {
            prev_last_read = dir_openlist_entry->
                members[dir_openlist_entry->pds_entries_cached - 1].name;
            

        }
    }
    strncpy(name, dir_openlist_entry->members[mem_num].name, maxname - 1);
    name[maxname - 1] = '\0';
    ebcdic_to_ascii(name, name, maxname);

    de = readdir(d->dp);
    if (!de) return -1;
 
 
    *fileid = mvs_fid_hash(
        dir_openlist_entry->pds_dsname, 
        dir_openlist_entry->members[memnum].name ); /* generate fileid based on dataset and member name */

    /* Update cookie for next entry to be retrieved */
    *cookie = dir_openlist_entry->next_cookie;
    dir_openlist_entry->next_cookie++;

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
 
    rewinddir(d->dp);
    d->next_cookie = 1;
 
    while (skip > 0) {
        if (!readdir(d->dp)) break;
        d->next_cookie++;
        skip--;
    }
}
 
/* -------------------------------------------------------------------- */
/* vfs_closedir: release a directory handle back to the pool.           */
/* -------------------------------------------------------------------- */
void vfs_closedir(vfs_dir_t *d)
{
    d->status = MVSVFS_DIR_OPENLIST_FREE;
}
