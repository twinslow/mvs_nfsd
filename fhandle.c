/*
 * fhandle.c - File handle construction and path resolution cache.
 *
 * FILE HANDLE LAYOUT (16 bytes, big-endian on wire):
 *   bytes  0- 3: magic   = OUR_FH_MAGIC ('NFS3')
 *   bytes  4- 7: export_id
 *   bytes  8-11: dev      (st_dev truncated to 32 bits)
 *   bytes 12-15: ino      (st_ino truncated to 32 bits)
 *
 * All encoding is explicit byte-by-byte so the code is correct on
 * both little-endian (Linux x86_64) and big-endian (MVS) hosts.
 *
 * PATH CACHE
 * ----------
 * NFS requires that a file handle resolve to a path on every call, but
 * inodes alone don't give us a path.  We maintain a fixed-size cache
 * keyed on (export_id, dev, ino) -> relative path from export root.
 *
 * The cache is a flat array with round-robin eviction.  Capacity is
 * FH_CACHE_SIZE (512) entries.  Entries are populated:
 *   - On MOUNTPROC3_MNT (root of export, relpath="")
 *   - On NFS3PROC_LOOKUP (child path is parent_relpath + "/" + name)
 *   - On NFS3PROC_READDIRPLUS (all entries in a directory scan)
 *
 * For the MVS port: replace dev/ino with dataset-index / member-index,
 * or a hash of the dataset + member name, in fh_make().
 */

#include <string.h>   /* memset, strncpy, strcmp, strlen, strrchr */
#include <stdio.h>    /* snprintf */
#include "nfsd.h"

/* ------------------------------------------------------------------ */
/* Cache storage                                                        */
/* ------------------------------------------------------------------ */
static fh_cache_entry_t g_cache[FH_CACHE_SIZE];
static int              g_cache_next  = 0;   /* next eviction slot    */
static int              g_cache_count = 0;   /* entries ever inserted */

/* ------------------------------------------------------------------ */
/* fh_init: clear the cache at startup                                  */
/* ------------------------------------------------------------------ */
void fh_init(void)
{
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_next  = 0;
    g_cache_count = 0;
}

/* ------------------------------------------------------------------ */
/* fh_make: construct a file handle from export_id + dev + ino          */
/* ------------------------------------------------------------------ */
void fh_make(our_fhandle_t *fh, uint32_t export_id,
             uint32_t dev, uint32_t ino)
{
    fh->magic     = OUR_FH_MAGIC;
    fh->export_id = export_id;
    fh->dev       = dev;
    fh->ino       = ino;
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
/* Returns 0 on success, -1 if the magic is wrong or len is bad.        */
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
/* fh_cache_insert: add or update a (export_id, dev, ino) -> relpath    */
/* entry.  relpath is relative to the export root with NO leading '/'.  */
/* The empty string "" means the export root itself.                    */
/* ------------------------------------------------------------------ */
void fh_cache_insert(uint32_t export_id, uint32_t dev,
                     uint32_t ino, const char *relpath)
{
    int i;
    int limit = (g_cache_count < FH_CACHE_SIZE)
                    ? g_cache_count : FH_CACHE_SIZE;

    /* Update existing entry if present */
    for (i = 0; i < limit; i++) {
        if (g_cache[i].valid
            && g_cache[i].export_id == export_id
            && g_cache[i].dev       == dev
            && g_cache[i].ino       == ino) {
            strncpy(g_cache[i].relpath, relpath, MAX_PATH - 1);
            g_cache[i].relpath[MAX_PATH - 1] = '\0';
            return;
        }
    }

    /* Insert into next eviction slot */
    g_cache[g_cache_next].valid     = 1;
    g_cache[g_cache_next].export_id = export_id;
    g_cache[g_cache_next].dev       = dev;
    g_cache[g_cache_next].ino       = ino;
    strncpy(g_cache[g_cache_next].relpath, relpath, MAX_PATH - 1);
    g_cache[g_cache_next].relpath[MAX_PATH - 1] = '\0';

    g_cache_next = (g_cache_next + 1) % FH_CACHE_SIZE;
    if (g_cache_count < FH_CACHE_SIZE) g_cache_count++;
}

/* ------------------------------------------------------------------ */
/* fh_cache_lookup: find the relpath for (export_id, dev, ino).         */
/* Copies at most maxlen-1 bytes into relpath (NUL-terminated).         */
/* Returns 0 on success, -1 if not found.                               */
/* ------------------------------------------------------------------ */
int fh_cache_lookup(uint32_t export_id, uint32_t dev,
                    uint32_t ino, char *relpath, uint32_t maxlen)
{
    int i;
    int limit = (g_cache_count < FH_CACHE_SIZE)
                    ? g_cache_count : FH_CACHE_SIZE;

    for (i = 0; i < limit; i++) {
        if (g_cache[i].valid
            && g_cache[i].export_id == export_id
            && g_cache[i].dev       == dev
            && g_cache[i].ino       == ino) {
            strncpy(relpath, g_cache[i].relpath, maxlen - 1);
            relpath[maxlen - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* fh_resolve: convert a file handle to an absolute path on this host.  */
/*                                                                      */
/* Looks up the relpath in the cache, then prepends the export's        */
/* host_path.  Writes the result into abspath (maxlen bytes).           */
/* Returns 0 on success, -1 if the handle is stale or unknown.          */
/* ------------------------------------------------------------------ */
int fh_resolve(const our_fhandle_t *fh, char *abspath, uint32_t maxlen)
{
    char      relpath[MAX_PATH];
    export_t *exp;

    exp = exports_find_by_id(fh->export_id);
    if (!exp) return -1;

    if (fh_cache_lookup(fh->export_id, fh->dev, fh->ino,
                        relpath, MAX_PATH) < 0) {
        return -1;
    }

    if (relpath[0] == '\0') {
        /* The handle IS the export root */
        strncpy(abspath, exp->host_path, maxlen - 1);
        abspath[maxlen - 1] = '\0';
    } else {
        snprintf(abspath, maxlen, "%s/%s", exp->host_path, relpath);
    }
    return 0;
}
