/*
 * mvsfsz.c - PDS member file-size cache.
 *
 * See mvsfsz.h for the full API description.
 *
 * Implementation notes:
 *
 *   Storage: three parallel static arrays of MVSFSZ_CACHE_CAPACITY elements:
 *     g_cache[]     -- the cache entries (dsname, member, size, validity fields)
 *     g_in_use[]    -- 1 if the slot is occupied, 0 if free
 *     g_last_used[] -- time_t stamp updated on every write or validated read
 *
 *   Eviction: when the cache is full find_free_slot() evicts the entry with
 *   the oldest g_last_used[] timestamp (LRU).  Ties are broken by index
 *   order (lowest index wins).
 *
 *   mvsfsz_get_member_size() is the primary API: it checks the cache,
 *   validates with the live pds_member_entry_t, and falls back to opening
 *   the PDS member in text mode ("rt") when necessary.
 *
 * JCC C89 compliance: block comments only; all declarations precede
 * executable statements within each function body.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mvsfsz.h"
#include "mvsprf.h"

/* --------------------------------------------------------------------- */
/* Internal state                                                        */
/* --------------------------------------------------------------------- */

static mvsfsz_entry_t g_cache[MVSFSZ_CACHE_CAPACITY];
static int            g_in_use[MVSFSZ_CACHE_CAPACITY];
static time_t         g_last_used[MVSFSZ_CACHE_CAPACITY];
static int            g_count;

/* --------------------------------------------------------------------- */
/* Internal helpers                                                      */
/* --------------------------------------------------------------------- */

/*
 * find_entry: return the index of an in-use entry matching
 * (dsname, member_name), or -1 if no such entry exists.
 */
static int find_entry(const char *dsname, const char *member_name)
{
    int i;

    for (i = 0; i < MVSFSZ_CACHE_CAPACITY; i++) {
        if (!g_in_use[i])
            continue;
        if (strcmp(g_cache[i].member_name, member_name) == 0 &&
            strcmp(g_cache[i].dsname,      dsname)      == 0)
            return i;
    }
    return -1;
}

/*
 * find_free_slot: return the index of a free slot.
 *
 * If an empty slot exists it is returned directly.  Otherwise the entry
 * with the oldest g_last_used[] timestamp is evicted (LRU) and its slot
 * is returned.  The evicted entry is cleared and g_count is decremented;
 * the caller is expected to populate the slot and increment g_count.
 *
 * Always returns a valid index (never -1).
 */
static int find_free_slot(void)
{
    int    i;
    int    lru_idx;
    time_t lru_time;

    /* First pass: look for a genuinely empty slot. */
    for (i = 0; i < MVSFSZ_CACHE_CAPACITY; i++) {
        if (!g_in_use[i])
            return i;
    }

    /* Cache is full: evict the least recently used entry. */
    lru_idx  = 0;
    lru_time = g_last_used[0];
    for (i = 1; i < MVSFSZ_CACHE_CAPACITY; i++) {
        if (g_last_used[i] < lru_time) {
            lru_time = g_last_used[i];
            lru_idx  = i;
        }
    }

    memset(&g_cache[lru_idx], 0, sizeof(mvsfsz_entry_t));
    g_in_use[lru_idx]    = 0;
    g_last_used[lru_idx] = 0;
    g_count--;
    return lru_idx;
}

/*
 * fill_entry: write all data fields into the entry at index idx.
 */
static void fill_entry(
    int         idx,
    const char *dsname,
    const char *member_name,
    uint64_t    file_size,
    uint16_t    ttr_tt,
    uint8_t     ttr_r,
    int32_t     ispf_size,
    int32_t     ispf_mtime)
{
    mvsfsz_entry_t *e;

    e = &g_cache[idx];
    strncpy(e->dsname,      dsname,      MVSFSZ_DSNAME_LEN - 1);
    e->dsname[MVSFSZ_DSNAME_LEN - 1]      = '\0';
    strncpy(e->member_name, member_name, MVSFSZ_MEMBER_LEN - 1);
    e->member_name[MVSFSZ_MEMBER_LEN - 1] = '\0';
    e->file_size  = file_size;
    e->ttr_tt     = ttr_tt;
    e->ttr_r      = ttr_r;
    e->ispf_size  = ispf_size;
    e->ispf_mtime = ispf_mtime;
}

/*
 * evict_entry: clear a specific slot without going through find_free_slot.
 * Used when validation detects a stale entry that must be removed before
 * the member is re-read.
 */
static void evict_entry(int idx)
{
    memset(&g_cache[idx], 0, sizeof(mvsfsz_entry_t));
    g_in_use[idx]    = 0;
    g_last_used[idx] = 0;
    g_count--;
}

/* --------------------------------------------------------------------- */
/* Primary API                                                           */
/* --------------------------------------------------------------------- */

