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

/* --------------------------------------------------------------------- */
/* Internal state                                                        */
/* --------------------------------------------------------------------- */

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
    "MVSPOOL_MISS",   /* PERF_MVSPOOL_MISS  */
    "PWW_WRITE_GAP",  /* PERF_PWW_WRITE_GAP (milliseconds)  */
    "PWW_LATE_GAP",   /* PERF_PWW_LATE_GAP  (milliseconds)  */
    "PWW_NZSTART",    /* PERF_PWW_NZSTART   (count only)    */
    "PWW_EVICT_AGE"   /* PERF_PWW_EVICT_AGE (milliseconds)  */
};

/* --------------------------------------------------------------------- */
/* Helpers                                                               */
/* --------------------------------------------------------------------- */

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

/* Advance past run of blanks. */
static const char *skip_blanks(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

/*
 * Copy the next token (uppercased) from s into out, stopping at a blank,
 * an '=', or end of string.  out is always NUL-terminated and never
 * overflows outlen.  Returns a pointer to the delimiter that stopped the
 * scan (the caller inspects it to distinguish "PROC=" from "PROC ").
 */
static const char *scan_token(const char *s, char *out, int outlen)
{
    int i = 0;
    while (*s != '\0' && *s != ' ' && *s != '=') {
        if (i < outlen - 1)
            out[i++] = (char)toupper((unsigned char)*s);
        s++;
    }
    out[i] = '\0';
    return s;
}

/* --------------------------------------------------------------------- */
/* API                                                                   */
/* --------------------------------------------------------------------- */

void mvsprf_init(void)
{
    int i;

    memset(g_stats, 0, sizeof(g_stats));
    for (i = 0; i < PERF_NUM_SLOTS; i++)
        g_stats[i].name = g_names[i];
}

/*
 * slot_is_ms: does this slot hold wall-clock milliseconds rather than
 * clock_t ticks?  Written as a predicate rather than a parallel table so
 * it cannot drift out of step with the enum when slots are added.
 */
static int slot_is_ms(int slot)
{
    return (slot == PERF_PWW_WRITE_GAP) ||
           (slot == PERF_PWW_LATE_GAP)  ||
           (slot == PERF_PWW_NZSTART)   ||
           (slot == PERF_PWW_EVICT_AGE);
}

void mvsprf_record_ms(int slot, unsigned long ms)
{
    mvsprf_stat_t *s;

    if (slot < 0 || slot >= PERF_NUM_SLOTS)
        return;

    s = &g_stats[slot];
    s->total += (clock_t)ms;
    s->count++;
    if (s->count == 1 || (clock_t)ms < s->min)
        s->min = (clock_t)ms;
    if ((clock_t)ms > s->max)
        s->max = (clock_t)ms;
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
        logmsg_info("NFSST010I", "perf: no statistics recorded");
        return;
    }

    logmsg_info("NFSST020I", "perf: CLOCKS_PER_SEC = %lu",
             (unsigned long)CLOCKS_PER_SEC);
    logmsg_info("NFSST030I", "perf: %-16s %8s %12s %10s %10s %10s",
             "Slot", "Count", "Total(ms)", "Avg(ms)", "Min(ms)", "Max(ms)");
    logmsg_info("NFSST040I", "perf: %-16s %8s %12s %10s %10s %10s",
             "----------------", "--------",
             "------------", "----------", "----------", "----------");

    for (i = 0; i < PERF_NUM_SLOTS; i++) {
        s = &g_stats[i];
        if (s->count == 0)
            continue;

        if (slot_is_ms(i)) {
            /* Already milliseconds -- converting would be wrong. */
            total_ms = (unsigned long)s->total;
            min_ms   = (unsigned long)s->min;
            max_ms   = (unsigned long)s->max;
        } else {
            total_ms = ticks_to_ms(s->total);
            min_ms   = ticks_to_ms(s->min);
            max_ms   = ticks_to_ms(s->max);
        }
        avg_ms   = (s->count > 0) ? (total_ms / s->count) : 0UL;

        logmsg_info("NFSST050I", "perf: %-16s %8lu %12lu %10lu %10lu %10lu",
                 s->name, s->count, total_ms, avg_ms, min_ms, max_ms);
    }

    logmsg_info("NFSST060I", "perf: %-16s %8s %12s %10s %10s %10s",
             "----------------", "--------",
             "------------", "----------", "----------", "----------");
}


/* -------------------------------------------------------------------- */
/* Command handling                                                     */
/* -------------------------------------------------------------------- */

/*
 * handle_stats_reset: Reset performance stats
 */
static int handle_stats_reset(const char *p)
{
    log_level_t saved_log_level;
    log_level_t saved_wto_level;

    saved_log_level = log_get_level();
    saved_wto_level = log_get_wto_level();

    log_set_level(LOG_INFO);
    log_set_wto_level(LOG_INFO);

    mvsprf_init();
    logmsg_info("NFSST070I", "mvsprf: Statistics have been reset");

    /* Restore previous log/wto level */
    log_set_level(saved_log_level);
    log_set_wto_level(saved_wto_level);
    return 0;
}

/*
 * handle_stats_list: List out performance stats to log and console
 */
static int handle_stats_list(const char *p)
{
    log_level_t saved_log_level;
    log_level_t saved_wto_level;

    saved_log_level = log_get_level();
    saved_wto_level = log_get_wto_level();
    log_set_level(LOG_INFO);
    log_set_wto_level(LOG_INFO);

    mvsprf_dump();

    /* Restore previous log/wto level */
    log_set_level(saved_log_level);
    log_set_wto_level(saved_wto_level);
    return 0;
}

int mvsprf_handle_modify(const char *cmd)
{
    char        tok[24];
    const char *p;

    if (cmd == NULL)
        return 1;

    /* Verb must be "SET" for us to look further. */
    p = skip_blanks(cmd);
    p = scan_token(p, tok, (int)sizeof(tok));
    if (strcmp(tok, "STATS") != 0)
        return 1;                       /* not a logger command          */

    /* Target keyword selects which threshold we adjust. */
    p = skip_blanks(p);
    p = scan_token(p, tok, (int)sizeof(tok));
    if (strcmp(tok, "LIST") == 0)
        return handle_stats_list(p);
    if (strcmp(tok, "RESET") == 0)
        return handle_stats_reset(p);

    return 1;                           /* "SET" but not one of ours     */
}

