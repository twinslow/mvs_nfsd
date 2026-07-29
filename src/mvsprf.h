/*
 * mvsprf.h - Lightweight performance-statistics module.
 *
 * Tracks how long a fixed set of operations take and emits a summary
 * table via log_info() when the server shuts down.
 *
 * Usage pattern at a call site:
 *
 *   clock_t _t = clock();
 *   rc = some_function(...);
 *   mvsprf_record(PERF_NFS3_READ, clock() - _t);
 *
 * call mvsprf_init() once at startup and mvsprf_dump() at shutdown.
 *
 * Design constraints:
 *   - C89 compatible (JCC on MVS 3.8J).
 *   - No dynamic allocation; all state is in static arrays.
 *   - Uses clock() (CPU time) for sub-second resolution.
 *   - Times are accumulated as clock_t ticks; conversion to useful units
 *     is performed in mvsprf_dump() using CLOCKS_PER_SEC.
 */

#ifndef MVSPRF_H_INCLUDED
#define MVSPRF_H_INCLUDED

#include <time.h>   /* clock_t, CLOCKS_PER_SEC */

/* -------------------------------------------------------------------- */
/* Slot identifiers                                                      */
/* -------------------------------------------------------------------- */

/*
 * Each enumerator identifies one tracked operation.
 * Add new slots immediately before PERF_NUM_SLOTS.
 * The numeric values are array indices, so do not reorder or give
 * explicit values to any enumerator other than PERF_NFS3_GETATTR = 0.
 */
typedef enum {

    /* NFSv3 protocol procedures (RFC 1813) */
    PERF_NFS3_GETATTR  = 0,
    PERF_NFS3_SETATTR,
    PERF_NFS3_LOOKUP,
    PERF_NFS3_ACCESS,
    PERF_NFS3_READ,
    PERF_NFS3_WRITE,
    PERF_NFS3_CREATE,
    PERF_NFS3_REMOVE,
    PERF_NFS3_RENAME,
    PERF_NFS3_READDIR,
    PERF_NFS3_RDIRPLUS,  /* READDIRPLUS */
    PERF_NFS3_FSSTAT,
    PERF_NFS3_FSINFO,
    PERF_NFS3_PATHCONF,
    PERF_NFS3_COMMIT,

    /* VFS layer calls */
    PERF_VFS_STAT,
    PERF_VFS_PREAD,
    PERF_VFS_PWRITE,
    PERF_VFS_READDIR,

    /* PDS member file-size cache */
    PERF_MVSFSZ_HIT,    /* mvsfsz_get_member_size(): valid cache hit      */
    PERF_MVSFSZ_MISS,   /* mvsfsz_get_member_size(): cache miss / stale   */
    PERF_MVSFSZ_LOAD,   /* mvsfsz_load(): full file-load operation         */

    /* PDS directory open-list (DOL) pool cache */
    PERF_MVSPOOL_HIT,   /* mvsvfs_find_cached_member(): pool cache hit     */
    PERF_MVSPOOL_MISS,  /* mvsvfs_find_cached_member(): miss -> PDS rescan */

    /*
     * Gap between consecutive NFS WRITE requests for the SAME pending
     * member -- i.e. how fast the client streams a file at us, measured
     * from one WRITE arriving to the next.  One sample per gap, so a
     * member written by N requests contributes N-1 samples and a member
     * written by a single request contributes none.
     *
     * This slot is WALL-CLOCK MILLISECONDS, not clock_t ticks: the
     * interval is time the server spends idle in select() waiting for the
     * client, which consumes no CPU, so clock() would report ~0.  Record
     * it with mvsprf_record_ms(), never mvsprf_record().
     */
    PERF_PWW_WRITE_GAP,

    /*
     * The censored tail of PERF_PWW_WRITE_GAP: a pause so long that the
     * idle sweep gave up, flushed the member and released the slot --
     * after which the client carried on writing where it left off.  One
     * sample per such event, valued at the gap that caused it.
     *
     * Every sample is a member STOWed more than once, which permanently
     * abandons the blocks of the earlier copy (dead space until the PDS
     * is compressed).  So this is the statistic that says whether
     * PWW_IDLE_TIMEOUT_SECONDS is set too low: count == 0 means no write
     * sequence was ever cut short, and max says how much higher the
     * timeout would have had to be to avoid them all.
     *
     * It matters most for clients this system has never measured.  A
     * Linux client on a LAN peaks around 27 ms between writes, but
     * Windows clients are FILE_SYNC -- they never send COMMIT, so the
     * idle sweep is their ONLY flush trigger.
     *
     * Wall-clock MILLISECONDS -- record with mvsprf_record_ms().
     */
    PERF_PWW_LATE_GAP,

    /*
     * Write sequences that began at a NON-ZERO offset.  A normal client
     * writes a member from offset 0, so a first write anywhere else means
     * one of three things:
     *
     *   a) the idle sweep flushed and released the slot while the client
     *      was merely pausing, and it has now resumed -- the member gets
     *      STOWed twice and the earlier copy's blocks are abandoned;
     *   b) writes simply arrived out of order, which is HARMLESS: the
     *      zero-filled gap is overwritten when the earlier offsets turn
     *      up;
     *   c) a genuine random-access write that never fills the gap, which
     *      leaves binary zeros in the member.
     *
     * This slot counts all three, and needs no state to do it, so it can
     * never miss one.  The subset confirmed as (a) is also recorded in
     * PERF_PWW_LATE_GAP; the difference between the two counts is the
     * (b)+(c) population, which is worth investigating if it is not zero.
     *
     * COUNT is the only meaningful column here -- the recorded value is
     * always 0, so total/avg/min/max will read zero.
     */
    PERF_PWW_NZSTART,

    /*
     * How long an evicted member had been accumulating, measured from its
     * first write to the moment the pool evicted it.
     *
     * Eviction is worse than an idle-sweep flush: the sweep only takes
     * slots that have gone quiet, whereas eviction can cut a sequence
     * that is still being actively written.  It happens when all
     * PWW_MAX_PENDING slots are busy.
     *
     * count -- how often the pool is under pressure at all.
     * min   -- the alarming number: a YOUNG eviction means a member was
     *          cut off mid-write purely because the pool was full.
     * max   -- old evictions are benign; the idle sweep would have taken
     *          them shortly anyway.
     *
     * Wall-clock MILLISECONDS -- record with mvsprf_record_ms().
     */
    PERF_PWW_EVICT_AGE,


    /* Sentinel -- must remain last */
    PERF_NUM_SLOTS

} perf_slot_t;

