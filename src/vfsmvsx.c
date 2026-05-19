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
 *
 * A DUMMIED UP VFS FUNCTION SET FOR ...
 * A DUMMIED UP VFS FUNCTION SET FOR ...
 * A DUMMIED UP VFS FUNCTION SET FOR ...
 *
 * /export/src
 * /export/src/file1.c
 * /export/src/file2.h
 *
 */
 
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include "nfsd.h"
 
/* -------------------------------------------------------------------- */
/* Directory handle type (opaque outside this file)                     */
/* -------------------------------------------------------------------- */
struct vfs_dir {
    unsigned char **dir_content;
    uint64_t next_cookie; /* 1-based index of the next entry to return */
};
 
/* Static pool of directory handles -- no malloc required */
static vfs_dir_t g_dir_pool[MAX_OPEN_DIRS];
static int       g_dir_used[MAX_OPEN_DIRS]; /* 1 if slot is in use */
 
unsigned char * file1_c = "/* Content of file1.c */\n";
unsigned char * file2_h = "Content of file2.h\n";
 
/* -------------------------------------------------------------------- */
/* vfs_stat: fill a vfs_stat_t from lstat() of path.                    */
/* Returns 0 on success, -1 on error (errno set).                       */
/* -------------------------------------------------------------------- */
int vfs_stat(const char *path, vfs_stat_t *vs)
{
    struct stat st;
    time_t    now;
    time_t    ftime;
 
    now = time(NULL);
 
    if ( strcmp(path, "/export") == 0 ) {
        vs->ftype = NF3DIR;
        vs->fileid = 1;
        vs->size   = (uint32_t) 512;
        ftime = now - 7200;
    } else if ( strcmp(path, "/export/src") == 0 ) {
        vs->ftype = NF3DIR;
        vs->fileid = 2;
        vs->size   = (uint32_t) 512;
        ftime = now - 7200;
    } else if ( strcmp(path, "/export/src/file1.c") == 0 ) {
        vs->ftype = NF3REG;
        vs->fileid = 3;
        vs->size   = (uint32_t) strlen(file1_c);
        ftime = now;
    } else if ( strcmp(path, "/export/src/file2.h") == 0 ) {
        vs->ftype = NF3REG;
        vs->fileid = 4;
        vs->size   = (uint32_t) strlen(file2_h);
        ftime = now;
    } else {
        errno = ENOENT;
        return -1;
    }
 
    vs->used   = (uint32_t) 512;
 
    vs->mode       = (uint32_t) 0;
    vs->nlnk       = (uint32_t) 0;
    vs->uid        = (uint32_t) 999;
    vs->gid        = (uint32_t) 999;
    vs->rdev_maj   = (uint32_t) 0;
    vs->rdev_min   = (uint32_t) 0;
    vs->fsid       = (uint32_t) vs->fileid;
 
    vs->atime_sec  = (uint32_t) ftime;
    vs->mtime_sec  = (uint32_t) ftime;
    vs->ctime_sec  = (uint32_t) ftime;
    /* Set nano-seconds portion of time to 0 */
    vs->atime_nsec = 0;
    vs->mtime_nsec = 0;
    vs->ctime_nsec = 0;
 
    vs->raw_dev = (uint32_t) vs->fileid;
    vs->raw_ino = (uint32_t) vs->fileid;
 
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
    int        fd;
    ssize_t    n;
    struct     stat st;
    int        saved_errno;
 
    unsigned char * file_data;
 
    if ( strcmp(path, "/export/src/file1.c") == 0 ) {
        file_data =  file1_c;
    } else if ( strcmp(path, "/export/src/file2.h") == 0 ) {
        file_data = file2_h;
    ) else if ( strcmp(path, "/export/src") == 0 ) {
        errno = EISDIR;
        return -1;
    ) else if ( strcmp(path, "/export") == 0 ) {
        errno = EISDIR;
        return -1;
    ) else {
        errno = ENOENT;
        return -1;
    }
 
    if ( offset > strlen(file_data) ) {
        *eof = 1;
        *nread = 0;
        return 0;
    }
    strncpy(buf, file_data, count);
    *nread = strlen(file_data);
    *eof = 0;
 
    return 0;
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
}
 
