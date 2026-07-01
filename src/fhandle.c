/*
 * fhandle.c - File handle construction and path-resolution cache.
 *
 * FILE HANDLE LAYOUT (16 bytes, big-endian on wire):
 *   bytes  0- 3: magic     = OUR_FH_MAGIC ('NFS3')
 *   bytes  4- 7: export_id
 *   bytes  8-11: reserved  = 0
 *   bytes 12-15: id32      -- stable 32-bit ID for this file
 *
 * The id32 is allocated from the path cache below.  Keying on
 * (dev, ino) from vfs_stat_t avoids the previous design where the same
 * raw truncated values were stored directly in the handle, making it
 * impossible to distinguish the stable ID from a raw inode number.
 *
 * PATH CACHE
 * ----------
 * A flat array of FH_CACHE_SIZE (512) entries.  Each entry stores:
 *   (export_id, dev, ino)  -- the filesystem identity (32-bit each)
 *   id32                   -- the stable token carried in the handle
 *   relpath                -- path relative to export root
 *
 * Two lookup modes are needed:
 *   - By (export_id, dev, ino): used when inserting / building handles.
 *   - By (export_id, id32):     used when resolving a received handle.
 *
 * id32 values are issued from a monotonically increasing counter.  With
 * a 512-entry cache a 32-bit counter will not wrap in practice.
 *
 * Round-robin eviction.  Entries are populated:
 *   - On MOUNTPROC3_MNT       (export root, relpath = "")
 *   - On NFS3PROC_LOOKUP      (child path built from parent + name)
 *   - On NFS3PROC_READDIRPLUS (every entry scanned in a directory)
 *
 * Portability note: all types are uint32_t so this file compiles
 * cleanly on both 64-bit Linux (x86_64) and 32-bit MVS with GCCMVS.
 * On Linux, vfs_stat_t.raw_dev and .raw_ino carry st_dev/st_ino
 * truncated to 32 bits (see vfs.c); on MVS the VFS layer maps PDS
 * dataset/member indices directly into these 32-bit fields.
 */

#include <string.h>   /* memset, strncpy, snprintf */
#include <stdio.h>

#include "ebcdic.h"
#include "nfsd.h"

/* ------------------------------------------------------------------ */
/* Cache entry                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    int      valid;
    uint32_t export_id;
    uint32_t dev;           /* raw_dev from vfs_stat_t */
    uint32_t ino;           /* raw_ino from vfs_stat_t */
    uint32_t id32;          /* stable ID carried in the file handle */
    char     relpath[MAX_PATH];
} cache_entry_t;

static cache_entry_t g_cache[FH_CACHE_SIZE];
static int           g_cache_next  = 0;   /* next eviction slot */
static int           g_cache_count = 0;   /* entries ever inserted (capped at FH_CACHE_SIZE) */
static uint32_t      g_id_seq      = 1;   /* id32 allocator; never issues 0 */

/* ------------------------------------------------------------------ */
/* fh_init: clear the cache at startup                                  */
/* ------------------------------------------------------------------ */
void fh_init(void)
{
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_next  = 0;
    g_cache_count = 0;
    g_id_seq      = 1;
}

