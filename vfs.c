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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include "nfsd.h"

/* ------------------------------------------------------------------ */
/* Directory handle type (opaque outside this file)                     */
/* ------------------------------------------------------------------ */
struct vfs_dir {
    DIR     *dp;
    uint64_t next_cookie; /* 1-based index of the next entry to return */
};

/* Static pool of directory handles -- no malloc required */
static vfs_dir_t g_dir_pool[MAX_OPEN_DIRS];
static int       g_dir_used[MAX_OPEN_DIRS]; /* 1 if slot is in use */

/* ------------------------------------------------------------------ */
/* vfs_stat: fill a vfs_stat_t from lstat() of path.                   */
/* Returns 0 on success, -1 on error (errno set).                      */
/* ------------------------------------------------------------------ */
int vfs_stat(const char *path, vfs_stat_t *vs)
{
    struct stat st;

    if (lstat(path, &st) < 0) return -1;

    /* Map S_IF* bits to NFS3 file type */
    if      (S_ISREG(st.st_mode))  vs->ftype = NF3REG;
    else if (S_ISDIR(st.st_mode))  vs->ftype = NF3DIR;
    else if (S_ISBLK(st.st_mode))  vs->ftype = NF3BLK;
    else if (S_ISCHR(st.st_mode))  vs->ftype = NF3CHR;
    else if (S_ISLNK(st.st_mode))  vs->ftype = NF3LNK;
    else if (S_ISSOCK(st.st_mode)) vs->ftype = NF3SOCK;
    else if (S_ISFIFO(st.st_mode)) vs->ftype = NF3FIFO;
    else                            vs->ftype = NF3REG;

    vs->mode       = (uint32_t)(st.st_mode & 0xFFFu);
    vs->nlink      = (uint32_t) st.st_nlink;
    vs->uid        = (uint32_t) st.st_uid;
    vs->gid        = (uint32_t) st.st_gid;
    vs->size       = (uint64_t) st.st_size;
    vs->used       = (uint64_t) st.st_blocks * 512u;
    vs->rdev_maj   = 0;
    vs->rdev_min   = 0;
    vs->fsid       = (uint64_t) st.st_dev;
    vs->fileid     = (uint64_t) st.st_ino;

    /*
     * Nanosecond timestamps: st_mtim is POSIX 2008; present on Linux
     * with _POSIX_C_SOURCE >= 200809L.  On older systems or MVS,
     * the nsec fields are simply left as 0.
     */
    vs->atime_sec  = (uint32_t) st.st_atime;
    vs->mtime_sec  = (uint32_t) st.st_mtime;
    vs->ctime_sec  = (uint32_t) st.st_ctime;
#if defined(__linux__)
    vs->atime_nsec = (uint32_t) st.st_atim.tv_nsec;
    vs->mtime_nsec = (uint32_t) st.st_mtim.tv_nsec;
    vs->ctime_nsec = (uint32_t) st.st_ctim.tv_nsec;
#else
    vs->atime_nsec = 0;
    vs->mtime_nsec = 0;
    vs->ctime_nsec = 0;
#endif

    vs->raw_dev = (uint32_t) st.st_dev;
    vs->raw_ino = (uint32_t) st.st_ino;

    return 0;
}

