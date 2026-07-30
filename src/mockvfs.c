/*
 * mockvfs.c - Mock VFS implementation for MVS build and integration
 *             testing.
 *
 * Drop-in replacement for vfs.c.  Implements the complete vfs_* interface
 * declared in nfsd.h without making any OS filesystem calls.
 *
 * Each configured NFS export presents the same fixed set of five files:
 *
 *   file1.txt   1 line of text
 *   file2.txt   2 lines of text
 *   file3.txt   3 lines of text
 *   file4.txt   4 lines of text
 *   file5.txt   5 lines of text
 *
 * Behaviour summary:
 *   vfs_stat       returns synthesized metadata for known paths
 *   vfs_pread      returns the mock file content respecting offset/count
 *   vfs_pwrite     logs the call and returns success (write ignored)
 *   vfs_create     logs the call and returns success (ignored)
 *   vfs_remove     logs the call and returns success (ignored)
 *   vfs_rename     logs the call and returns -1/EPERM
 *   vfs_truncate   logs the call and returns success (ignored)
 *   vfs_set_times  logs the call and returns success (ignored)
 *   vfs_fsstat     returns fixed read-only filesystem statistics
 *   vfs_opendir    returns a handle for any known export directory
 *   vfs_readdir_next iterates the fixed directory contents
 *   vfs_seekdir_to repositions the directory iterator by cookie
 *   vfs_closedir   releases the directory handle slot
 *
 * Every call is logged to stdout with the prefix [MOCKVFS].
 *
 * Path format (from fh_resolve in fhandle.c):
 *   export root:  <export.host_path>
 *   file:         <export.host_path>/<filename>
 *
 * EBCDIC / ASCII encoding (MVS only):
 *   All C string literals in this file are compiled as EBCDIC by JCC.
 *   Paths received by vfs_* functions have a mixed encoding: the
 *   host_path prefix is EBCDIC (loaded from the EBCDIC config file),
 *   but the filename component is ASCII because fh_resolve() appends
 *   relpaths that were stored from what vfs_readdir_next() returned,
 *   and that function translates its output to ASCII before returning.
 *   mock_name_eq() handles the mismatch inside mock_resolve() by
 *   converting the ASCII filename to EBCDIC before comparing it with
 *   a compiled EBCDIC string literal.
 *
 * JCC C89 compliance: all variable declarations precede executable
 * statements in every function.  Only block comments are used.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "nfsd.h"
#include "ebcdic.h"
#include "hexdump.h"

/* EPERM and the other errno values JCC omits come from the errno
   compatibility block in nfsd.h. */

/* -------------------------------------------------------------------- */
/* Layout constants                                                     */
/* -------------------------------------------------------------------- */

#define MOCK_NUM_FILES    5   /* files exposed per export directory     */
#define MOCK_NUM_ENTRIES  7   /* readdir slots: "." + ".." + 5 files   */

/* Return codes from mock_resolve() */
#define MOCK_NOT_FOUND  (-1)
#define MOCK_IS_DIR       0
#define MOCK_IS_FILE      1

/* -------------------------------------------------------------------- */
/* File table                                                           */
/*                                                                      */
/* File N is named fileN.txt and contains exactly N lines.             */
/* Adjacent string literal tokens are concatenated by the preprocessor  */
/* (standard in C89), so each entry is one contiguous string constant.  */
/* -------------------------------------------------------------------- */

static const char *s_file_name[MOCK_NUM_FILES] = {
    "file1.txt",
    "file2.txt",
    "file3.txt",
    "file4.txt",
    "file5.txt"
};

static const char *s_file_content[MOCK_NUM_FILES] = {

    /* file1.txt -- 1 line */
    "file1.txt line 1\n",

    /* file2.txt -- 2 lines */
    "file2.txt line 1\n"
    "file2.txt line 2\n",

    /* file3.txt -- 3 lines */
    "file3.txt line 1\n"
    "file3.txt line 2\n"
    "file3.txt line 3\n",

    /* file4.txt -- 4 lines */
    "file4.txt line 1\n"
    "file4.txt line 2\n"
    "file4.txt line 3\n"
    "file4.txt line 4\n",

    /* file5.txt -- 5 lines */
    "file5.txt line 1\n"
    "file5.txt line 2\n"
    "file5.txt line 3\n"
    "file5.txt line 4\n"
    "file5.txt line 5\n"
};