/* ------------------------------------------------------------------ */
/* Internal: find slot by filesystem identity.  Returns -1 if absent.  */
/* ------------------------------------------------------------------ */
static int cache_find_by_ino(uint32_t export_id, uint32_t dev, uint32_t ino)
{
    int i;
    int limit = (g_cache_count < FH_CACHE_SIZE) ? g_cache_count : FH_CACHE_SIZE;
    for (i = 0; i < limit; i++) {
        if (g_cache[i].valid
            && g_cache[i].export_id == export_id
            && g_cache[i].dev       == dev
            && g_cache[i].ino       == ino)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Internal: find slot by stable id32.  Returns -1 if absent.          */
/* ------------------------------------------------------------------ */
static int cache_find_by_id(uint32_t export_id, uint32_t id32)
{
    int i;
    int limit = (g_cache_count < FH_CACHE_SIZE) ? g_cache_count : FH_CACHE_SIZE;
    for (i = 0; i < limit; i++) {
        if (g_cache[i].valid
            && g_cache[i].export_id == export_id
            && g_cache[i].id32      == id32)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Internal: evict the next slot and return its index.                  */
/* ------------------------------------------------------------------ */
static int cache_alloc_slot(void)
{
    int i = g_cache_next;
    g_cache_next = (g_cache_next + 1) % FH_CACHE_SIZE;
    if (g_cache_count < FH_CACHE_SIZE) g_cache_count++;
    return i;
}

/* ------------------------------------------------------------------ */
/* fh_make: construct a file handle from (export_id, dev, ino).         */
/*                                                                      */
/* Looks up or allocates a stable id32 for the (dev, ino) pair.        */
/* Sets fh->dev = 0 (reserved) and fh->ino = id32.                     */
/*                                                                      */
/* Separating id32 from the raw inode value means the handle's id32    */
/* is never confused with a raw ino, and the cache can be updated       */
/* without changing the handle the client holds.                        */
/* ------------------------------------------------------------------ */
void fh_make(our_fhandle_t *fh, uint32_t export_id,
             uint32_t dev, uint32_t ino)
{
    int      i;
    uint32_t id32;

    i = cache_find_by_ino(export_id, dev, ino);
    if (i >= 0) {
        id32 = g_cache[i].id32;
    } else {
        /* New entry: allocate slot and a fresh id32 */
        i    = cache_alloc_slot();
        id32 = g_id_seq++;
        if (g_id_seq == 0) g_id_seq = 1;   /* keep id32 != 0 */

        g_cache[i].valid     = 1;
        g_cache[i].export_id = export_id;
        g_cache[i].dev       = dev;
        g_cache[i].ino       = ino;
        g_cache[i].id32      = id32;
        g_cache[i].relpath[0] = '\0';
    }

    fh->magic     = OUR_FH_MAGIC;
    fh->export_id = export_id;
    fh->dev       = 0u;     /* reserved */
    fh->ino       = id32;
}

/* ------------------------------------------------------------------ */
/* fh_encode: write our_fhandle_t to 16 bytes in big-endian order       */
/* ------------------------------------------------------------------ */
void fh_encode(const our_fhandle_t *fh, uint8_t *bytes)
{
    bytes[ 0] = (uint8_t)(fh->magic     >> 24);
    bytes[ 1] = (uint8_t)(fh->magic     >> 16);
    bytes[ 2] = (uint8_t)(fh->magic     >>  8);
    bytes[ 3] = (uint8_t)(fh->magic         );
    bytes[ 4] = (uint8_t)(fh->export_id >> 24);
    bytes[ 5] = (uint8_t)(fh->export_id >> 16);
    bytes[ 6] = (uint8_t)(fh->export_id >>  8);
    bytes[ 7] = (uint8_t)(fh->export_id      );
    bytes[ 8] = (uint8_t)(fh->dev       >> 24);
    bytes[ 9] = (uint8_t)(fh->dev       >> 16);
    bytes[10] = (uint8_t)(fh->dev       >>  8);
    bytes[11] = (uint8_t)(fh->dev            );
    bytes[12] = (uint8_t)(fh->ino       >> 24);
    bytes[13] = (uint8_t)(fh->ino       >> 16);
    bytes[14] = (uint8_t)(fh->ino       >>  8);
    bytes[15] = (uint8_t)(fh->ino            );
}

/* ------------------------------------------------------------------ */
/* fh_decode: read our_fhandle_t from 'len' bytes (must be OUR_FHSIZE). */
/* Returns 0 on success, -1 if the magic is wrong or len is too small.  */
/* ------------------------------------------------------------------ */
int fh_decode(const uint8_t *bytes, uint32_t len, our_fhandle_t *fh)
{
    if (len < OUR_FHSIZE) return -1;

    fh->magic = ((uint32_t)bytes[ 0] << 24)
              | ((uint32_t)bytes[ 1] << 16)
              | ((uint32_t)bytes[ 2] <<  8)
              |  (uint32_t)bytes[ 3];

    if (fh->magic != OUR_FH_MAGIC) return -1;

    fh->export_id = ((uint32_t)bytes[ 4] << 24)
                  | ((uint32_t)bytes[ 5] << 16)
                  | ((uint32_t)bytes[ 6] <<  8)
                  |  (uint32_t)bytes[ 7];

    fh->dev = ((uint32_t)bytes[ 8] << 24)
            | ((uint32_t)bytes[ 9] << 16)
            | ((uint32_t)bytes[10] <<  8)
            |  (uint32_t)bytes[11];

    fh->ino = ((uint32_t)bytes[12] << 24)
            | ((uint32_t)bytes[13] << 16)
            | ((uint32_t)bytes[14] <<  8)
            |  (uint32_t)bytes[15];

    return 0;
}

/* ------------------------------------------------------------------ */
/* fh_cache_insert: record or refresh (export_id, dev, ino) -> relpath  */
/*                                                                      */
/* If an entry for (export_id, dev, ino) already exists, only the      */
/* relpath is updated (preserving the existing id32).  Otherwise a new  */
/* slot is evicted and a fresh id32 allocated.                          */
/*                                                                      */
/* relpath is relative to the export root with NO leading '/'.          */
/* The empty string "" represents the export root itself.               */
/* ------------------------------------------------------------------ */
void fh_cache_insert(uint32_t export_id, uint32_t dev,
                     uint32_t ino, const char *relpath)
{
    int      i;
    uint32_t id32;

    i = cache_find_by_ino(export_id, dev, ino);
    if (i >= 0) {
        /* Refresh relpath in existing entry */
        strncpy(g_cache[i].relpath, relpath, MAX_PATH - 1);
        g_cache[i].relpath[MAX_PATH - 1] = '\0';
        return;
    }

    /* New entry */
    i    = cache_alloc_slot();
    id32 = g_id_seq++;
    if (g_id_seq == 0) g_id_seq = 1;

    g_cache[i].valid     = 1;
    g_cache[i].export_id = export_id;
    g_cache[i].dev       = dev;
    g_cache[i].ino       = ino;
    g_cache[i].id32      = id32;
    strncpy(g_cache[i].relpath, relpath, MAX_PATH - 1);
    g_cache[i].relpath[MAX_PATH - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* fh_cache_lookup: find the relpath for (export_id, id32).             */
/*                                                                      */
/* id32 is the value stored in our_fhandle_t.ino (the file handle's    */
/* stable file ID).  Copies at most maxlen-1 bytes into relpath.        */
/* Returns 0 on success, -1 if not found (handle is stale).            */
/* ------------------------------------------------------------------ */
int fh_cache_lookup(uint32_t export_id, uint32_t id32,
                    char *relpath, uint32_t maxlen)
{
    int i = cache_find_by_id(export_id, id32);
    if (i < 0) return -1;
    strncpy(relpath, g_cache[i].relpath, maxlen - 1);
    relpath[maxlen - 1] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* fh_resolve: convert a file handle to its canonical NFS path.         */
/*                                                                      */
/* Looks up the relpath in the cache by (export_id, id32), then        */
/* prepends the export's NFS path (export_path).  The result is the    */
/* export-relative path the VFS layer classifies via mvs_path_type.    */
/* Returns 0 on success, -1 if the handle is stale or unknown.          */
/* ------------------------------------------------------------------ */
int fh_resolve(const our_fhandle_t *fh, char *abspath, uint32_t maxlen)
{
    char      relpath[MAX_PATH];
    export_t *exp;

    exp = exports_find_by_id(fh->export_id);
    if (!exp) return -1;

    /* fh->ino carries the stable id32 */
    if (fh_cache_lookup(fh->export_id, fh->ino, relpath, MAX_PATH) < 0)
        return -1;

    if (relpath[0] == '\0') {
        /* The handle IS the export root (a virtual directory). */
        strncpy(abspath, exp->export_path, maxlen - 1);
        abspath[maxlen - 1] = '\0';
    } else {
        /* export_path / relpath  (relpath is "<dirname>" or "<dirname>/<member>") */
        snprintf(abspath, maxlen, "%s%c%s", exp->export_path,
            ebcdic_to_ascii_c('/'), relpath);
    }
    return 0;
}