/* ------------------------------------------------------------------ */
/* vfs_pread: positional read from path.                                */
/* Opens, reads count bytes at offset, closes.                          */
/* Sets *nread to bytes actually read; *eof to 1 if at end of file.    */
/* Returns 0 on success, -1 on error.                                   */
/* ------------------------------------------------------------------ */
int vfs_pread(const char *path, void *buf, uint32_t count,
              uint64_t offset, uint32_t *nread, int *eof)
{
    int     fd;
    ssize_t n;
    struct  stat st;

    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    n = pread(fd, buf, (size_t)count, (off_t)offset);
    if (n < 0) { close(fd); return -1; }

    *nread = (uint32_t)n;
    *eof   = 0;

    if (fstat(fd, &st) == 0) {
        *eof = ((off_t)(offset + (uint64_t)n) >= st.st_size) ? 1 : 0;
    } else {
        *eof = (n < (ssize_t)count) ? 1 : 0;
    }

    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* vfs_pwrite: positional write to path.                                */
/* Opens for writing, writes count bytes at offset, fsyncs, closes.    */
/* Returns 0 on success, -1 on error.                                   */
/* ------------------------------------------------------------------ */
int vfs_pwrite(const char *path, const void *buf,
               uint32_t count, uint64_t offset)
{
    int     fd;
    ssize_t n;

    fd = open(path, O_WRONLY);
    if (fd < 0) return -1;

    n = pwrite(fd, buf, (size_t)count, (off_t)offset);
    if (n >= 0) fsync(fd);
    close(fd);

    return (n == (ssize_t)count) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* vfs_create: create a new empty file (or truncate if it exists).      */
/* mode is the permission bits (e.g. 0644).                             */
/* Returns 0 on success, -1 on error.                                   */
/* ------------------------------------------------------------------ */
int vfs_create(const char *path, uint32_t mode)
{
    int fd = open(path,
                  O_CREAT | O_WRONLY | O_TRUNC,
                  (mode_t)mode);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* vfs_remove: delete a regular file.                                   */
/* ------------------------------------------------------------------ */
int vfs_remove(const char *path)
{
    return unlink(path);
}

/* ------------------------------------------------------------------ */
/* vfs_rename: rename / move a file.                                    */
/* ------------------------------------------------------------------ */
int vfs_rename(const char *from, const char *to)
{
    return rename(from, to);
}

/* ------------------------------------------------------------------ */
/* vfs_truncate: change file size to 'size' bytes.                      */
/* ------------------------------------------------------------------ */
int vfs_truncate(const char *path, uint64_t size)
{
    return truncate(path, (off_t)size);
}

/* ------------------------------------------------------------------ */
/* vfs_set_times: set atime and/or mtime on path.                       */
/* set_atime / set_mtime use the SET_* constants from nfsd.h:           */
/*   SET_DONT_CHANGE      - leave unchanged                             */
/*   SET_TO_SERVER_TIME   - use current time                            */
/*   SET_TO_CLIENT_TIME   - use the supplied *_sec value                */
/* ------------------------------------------------------------------ */
int vfs_set_times(const char *path,
                  int set_atime, uint32_t atime_sec,
                  int set_mtime, uint32_t mtime_sec)
{
    struct timeval tv[2];
    struct stat    st;

    if (set_atime == SET_DONT_CHANGE || set_mtime == SET_DONT_CHANGE) {
        /* Read current times so we don't change the one we should keep */
        if (stat(path, &st) < 0) return -1;
    }

    if (set_atime == SET_DONT_CHANGE) {
        tv[0].tv_sec  = (long)st.st_atime;
        tv[0].tv_usec = 0;
    } else if (set_atime == SET_TO_CLIENT_TIME) {
        tv[0].tv_sec  = (long)atime_sec;
        tv[0].tv_usec = 0;
    } else {
        /* SET_TO_SERVER_TIME: pass NULL to utimes for current time */
        tv[0].tv_sec  = 0;
        tv[0].tv_usec = 0;
    }

    if (set_mtime == SET_DONT_CHANGE) {
        tv[1].tv_sec  = (long)st.st_mtime;
        tv[1].tv_usec = 0;
    } else if (set_mtime == SET_TO_CLIENT_TIME) {
        tv[1].tv_sec  = (long)mtime_sec;
        tv[1].tv_usec = 0;
    } else {
        tv[1].tv_sec  = 0;
        tv[1].tv_usec = 0;
    }

    /* If either time is SET_TO_SERVER_TIME, use NULL for current time */
    if (set_atime == SET_TO_SERVER_TIME ||
        set_mtime == SET_TO_SERVER_TIME) {
        return utimes(path, NULL);
    }

    return utimes(path, tv);
}

/* ------------------------------------------------------------------ */
/* vfs_fsstat: fill vfs_fsstat_t from statvfs().                        */
/* ------------------------------------------------------------------ */
int vfs_fsstat(const char *path, vfs_fsstat_t *fs)
{
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
}

/* ------------------------------------------------------------------ */
/* vfs_errno_to_nfs3: map a POSIX errno to an NFSv3 error code.         */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* vfs_opendir: open a directory for iteration.                         */
/* Returns a handle from the static pool, or NULL on error.             */
/* ------------------------------------------------------------------ */
vfs_dir_t *vfs_opendir(const char *path)
{
    DIR *dp;
    int  i;

    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        if (!g_dir_used[i]) break;
    }
    if (i == MAX_OPEN_DIRS) {
        errno = EMFILE;
        return NULL;
    }

    dp = opendir(path);
    if (!dp) return NULL;

    g_dir_pool[i].dp          = dp;
    g_dir_pool[i].next_cookie = 1;
    g_dir_used[i]             = 1;
    return &g_dir_pool[i];
}

/* ------------------------------------------------------------------ */
/* vfs_readdir_next: read the next directory entry.                     */
/*                                                                      */
/* Fills name (NUL-terminated), *fileid (inode), and *cookie (1-based  */
/* position of this entry, for use as the NFS READDIR cookie).          */
/* Returns 0 on success, -1 at end of directory or error.              */
/* ------------------------------------------------------------------ */
int vfs_readdir_next(vfs_dir_t *d, char *name,
                     uint32_t maxname, uint64_t *fileid,
                     uint64_t *cookie)
{
    struct dirent *de;

    de = readdir(d->dp);
    if (!de) return -1;

    strncpy(name, de->d_name, maxname - 1);
    name[maxname - 1] = '\0';

    *fileid = (uint64_t)de->d_ino;
    *cookie = d->next_cookie;
    d->next_cookie++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* vfs_seekdir_to: seek so the next vfs_readdir_next call returns the   */
/* entry AFTER the one that had the given cookie value.                 */
/* cookie=0 means start from the very beginning.                        */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* vfs_closedir: release a directory handle back to the pool.           */
/* ------------------------------------------------------------------ */
void vfs_closedir(vfs_dir_t *d)
{
    int idx;
    if (!d) return;
    closedir(d->dp);
    idx = (int)(d - g_dir_pool);
    if (idx >= 0 && idx < MAX_OPEN_DIRS) g_dir_used[idx] = 0;
}