/* -------------------------------------------------------------------- */
/* Directory entry table                                                */
/*                                                                      */
/* Cookie values are 1-based.  The array index equals cookie - 1.      */
/* -------------------------------------------------------------------- */

static const char *s_dir_entries[MOCK_NUM_ENTRIES] = {
    ".",          /* cookie 1 */
    "..",         /* cookie 2 */
    "file1.txt",  /* cookie 3 */
    "file2.txt",  /* cookie 4 */
    "file3.txt",  /* cookie 5 */
    "file4.txt",  /* cookie 6 */
    "file5.txt"   /* cookie 7 */
};

/* -------------------------------------------------------------------- */
/* EBCDIC filename comparison helper                                    */
/*                                                                      */
/* On MVS, string literals such as "file1.txt" and "." are EBCDIC.     */
/* The filename component of a path passed to mock_resolve() is ASCII   */
/* (it was stored in the fhandle cache from an ASCII vfs_readdir_next   */
/* result).  mock_name_eq() translates the ASCII filename to EBCDIC in  */
/* a local buffer and then does a plain strcmp against the literal.     */
/*                                                                      */
/* On Linux both sides of every filename comparison are ASCII, so the   */
/* macro falls back to a direct strcmp.                                 */
/* -------------------------------------------------------------------- */

/* These define the forward slash characters in ASCII and EBCDIC */

#define PATH_SEPARATOR_ASCII  (char)0x2f  
#define PATH_SEPARATOR_EBCDIC (char)0x61 

#ifdef __MVS__

static int mock_name_eq(const char *ascii_name, const char *ebcdic_lit)
{
    char   ebuf[MAX_NAME];
    size_t len;

    len = strlen(ascii_name);
    if (len >= MAX_NAME) return 0;
    ascii_to_ebcdic((uint8_t *)ebuf,
                    (const uint8_t *)ascii_name,
                    len + 1u);   /* +1 converts the NUL terminator too */
    return strcmp(ebuf, ebcdic_lit) == 0;
}

#define MOCK_NAME_EQ(aname, elit)  mock_name_eq((aname), (elit))

/* -------------------------------------------------------------------- */
/* mock_path_for_log: return a fully-EBCDIC path for printf logging.    */
/*                                                                      */
/* Paths have the form:                                                 */
/*   <ASCII host_path>                           (directory root)       */
/*   <ASCII host_path><ASCII '/'><ASCII relpath> (file)                 */
/*                                                                      */
/* A single static buffer is used; safe for single-threaded use.        */
/* Do NOT call MOCK_PATH() twice in the same printf argument list --    */
/* use consecutive printfs instead (see vfs_rename).                    */
/* -------------------------------------------------------------------- */

#define MOCK_LOG_BUF_SIZE  (MAX_PATH * 2)

static char s_log_buf[MOCK_LOG_BUF_SIZE];

static const char *mock_path_for_log(const char *path)
{
    const char *slash;
    int         prefix_len;
    int         rest_len;
    int         total_len;

    ascii_to_ebcdic((uint8_t *)s_log_buf, (const uint8_t *)path, MOCK_LOG_BUF_SIZE - 1);
    s_log_buf[MOCK_LOG_BUF_SIZE - 1] = '\0';

#if 0
    total_len = (int)strlen(path);
    if (total_len >= MOCK_LOG_BUF_SIZE)
        total_len = MOCK_LOG_BUF_SIZE - 1;

    /* Find the EBCDIC '/' that separates host_path from the relpath.   */
    /* On MVS the literal '/' in C source is compiled as EBCDIC 0x61,   */
    /* which is exactly the separator fh_resolve() inserts.             */
    slash = strchr(path, '/');

    if (slash == NULL) {
        /* No separator -- entire path is the EBCDIC host_path only */
        strncpy(s_log_buf, path, (size_t)total_len);
        s_log_buf[total_len] = '\0';
    } else {
        /* Copy the EBCDIC prefix (host_path + '/') without change */
        prefix_len = (int)(slash - path) + 1;
        strncpy(s_log_buf, path, (size_t)prefix_len);

        /* Translate the ASCII relpath suffix to EBCDIC.             */
        /* +1 converts the NUL terminator as well.                   */
        rest_len = total_len - prefix_len;
        ascii_to_ebcdic((uint8_t *)s_log_buf + prefix_len,
                        (const uint8_t *)path  + prefix_len,
                        (size_t)rest_len + 1u);
    }
#endif

    return s_log_buf;
}

