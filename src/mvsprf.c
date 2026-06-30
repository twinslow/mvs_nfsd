/*
 * mvsprf.c - Lightweight performance-statistics module.
 *
 * See mvsprf.h for the API description.
 *
 * Time conversion note:
 *   Times are accumulated as raw clock_t ticks.  mvsprf_dump() converts
 *   to milliseconds using CLOCKS_PER_SEC.  Integer arithmetic is used
 *   throughout to avoid floating-point dependencies on MVS.
 *
 *   To avoid 32-bit overflow when computing  ticks * 1000 / cps,
 *   the division is performed first when CLOCKS_PER_SEC >= 1000, which
 *   gives millisecond precision without overflow.  When CLOCKS_PER_SEC
 *   is smaller (e.g. 100 on some MVS JCC builds), the multiplication is
 *   performed first; overflow cannot occur because a single elapsed value
 *   small enough to be meaningful fits within 32 bits.
 *
 * JCC C89 compliance: block comments only; all declarations precede
 * executable statements within each function body.
 */

#include <string.h>
#include <time.h>

#include "mvsprf.h"
#include "logger.h"

/* -------------------------------------------------------------------- */
/* Internal state                                                        */
/* -------------------------------------------------------------------- */

static mvsprf_stat_t g_stats[PERF_NUM_SLOTS];

/*
 * Label strings indexed by perf_slot_t value.
 * Must stay in sync with the enum in mvsprf.h.
 */
static const char * const g_names[PERF_NUM_SLOTS] = {
    "NFS3_GETATTR",   /* PERF_NFS3_GETATTR  */
    "NFS3_SETATTR",   /* PERF_NFS3_SETATTR  */
    "NFS3_LOOKUP",    /* PERF_NFS3_LOOKUP   */
    "NFS3_ACCESS",    /* PERF_NFS3_ACCESS   */
    "NFS3_READ",      /* PERF_NFS3_READ     */
    "NFS3_WRITE",     /* PERF_NFS3_WRITE    */
    "NFS3_CREATE",    /* PERF_NFS3_CREATE   */
    "NFS3_REMOVE",    /* PERF_NFS3_REMOVE   */
    "NFS3_RENAME",    /* PERF_NFS3_RENAME   */
    "NFS3_READDIR",   /* PERF_NFS3_READDIR  */
    "NFS3_RDIRPLUS",  /* PERF_NFS3_RDIRPLUS */
    "NFS3_FSSTAT",    /* PERF_NFS3_FSSTAT   */
    "NFS3_FSINFO",    /* PERF_NFS3_FSINFO   */
    "NFS3_PATHCONF",  /* PERF_NFS3_PATHCONF */
    "NFS3_COMMIT",    /* PERF_NFS3_COMMIT   */
    "VFS_STAT",       /* PERF_VFS_STAT      */
    "VFS_PREAD",      /* PERF_VFS_PREAD     */
    "VFS_PWRITE",     /* PERF_VFS_PWRITE    */
    "VFS_READDIR",    /* PERF_VFS_READDIR   */
    "MVSFSZ_HIT",     /* PERF_MVSFSZ_HIT    */
    "MVSFSZ_MISS",    /* PERF_MVSFSZ_MISS   */
    "MVSFSZ_LOAD",    /* PERF_MVSFSZ_LOAD   */
    "MVSPOOL_HIT",    /* PERF_MVSPOOL_HIT   */
    "MVSPOOL_MISS"    /* PERF_MVSPOOL_MISS  */
};

/* -------------------------------------------------------------------- */
/* Helpers                                                               */
/* -------------------------------------------------------------------- */

/*
 * ticks_to_ms: convert clock_t ticks to whole milliseconds.
 *
 * To avoid 32-bit overflow in  ticks * 1000:
 *   - If CLOCKS_PER_SEC >= 1000, divide first (lose sub-ms precision but
 *     no overflow for any sane accumulated total).
 *   - Otherwise multiply first (safe because ticks is a single-sample
 *     elapsed value which is small by definition).
 *
 * For the total column the caller passes the accumulated sum.  Overflow
 * can still occur on very long-running servers; this is documented in
 * the header as an accepted limitation.
 */
static unsigned long ticks_to_ms(clock_t ticks)
{
    unsigned long cps;
    unsigned long t;

    cps = (unsigned long)CLOCKS_PER_SEC;
    t   = (unsigned long)ticks;

    if (cps >= 1000UL)
        return (t / (cps / 1000UL));
    else
        return (t * (1000UL / cps));
}

/* -------------------------------------------------------------------- */
/* API                                                                   */
/* -------------------------------------------------------------------- */

void mvsprf_init(void)
{
    int i;

    memset(g_stats, 0, sizeof(g_stats));
    for (i = 0; i < PERF_NUM_SLOTS; i++)
        g_stats[i].name = g_names[i];
}

void mvsprf_record(int slot, clock_t elapsed)
{
    mvsprf_stat_t *s;

    if (slot < 0 || slot >= PERF_NUM_SLOTS)
        return;
    if (elapsed < 0)
        return;

    s = &g_stats[slot];
    s->total += elapsed;
    s->count++;
    if (s->count == 1 || elapsed < s->min)
        s->min = elapsed;
    if (elapsed > s->max)
        s->max = elapsed;
}

int mvsprf_get_stat(int slot, mvsprf_stat_t *stat_out)
{
    if (slot < 0 || slot >= PERF_NUM_SLOTS || stat_out == NULL)
        return -1;
    *stat_out = g_stats[slot];
    return 0;
}

void mvsprf_dump(void)
{
    int            i;
    int            any;
    unsigned long  total_ms;
    unsigned long  avg_ms;
    unsigned long  min_ms;
    unsigned long  max_ms;
    mvsprf_stat_t *s;

    /* Check whether anything was recorded at all. */
    any = 0;
    for (i = 0; i < PERF_NUM_SLOTS; i++) {
        if (g_stats[i].count > 0) { any = 1; break; }
    }
    if (!any) {
        log_info("perf: no statistics recorded");
        return;
    }

    log_info("perf: CLOCKS_PER_SEC = %lu",
             (unsigned long)CLOCKS_PER_SEC);
    log_info("perf: %-16s %8s %12s %10s %10s %10s",
             "Slot", "Count", "Total(ms)", "Avg(ms)", "Min(ms)", "Max(ms)");
    log_info("perf: %-16s %8s %12s %10s %10s %10s",
             "----------------", "--------",
             "------------", "----------", "----------", "----------");

    for (i = 0; i < PERF_NUM_SLOTS; i++) {
        s = &g_stats[i];
        if (s->count == 0)
            continue;

        total_ms = ticks_to_ms(s->total);
        min_ms   = ticks_to_ms(s->min);
        max_ms   = ticks_to_ms(s->max);
        avg_ms   = (s->count > 0) ? (total_ms / s->count) : 0UL;

        log_info("perf: %-16s %8lu %12lu %10lu %10lu %10lu",
                 s->name, s->count, total_ms, avg_ms, min_ms, max_ms);
    }

    log_info("perf: %-16s %8s %12s %10s %10s %10s",
             "----------------", "--------",
             "------------", "----------", "----------", "----------");
}