int mvsfsz_get_member_size(
    const char         *dsname,
    const char         *member_name,
    pds_member_entry_t *member_entry,
    uint64_t           *file_size_out)
{
    int      idx;
    char     filename[64]; /* "//DSN:" + dsname(44) + "(" + member(8) + ")" + NUL */
    FILE    *fp;
    char     buf[512];
    size_t   n;
    uint64_t computed_size;
    clock_t  t_start;

    t_start = clock();

    /* --- Cache lookup ------------------------------------------------ */
    idx = find_entry(dsname, member_name);
    if (idx >= 0) {
        if (g_cache[idx].ispf_size  == (int32_t)member_entry->size     &&
            g_cache[idx].ispf_mtime == member_entry->chgdate            &&
            g_cache[idx].ttr_tt     == member_entry->first_block_tt     &&
            g_cache[idx].ttr_r      == member_entry->first_block_rec) {
            /* Valid cache hit. */
            *file_size_out   = g_cache[idx].file_size;
            g_last_used[idx] = time(NULL);
            mvsprf_record(PERF_MVSFSZ_HIT, clock() - t_start);
            return 0;
        }
        /* Stale entry: evict before re-reading. */
        evict_entry(idx);
    }

    /* --- Open and read in text mode ---------------------------------- */
    strcpy(filename,  "//DSN:");
    strncat(filename, dsname,       MVSFSZ_DSNAME_LEN - 1);
    strcat(filename,  "(");
    strncat(filename, member_name,  MVSFSZ_MEMBER_LEN - 1);
    strcat(filename,  ")");

    fp = fopen(filename, "rt");
    if (fp == NULL)
        return -1;

    computed_size = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        computed_size += (uint64_t)n;

    fclose(fp);

    /* --- Cache the result -------------------------------------------- */
    mvsfsz_put(dsname, member_name, computed_size,
               member_entry->first_block_tt,
               member_entry->first_block_rec,
               (int32_t)member_entry->size,
               member_entry->chgdate);

    mvsprf_record(PERF_MVSFSZ_MISS, clock() - t_start);
    *file_size_out = computed_size;
    return 0;
}

/* --------------------------------------------------------------------- */
/* Cache primitives                                                      */
/* --------------------------------------------------------------------- */

void mvsfsz_init(void)
{
    memset(g_cache,     0, sizeof(g_cache));
    memset(g_in_use,    0, sizeof(g_in_use));
    memset(g_last_used, 0, sizeof(g_last_used));
    g_count = 0;
}

int mvsfsz_put(
    const char *dsname,
    const char *member_name,
    uint64_t    file_size,
    uint16_t    ttr_tt,
    uint8_t     ttr_r,
    int32_t     ispf_size,
    int32_t     ispf_mtime)
{
    int idx;

    /* Update an existing entry if present (upsert semantics). */
    idx = find_entry(dsname, member_name);
    if (idx < 0) {
        /* No existing entry: allocate a slot (LRU evicts if full). */
        idx = find_free_slot();
        if (idx < 0)
            return -1; /* should not occur with LRU eviction */
        g_in_use[idx] = 1;
        g_count++;
    }

    fill_entry(idx, dsname, member_name,
               file_size, ttr_tt, ttr_r, ispf_size, ispf_mtime);
    g_last_used[idx] = time(NULL);
    return 0;
}

int mvsfsz_get(
    const char     *dsname,
    const char     *member_name,
    mvsfsz_entry_t *entry_out)
{
    int idx;

    idx = find_entry(dsname, member_name);
    if (idx < 0)
        return -1;

    *entry_out = g_cache[idx];
    return 0;
}

int mvsfsz_invalidate(
    const char *dsname,
    const char *member_name)
{
    int idx;

    idx = find_entry(dsname, member_name);
    if (idx < 0)
        return -1;

    evict_entry(idx);
    return 0;
}

int mvsfsz_count(void)
{
    return g_count;
}

int mvsfsz_load(const char *filename)
{
    FILE          *fp;
    char           line[256];
    char           dsname[MVSFSZ_DSNAME_LEN];
    char           member[MVSFSZ_MEMBER_LEN];
    unsigned long  size_ul;
    int            loaded;
    int            rc;
    clock_t        t_start;

    t_start = clock();

    fp = fopen(filename, "rt");
    if (fp == NULL)
        return -1;

    loaded = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        /* Skip comment lines and lines that don't parse as three fields. */
        if (line[0] == '#')
            continue;
        rc = sscanf(line, "%44s %8s %lu", dsname, member, &size_ul);
        if (rc != 3)
            continue;

        /* Validity fields are zeroed; mvsfsz_get_member_size() will
         * detect the mismatch and recompute the size on first access. */
        rc = mvsfsz_put(dsname, member, (uint64_t)size_ul, 0, 0, 0, 0);
        if (rc == 0)
            loaded++;
    }

    fclose(fp);
    mvsprf_record(PERF_MVSFSZ_LOAD, clock() - t_start);
    return loaded;
}