#define MOCK_PATH(p)  mock_path_for_log(p)

#else /* !__MVS__ */

#define MOCK_NAME_EQ(aname, elit)  (strcmp((aname), (elit)) == 0)
#define MOCK_PATH(p)               (p)

#endif /* __MVS__ */

/* -------------------------------------------------------------------- */
/* Directory handle (struct vfs_dir is opaque outside this file)        */
/*                                                                      */
/* File ID scheme (matches vfs_stat):                                   */
/*   directory:  (exp_idx + 1) * 10                                     */
/*   fileN.txt:  (exp_idx + 1) * 10 + N   (N = 1 .. MOCK_NUM_FILES)    */
/* -------------------------------------------------------------------- */

struct vfs_dir {
    int      in_use;
    uint64_t next_cookie;  /* 1-based; s_dir_entries index = next_cookie-1 */
    int      exp_idx;      /* export index, for fileid computation          */
};

static vfs_dir_t g_dir_pool[MAX_OPEN_DIRS];
static int       g_dir_used[MAX_OPEN_DIRS]; /* 1 if slot is in use */

void dir_openlist_init() {
    memset(g_dir_used, 0, sizeof(g_dir_used));
}

/* -------------------------------------------------------------------- */
/* mock_resolve: classify a path as directory, file, or not found.      */
/*                                                                      */
/* Paths are built by fh_resolve() as:                                  */
/*   <export.host_path>            -- export root (directory)            */
/*   <export.host_path>/<filename> -- member file                        */
/*                                                                      */
/* A filename of "." is also accepted and resolves to the parent        */
/* directory so that NFS LOOKUP(".") works correctly.                   */
/*                                                                      */
/* Returns MOCK_IS_DIR, MOCK_IS_FILE, or MOCK_NOT_FOUND.               */
/* On success, *exp_idx and *file_idx are set.                          */
/* *file_idx is -1 for directories and 0-4 for files.                  */
/* -------------------------------------------------------------------- */

static int mock_resolve(const char *path, int *exp_idx, int *file_idx)
{
    int         i;
    int         j;
    int         n;
    export_t   *exp;
    const char *last_slash;
    const char *filename;
    size_t      host_len;
    size_t      exp_len;

    n = exports_count();

    /* Check for an exact match against an export host_path (directory) */
    for (i = 0; i < n; i++) {
        exp = exports_get(i);
        if (strcmp(path, exp->host_path) == 0) {
            *exp_idx  = i;
            *file_idx = -1;
            return MOCK_IS_DIR;
        }
    }

    /* Split the last path component off the rest */
    last_slash = strrchr(path, PATH_SEPARATOR_ASCII);
    if (last_slash == NULL) {
        return MOCK_NOT_FOUND;
    }
    filename = last_slash + 1;
    if (*filename == '\0') {
        return MOCK_NOT_FOUND;
    }
    host_len = (size_t)(last_slash - path);

    /* "." -- the directory that contains this path component */
    if (MOCK_NAME_EQ(filename, ".")) {
        for (j = 0; j < n; j++) {
            exp     = exports_get(j);
            exp_len = strlen(exp->host_path);
            if (exp_len == host_len &&
                strncmp(path, exp->host_path, host_len) == 0) {
                *exp_idx  = j;
                *file_idx = -1;
                return MOCK_IS_DIR;
            }
        }
        return MOCK_NOT_FOUND;
    }

    /* Check against the mock file name table */
    for (i = 0; i < MOCK_NUM_FILES; i++) {
        if (MOCK_NAME_EQ(filename, s_file_name[i])) {
            /* Find the export that owns this parent directory path */
            for (j = 0; j < n; j++) {
                exp     = exports_get(j);
                exp_len = strlen(exp->host_path);
                if (exp_len == host_len &&
                    strncmp(path, exp->host_path, host_len) == 0) {
                    *exp_idx  = j;
                    *file_idx = i;
                    return MOCK_IS_FILE;
                }
            }
            /* Filename matched but the parent is not a known export.
             * Fall back to export 0 so the caller gets usable stats. */
            *exp_idx  = 0;
            *file_idx = i;
            return MOCK_IS_FILE;
        }
    }

    return MOCK_NOT_FOUND;
}

