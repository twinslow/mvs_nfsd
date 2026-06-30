/*
 * mvsdol.c - MVS directory open-list (DOL) pool management.
 *
 * Implements the pool of open vfs_dir_t directory handles used by the
 * MVS VFS layer.  See mvsdol.h for the public API.
 *
 * JCC C89 compliance: all variable declarations precede executable
 * statements within each function body.  Block comments only.
 */

#include <string.h>
#include <errno.h>
#include <time.h>

#include "nfsd.h"     /* MAX_OPEN_DIRS */
#include "mvsdol.h"
#include "logger.h"

/* -------------------------------------------------------------------- */
/* Static pool of directory handles -- no malloc required               */
/* -------------------------------------------------------------------- */

static vfs_dir_t g_dir_pool[MAX_OPEN_DIRS];

/* -------------------------------------------------------------------- */
/* Initialize the vfs_dir_t pool                                        */
/* -------------------------------------------------------------------- */

void dir_openlist_init(void)
{
    memset(g_dir_pool, 0, sizeof(g_dir_pool));
}

/* -------------------------------------------------------------------- */
/* Find a vfs_dir_t entry which is not being used                       */
/* Also reclaims USED entries that have exceeded the idle timeout.      */
/* -------------------------------------------------------------------- */

vfs_dir_t *dir_openlist_find_free(void)
{
    int    i;
    time_t now;
    int    lru_idx;
    double age;
    double lru_age;

    now     = time(NULL);
    lru_idx = 0;
    lru_age = difftime(now, g_dir_pool[0].last_used_time);

    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        if (g_dir_pool[i].status == MVSVFS_DIR_OPENLIST_FREE ||
                (g_dir_pool[i].status == MVSVFS_DIR_OPENLIST_USED &&
                 difftime(now, g_dir_pool[i].last_used_time) >
                         MVSVFS_DIR_OPENLIST_TIMEOUT_SECS)) {
            memset(&g_dir_pool[i], 0, sizeof(vfs_dir_t));
            g_dir_pool[i].last_used_time = now;
            return &g_dir_pool[i];
        }
        age = difftime(now, g_dir_pool[i].last_used_time);
        if (age > lru_age) {
            lru_age = age;
            lru_idx = i;
        }
    }
    /* All slots USED and non-expired: evict the least-recently-used    */
    log_debug("dir_openlist_find_free: evicting LRU slot %d (idle %ds)",
              lru_idx, (int)lru_age);
    memset(&g_dir_pool[lru_idx], 0, sizeof(vfs_dir_t));
    g_dir_pool[lru_idx].last_used_time = now;
    return &g_dir_pool[lru_idx];
}

/* -------------------------------------------------------------------- */
/* Mark the given entry as free and clean it up                         */
/* -------------------------------------------------------------------- */

void dir_openlist_free(vfs_dir_t *entry)
{
    memset(entry, 0, sizeof(vfs_dir_t));
    entry->status = MVSVFS_DIR_OPENLIST_FREE;
}

/* -------------------------------------------------------------------- */
/* Find a USED, non-expired entry by PDS dataset name                   */
/* -------------------------------------------------------------------- */

vfs_dir_t *dir_openlist_find_by_dsname(const char *pds_dsname_ebcdic)
{
    int    i;
    time_t now;

    now = time(NULL);
    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        if (g_dir_pool[i].status != MVSVFS_DIR_OPENLIST_USED)
            continue;
        if (difftime(now, g_dir_pool[i].last_used_time) >
                MVSVFS_DIR_OPENLIST_TIMEOUT_SECS)
            continue;
        if (strcmp(g_dir_pool[i].pds_dsname_ebcdic, pds_dsname_ebcdic) == 0) {
            g_dir_pool[i].last_used_time = now;
            return &g_dir_pool[i];
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------- */
/* Binary search of member info list (full operator set)                */
/* -------------------------------------------------------------------- */

int dir_openlist_search_members(
    vfs_dir_t           *dir_entry,
    const char          *member_name,
    int                  operator,
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
            if (operator & SEARCH_OP_EQ) {
                *member_info = &(dir_entry->members[mid_point]);
                return 0;
            }
            if (operator & SEARCH_OP_GT) {
                /* Exact match excluded; want strictly greater: search above */
                slice_low = mid_point + 1;
                break;
            }
            /* SEARCH_OP_LT: the answer is the entry immediately before */
            if (mid_point > 0) {
                *member_info = &(dir_entry->members[mid_point - 1]);
                return 0;
            }
            return -1;  /* exact match was the first entry */
        }
    }

    /*
     * No exact match (or exact match excluded by operator).
     * slice_low is the insertion point:
     *   members[slice_low-1].name  <  member_name
     *   members[slice_low].name    >  member_name
     */
    if (operator & SEARCH_OP_GT) {
        if (slice_low < num_in_list) {
            *member_info = &(dir_entry->members[slice_low]);
            return 0;
        }
    } else if (operator & SEARCH_OP_LT) {
        if (slice_low > 0) {
            *member_info = &(dir_entry->members[slice_low - 1]);
            return 0;
        }
    }

    return -1;
}
