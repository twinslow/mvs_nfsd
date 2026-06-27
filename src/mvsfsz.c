/*
 * mvsfsz.c - PDS member file-size cache.
 *
 * See mvsfsz.h for the full API description.
 *
 * Implementation: fixed-size static array with a parallel in-use flag
 * array and a running count.  Lookup is a linear scan keyed on
 * (dsname, member_name).  This is intentionally simple to make future
 * persistence straightforward -- the entire g_cache array can be written
 * to or read from a sequential dataset without structural change.
 *
 * JCC C89 compliance: block comments only; all declarations precede
 * executable statements within each function body.
 */

#include <string.h>

#include "mvsfsz.h"

/* -------------------------------------------------------------------- */
/* Internal state                                                        */
/* -------------------------------------------------------------------- */

static mvsfsz_entry_t g_cache[MVSFSZ_CACHE_CAPACITY];
static int            g_in_use[MVSFSZ_CACHE_CAPACITY];
static int            g_count;

/* -------------------------------------------------------------------- */
/* Internal helpers                                                      */
/* -------------------------------------------------------------------- */

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
        if (strcmp(g_cache[i].dsname,      dsname)      == 0 &&
            strcmp(g_cache[i].member_name, member_name) == 0)
            return i;
    }
    return -1;
}

/*
 * find_free_slot: return the index of the first unused slot,
 * or -1 if the cache is full.
 */
static int find_free_slot(void)
{
    int i;

    for (i = 0; i < MVSFSZ_CACHE_CAPACITY; i++) {
        if (!g_in_use[i])
            return i;
    }
    return -1;
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

/* -------------------------------------------------------------------- */
/* Public API                                                            */
/* -------------------------------------------------------------------- */

void mvsfsz_init(void)
{
    memset(g_cache,  0, sizeof(g_cache));
    memset(g_in_use, 0, sizeof(g_in_use));
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
        /* No existing entry -- allocate a new slot. */
        idx = find_free_slot();
        if (idx < 0)
            return -2;  /* cache full */
        g_in_use[idx] = 1;
        g_count++;
    }

    fill_entry(idx, dsname, member_name,
               file_size, ttr_tt, ttr_r, ispf_size, ispf_mtime);
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

    memset(&g_cache[idx], 0, sizeof(mvsfsz_entry_t));
    g_in_use[idx] = 0;
    g_count--;
    return 0;
}

int mvsfsz_count(void)
{
    return g_count;
}
