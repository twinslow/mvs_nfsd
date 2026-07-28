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
 * Use this ONLY for slots documented as millisecond slots (currently
 * PERF_PWW_WRITE_GAP); mixing the two units in one slot would make its
 * totals meaningless.  Out-of-range slots are silently ignored.
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