/* -------------------------------------------------------------------- */
/* vfs_remove: delete a regular file.                                   */
/* -------------------------------------------------------------------- */
int vfs_remove(const char *path)
{
    errno = EACCES;
    return -1;
}
 
/* -------------------------------------------------------------------- */
/* vfs_rename: rename / move a file.                                    */
/* -------------------------------------------------------------------- */
int vfs_rename(const char *from, const char *to)
{
    errno = EACCES;
    return -1;
}
 
/* -------------------------------------------------------------------- */
/* vfs_truncate: change file size to 'size' bytes.                      */
/* -------------------------------------------------------------------- */
int vfs_truncate(const char *path, uint64_t size)
{
    errno = EACCES;
    return -1;
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
}
 
/* -------------------------------------------------------------------- */
/* vfs_fsstat: fill vfs_fsstat_t from statvfs().                        */
/* -------------------------------------------------------------------- */
int vfs_fsstat(const char *path, vfs_fsstat_t *fs)
{
    fs->total_bytes = 512 * 200;
    fs->free_bytes  = 512 * 196;
    fs->avail_bytes = 512 * 196;
    fs->total_files = 2;
    fs->free_files  = 1024*8 - 4;
    fs->avail_files = 1024*8 - 4;
    fs->invarsec    = 0;
    return 0;
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
/* Dummy directory contents                                             */
/* -------------------------------------------------------------------- */
static unsigned char * dir_export[] = {
    ".",
    "..",
    "src",
    0
}
 
static unsigned char * dir_src[] = {
    ".",
    "..",
    "file1.c",
    "file2.h",
    0
}
 
/* -------------------------------------------------------------------- */
/* vfs_opendir: open a directory for iteration.                         */
/* Returns a handle from the static pool, or NULL on error.             */
/* -------------------------------------------------------------------- */
vfs_dir_t *vfs_opendir(const char *path)
{
    int  dir_index;
 
    for (dir_index = 0; dir_index < MAX_OPEN_DIRS; dir_index++) {
        if (!g_dir_used[dir_index]) break;
    }
    if (dir_index == MAX_OPEN_DIRS) {
        errno = EMFILE;
        return NULL;
    }
 
    if ( strcmp(path, "/export") == 0 ) {
        g_dir_pool[i].dir_content = dir_export;
        g_dir_pool[i].next_cookie = 1;
        g_dir_used[i]             = 1;
    } else if ( strcmp(path, "/export/src") == 0 ) {
        g_dir_pool[i].dir_content = dir_src;
        g_dir_pool[i].next_cookie = 1;
        g_dir_used[i]             = 1;
    } else {
        errno = ENOENT;
        return -1;
    }
 
    return &g_dir_pool[i];
}
 
/* -------------------------------------------------------------------- */
/* vfs_readdir_next: read the next directory entry.                     */
/*                                                                      */
/* Fills name (NUL-terminated), *fileid (inode), and *cookie (1-based   */
/* position of this entry, for use as the NFS READDIR cookie).          */
/* Returns 0 on success, -1 at end of directory or error.               */
/* -------------------------------------------------------------------- */
int vfs_readdir_next(vfs_dir_t *d, char *name,
                     uint32_t maxname, uint64_t *fileid,
                     uint64_t *cookie)
{
    unsigned char * dir_entry_name;
    dir_entry_name = d->dir_content[d->next_cookie-1];
 
    if ( dir_entry_name ) {
        strncpy(name, dir_entry_name, maxname - 1);
        name[maxname - 1] = '\0';
    } else {
        errno = 0;
        return -1;
    }
 
    *fileid = d->next_cookie;
    *cookie = d->next_cookie;
    d->next_cookie++;
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
    int idx;
    if (!d) return;
    closedir(d->dp);
    idx = (int)(d - g_dir_pool);
    if (idx >= 0 && idx < MAX_OPEN_DIRS) g_dir_used[idx] = 0;
}