/* -------------------------------------------------------------------- */
/* vfs_stat: synthesize metadata for a known path.                      */
/* -------------------------------------------------------------------- */

int vfs_stat(const char *path, vfs_stat_t *vs)
{
    int      exp_idx;
    int      file_idx;
    int      kind;
    uint32_t base_id;
    time_t   now;

    hexdump(stdout, "[MOCKVFS] vfs_stat path", path, strlen(path));

    printf("[MOCKVFS] vfs_stat path=\"%s\"\n", MOCK_PATH(path));

    kind = mock_resolve(path, &exp_idx, &file_idx);
    if (kind == MOCK_NOT_FOUND) {
        errno = ENOENT;
        return -1;
    }

    now = time(NULL);
    memset(vs, 0, sizeof(*vs));

    /* Fields common to both directories and files */
    vs->uid        = 0u;
    vs->gid        = 0u;
    vs->rdev_maj   = 0u;
    vs->rdev_min   = 0u;
    vs->atime_sec  = (uint32_t)now;
    vs->mtime_sec  = (uint32_t)now;
    vs->ctime_sec  = (uint32_t)now;
    vs->atime_nsec = 0u;
    vs->mtime_nsec = 0u;
    vs->ctime_nsec = 0u;
    vs->fsid       = (uint64_t)(exp_idx + 1);

    /* File ID base for this export: (exp_idx+1)*10                     */
    /* Directory ID = base; file N ID = base + N  (N is 1-based)        */
    base_id = (uint32_t)(exp_idx + 1) * 10u;

    if (kind == MOCK_IS_DIR) {
        vs->ftype   = NF3DIR;
        vs->mode    = 0555u;
        vs->nlink   = 2u;
        vs->size    = 512u;
        vs->used    = 512u;
        vs->fileid  = (uint64_t)base_id;
        vs->raw_dev = (uint32_t)(exp_idx + 1);
        vs->raw_ino = 0u;
    } else {
        vs->ftype   = NF3REG;
        vs->mode    = 0444u;
        vs->nlink   = 1u;
        vs->size    = (uint64_t)strlen(s_file_content[file_idx]);
        vs->used    = vs->size;
        vs->fileid  = (uint64_t)(base_id + (uint32_t)(file_idx + 1));
        vs->raw_dev = (uint32_t)(exp_idx + 1);
        vs->raw_ino = (uint32_t)(file_idx + 1);
    }

    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_pread: return mock file content respecting offset and count.     */
/* -------------------------------------------------------------------- */

int vfs_pread(const char *path, void *buf, uint32_t count,
              uint64_t offset, uint32_t *nread, int *eof) 
{
    int         exp_idx;
    int         file_idx;
    int         kind;
    const char *content;
    uint32_t    clen;
    uint32_t    off32;
    uint32_t    avail;

    printf("[MOCKVFS] vfs_pread path=\"%s\" offset=%u count=%u\n",
           MOCK_PATH(path), (unsigned)offset, (unsigned)count);

    kind = mock_resolve(path, &exp_idx, &file_idx);
    if (kind == MOCK_NOT_FOUND) {
        errno = ENOENT;
        return -1;
    }
    if (kind == MOCK_IS_DIR) {
        errno = EISDIR;
        return -1;
    }

    content = s_file_content[file_idx];
    clen    = (uint32_t)strlen(content);

    if (offset >= (uint64_t)clen) {
        *nread = 0u;
        *eof   = 1;
        return 0;
    }

    off32 = (uint32_t)offset;
    avail = clen - off32;
    if (avail > count) avail = count;

    memcpy(buf, content + off32, (size_t)avail);
#ifdef __MVS__
    /* s_file_content[] is EBCDIC; translate to ASCII for the NFS client */
    ebcdic_to_ascii((uint8_t *)buf, (const uint8_t *)buf, (size_t)avail);
#endif
    *nread = avail;
    *eof   = ((off32 + avail) >= clen) ? 1 : 0;
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_pwrite: log and accept (write is silently discarded).            */
/* -------------------------------------------------------------------- */

int vfs_pwrite(const char *path, const void *buf,
               uint32_t count, uint64_t offset)
{
    printf("[MOCKVFS] vfs_pwrite path=\"%s\" offset=%u count=%u"
           " (discarded)\n",
           MOCK_PATH(path), (unsigned)offset, (unsigned)count);
    (void)buf;
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_commit: log and accept (nothing is buffered in the mock).        */
/* -------------------------------------------------------------------- */

int vfs_commit(const char *path)
{
    printf("[MOCKVFS] vfs_commit path=\"%s\" (no-op)\n", MOCK_PATH(path));
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_create: log and accept (no file is actually created).            */
/* -------------------------------------------------------------------- */

int vfs_create(const char *path, uint32_t mode)
{
    printf("[MOCKVFS] vfs_create path=\"%s\" mode=0%o (ignored)\n",
           MOCK_PATH(path), (unsigned)mode);
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_remove: log and accept (no file is actually removed).            */
/* -------------------------------------------------------------------- */

int vfs_remove(const char *path)
{
    printf("[MOCKVFS] vfs_remove path=\"%s\" (ignored)\n", MOCK_PATH(path));
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_rename: log and reject with EPERM.                               */
/* -------------------------------------------------------------------- */

int vfs_rename(const char *from, const char *to)
{
    /* Two separate calls: MOCK_PATH uses a single static buffer so   */
    /* both paths cannot be in the same printf argument list.         */
    printf("[MOCKVFS] vfs_rename from=\"%s\"", MOCK_PATH(from));
    printf(" to=\"%s\" -> EPERM\n",            MOCK_PATH(to));
    errno = EPERM;
    return -1;
}

/* -------------------------------------------------------------------- */
/* vfs_truncate: log and accept (no change is made).                    */
/* -------------------------------------------------------------------- */

int vfs_truncate(const char *path, uint64_t size)
{
    printf("[MOCKVFS] vfs_truncate path=\"%s\" size=%u (ignored)\n",
           MOCK_PATH(path), (unsigned)size);
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_set_times: log and accept (no change is made).                   */
/* -------------------------------------------------------------------- */

int vfs_set_times(const char *path,
                  int set_atime, uint32_t atime_sec, uint32_t atime_nsec,
                  int set_mtime, uint32_t mtime_sec, uint32_t mtime_nsec)
{
    printf("[MOCKVFS] vfs_set_times path=\"%s\""
           " set_atime=%d set_mtime=%d (ignored)\n",
           MOCK_PATH(path), set_atime, set_mtime);
    (void)atime_sec;  (void)atime_nsec;
    (void)mtime_sec;  (void)mtime_nsec;
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_fsstat: return fixed read-only mock filesystem statistics.       */
/* -------------------------------------------------------------------- */

int vfs_fsstat(const char *path, vfs_fsstat_t *fs)
{
    printf("[MOCKVFS] vfs_fsstat path=\"%s\"\n", MOCK_PATH(path));

    fs->total_bytes = (uint64_t)1024u * 1024u * 64u; /* 64 MB nominal  */
    fs->free_bytes  = 0u;
    fs->avail_bytes = 0u;
    fs->total_files = (uint64_t)MOCK_NUM_FILES;
    fs->free_files  = 0u;
    fs->avail_files = 0u;
    fs->invarsec    = 3600u; /* statistics stable for one hour           */
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_opendir: open a mock directory handle for the given path.        */
/* -------------------------------------------------------------------- */

vfs_dir_t *vfs_opendir(const char *path, uint64_t cookie)
{
    int exp_idx;
    int file_idx;
    int kind;
    int i;

    printf("[MOCKVFS] vfs_opendir path=\"%s\"\n", MOCK_PATH(path));

    kind = mock_resolve(path, &exp_idx, &file_idx);
    if (kind == MOCK_NOT_FOUND) {
        errno = ENOENT;
        return NULL;
    }
    if (kind == MOCK_IS_FILE) {
        errno = ENOTDIR;
        return NULL;
    }

    /* Find a free slot in the pool */
    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        if (!g_dir_used[i]) break;
    }
    if (i == MAX_OPEN_DIRS) {
        errno = EMFILE;
        return NULL;
    }

    g_dir_pool[i].in_use      = 1;
    g_dir_pool[i].next_cookie = 1u;
    g_dir_pool[i].exp_idx     = exp_idx;
    g_dir_used[i]             = 1;

    vfs_seekdir_to(&g_dir_pool[i], cookie);

    return &g_dir_pool[i];
}

/* -------------------------------------------------------------------- */
/* vfs_readdir_next: return the next directory entry.                   */
/*                                                                      */
/* Fills name, *fileid, and *cookie.  Returns 0 on success, -1 at end. */
/*                                                                      */
/* File IDs match those returned by vfs_stat:                           */
/*   "."       -> (exp_idx+1)*10        (same as the directory itself)  */
/*   ".."      -> 1                     (generic parent sentinel)        */
/*   fileN.txt -> (exp_idx+1)*10 + N    (N is 1-based)                  */
/* -------------------------------------------------------------------- */

int vfs_readdir_next(vfs_dir_t *d, char *name,
                     uint32_t maxname, uint64_t *fileid,
                     uint64_t *cookie)
{
    int      pos;
    uint32_t base_id;

    printf("[MOCKVFS] vfs_readdir_next\n");

    pos = (int)(d->next_cookie - 1u);
    if (pos < 0 || pos >= MOCK_NUM_ENTRIES) {
        return -1; /* end of directory */
    }

    strncpy(name, s_dir_entries[pos], (size_t)(maxname - 1u));
    name[maxname - 1u] = '\0';
#ifdef __MVS__
    /* s_dir_entries[] is EBCDIC; NFS clients require ASCII names */
    printf("[MOCKVFS] vfs_readdir_next returning file-name=%s\n", name);
    ebcdic_to_ascii((uint8_t *)name, (const uint8_t *)name, strlen(name));
#endif

    base_id = (uint32_t)(d->exp_idx + 1) * 10u;

    if (pos == 0) {
        *fileid = (uint64_t)base_id;       /* "."  -- directory itself  */
    } else if (pos == 1) {
        *fileid = 1u;                       /* ".." -- parent sentinel   */
    } else {
        /* pos 2..6 -> file index 1..5 (1-based) */
        *fileid = (uint64_t)(base_id + (uint32_t)(pos - 1));
    }

    *cookie = d->next_cookie;
    d->next_cookie++;
    return 0;
}

/* -------------------------------------------------------------------- */
/* vfs_seekdir_to: reposition so the next readdir_next call returns     */
/* the entry after the one that bore the given cookie value.            */
/* cookie=0 restarts from the beginning.                                */
/* -------------------------------------------------------------------- */

void vfs_seekdir_to(vfs_dir_t *d, uint64_t cookie)
{
    printf("[MOCKVFS] vfs_seekdir_to cookie=%u\n", (unsigned)cookie);
    d->next_cookie = cookie + 1u;
}

/* -------------------------------------------------------------------- */
/* vfs_closedir: return the directory handle slot to the pool.          */
/* -------------------------------------------------------------------- */

void vfs_closedir(vfs_dir_t *d)
{
    int idx;
    printf("[MOCKVFS] vfs_closedir\n");
    if (!d) return;
    idx = (int)(d - g_dir_pool);
    if (idx >= 0 && idx < MAX_OPEN_DIRS) {
        g_dir_pool[idx].in_use = 0;
        g_dir_used[idx]        = 0;
    }
}