/* -------------------------------------------------------------------- */
/* Statistics record                                                     */
/* -------------------------------------------------------------------- */

/*
 * Per-slot statistics.  All time values are in clock_t ticks; multiply
 * by 1000 / CLOCKS_PER_SEC to convert to milliseconds.
 *
 * min is only valid when count > 0 (initialised to 0 by mvsprf_init()).
 */
typedef struct {
    const char   *name;    /* human-readable label, set by mvsprf_init() */
    unsigned long count;   /* number of samples recorded                  */
    clock_t       total;   /* cumulative elapsed ticks                    */
    clock_t       min;     /* smallest single elapsed value               */
    clock_t       max;     /* largest  single elapsed value               */
} mvsprf_stat_t;

/* -------------------------------------------------------------------- */
/* API                                                                   */
/* -------------------------------------------------------------------- */

/*
 * mvsprf_init: clear all statistics.
 * Must be called once at program startup before any other mvsprf_* call.
 */
void mvsprf_init(void);

/*
 * mvsprf_record: add one timing sample to slot.
 *
 *   slot    -- a PERF_* enum value (0 to PERF_NUM_SLOTS-1).
 *   elapsed -- ticks measured by the caller: clock() - t_start.
 *
 * Out-of-range slot values and negative elapsed values are silently
 * ignored; the function never crashes on bad input.
 */
void mvsprf_record(int slot, clock_t elapsed);

/*
 * mvsprf_record_ms: add one sample already expressed in WALL-CLOCK
 * MILLISECONDS.
 *
 * For intervals that clock() cannot measure because the server is idle
 * (not burning CPU) for the duration -- the gap between two client
 * requests, for instance.  Such a slot stores milliseconds directly and
 * mvsprf_dump() prints it without the usual tick conversion.
 *
 * Use this ONLY for slots documented as millisecond slots -- currently
 * PERF_PWW_WRITE_GAP, PERF_PWW_LATE_GAP and PERF_PWW_EVICT_AGE, plus
 * PERF_PWW_NZSTART which records a value of 0 because only its count is
 * meaningful.  Mixing the two units in one slot would make its totals
 * meaningless.  Out-of-range slots are silently ignored.
 */
void mvsprf_record_ms(int slot, unsigned long ms);

/*
 * mvsprf_get_stat: copy the statistics for slot into *stat_out.
 *
 * Returns  0 on success.
 * Returns -1 if slot is out of range or stat_out is NULL.
 */
int mvsprf_get_stat(int slot, mvsprf_stat_t *stat_out);

/*
 * mvsprf_dump: write a statistics table to the application log.
 *
 * Only slots with at least one sample are included.  Times are shown in
 * milliseconds (total, average, min, max).  Call at program shutdown.
 */
void mvsprf_dump(void);

/*
 * mvsprf_handle_modify: parse and apply a MVS MODIFY command that targets
 * the performance stats module.  Currently recognises:
 *
 *     STATS LIST                        -- List out stats to console
 *     STATS RESET                       -- Reset (re-init) stats counters
 *
 * Returns:
 *    0  recognised as a stats command and applied,
 *   -1  recognised as a stats command but malformed (diagnostic logged),
 *    1  not a stats command (the caller should report it as unhandled).
 */
int mvsprf_handle_modify(const char * cmd);

#endif /* MVSPRF_H_INCLUDED */
