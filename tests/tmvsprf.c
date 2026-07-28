/*
 * tests/tmvsprf.c - Unit tests for mvsprf.c (performance statistics).
 *
 * Suite prefix: /mvsprf
 * Sub-suites  : /init  /record  /record_ms  /get_stat  /dump
 *
 * Each test calls mvsprf_init() to start from a clean slate.
 * mvsprf_dump() is exercised in a smoke-test: we confirm it does not
 * crash and that it returns without touching any state.
 *
 * Build (from project root):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c \
 *      tests/tmvsprf.c \
 *      src/mvsprf.c src/logger.c tests/munit.c \
 *      -o tests/runall_prf
 */

#include "munit.h"
#include "mvsprf.h"
#include <string.h>

/* ==================================================================== */
/* /init                                                                 */
/* ==================================================================== */

static MunitResult test_init_zeroes_counts(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    int           i;
    int           rc;
    (void)params; (void)data;

    mvsprf_init();

    for (i = 0; i < PERF_NUM_SLOTS; i++) {
        rc = mvsprf_get_stat(i, &s);
        munit_assert_int(rc, ==, 0);
        munit_assert_int((int)s.count, ==, 0);
    }
    return MUNIT_OK;
}

static MunitResult test_init_names_not_null(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    int           i;
    (void)params; (void)data;

    mvsprf_init();

    for (i = 0; i < PERF_NUM_SLOTS; i++) {
        mvsprf_get_stat(i, &s);
        munit_assert_not_null((void *)s.name);
    }
    return MUNIT_OK;
}

static MunitTest init_tests[] = {
    { "/zeroes_counts",  test_init_zeroes_counts,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/names_not_null", test_init_names_not_null, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /record                                                               */
/* ==================================================================== */

static MunitResult test_record_increments_count(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_NFS3_READ, (clock_t)100);

    mvsprf_get_stat(PERF_NFS3_READ, &s);
    munit_assert_int((int)s.count, ==, 1);
    return MUNIT_OK;
}

static MunitResult test_record_accumulates_total(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_NFS3_READ, (clock_t)100);
    mvsprf_record(PERF_NFS3_READ, (clock_t)200);
    mvsprf_record(PERF_NFS3_READ, (clock_t) 50);

    mvsprf_get_stat(PERF_NFS3_READ, &s);
    munit_assert_int((int)s.count, ==, 3);
    munit_assert_int((int)s.total, ==, 350);
    return MUNIT_OK;
}

static MunitResult test_record_tracks_min(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_NFS3_GETATTR, (clock_t)300);
    mvsprf_record(PERF_NFS3_GETATTR, (clock_t) 10);
    mvsprf_record(PERF_NFS3_GETATTR, (clock_t)150);

    mvsprf_get_stat(PERF_NFS3_GETATTR, &s);
    munit_assert_int((int)s.min, ==, 10);
    return MUNIT_OK;
}

static MunitResult test_record_tracks_max(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_NFS3_LOOKUP, (clock_t) 10);
    mvsprf_record(PERF_NFS3_LOOKUP, (clock_t)999);
    mvsprf_record(PERF_NFS3_LOOKUP, (clock_t) 50);

    mvsprf_get_stat(PERF_NFS3_LOOKUP, &s);
    munit_assert_int((int)s.max, ==, 999);
    return MUNIT_OK;
}

static MunitResult test_record_first_sample_sets_min_max(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_NFS3_WRITE, (clock_t)42);

    mvsprf_get_stat(PERF_NFS3_WRITE, &s);
    munit_assert_int((int)s.min, ==, 42);
    munit_assert_int((int)s.max, ==, 42);
    return MUNIT_OK;
}

static MunitResult test_record_slots_are_independent(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t r;
    mvsprf_stat_t w;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_NFS3_READ,  (clock_t)100);
    mvsprf_record(PERF_NFS3_READ,  (clock_t)200);
    mvsprf_record(PERF_NFS3_WRITE, (clock_t)500);

    mvsprf_get_stat(PERF_NFS3_READ,  &r);
    mvsprf_get_stat(PERF_NFS3_WRITE, &w);

    munit_assert_int((int)r.count, ==, 2);
    munit_assert_int((int)r.total, ==, 300);
    munit_assert_int((int)w.count, ==, 1);
    munit_assert_int((int)w.total, ==, 500);
    return MUNIT_OK;
}

static MunitResult test_record_invalid_slot_noop(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    int           i;
    (void)params; (void)data;

    mvsprf_init();

    /* These must not crash and must not affect any valid slot. */
    mvsprf_record(-1,              (clock_t)999);
    mvsprf_record(PERF_NUM_SLOTS,  (clock_t)999);
    mvsprf_record(PERF_NUM_SLOTS + 100, (clock_t)999);

    for (i = 0; i < PERF_NUM_SLOTS; i++) {
        mvsprf_get_stat(i, &s);
        munit_assert_int((int)s.count, ==, 0);
    }
    return MUNIT_OK;
}

static MunitResult test_record_zero_elapsed_counted(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_NFS3_FSSTAT, (clock_t)0);

    mvsprf_get_stat(PERF_NFS3_FSSTAT, &s);
    munit_assert_int((int)s.count, ==, 1);
    munit_assert_int((int)s.total, ==, 0);
    munit_assert_int((int)s.min,   ==, 0);
    munit_assert_int((int)s.max,   ==, 0);
    return MUNIT_OK;
}

static MunitTest record_tests[] = {
    { "/increments_count",          test_record_increments_count,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/accumulates_total",         test_record_accumulates_total,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/tracks_min",                test_record_tracks_min,                NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/tracks_max",                test_record_tracks_max,                NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/first_sample_sets_min_max", test_record_first_sample_sets_min_max, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/slots_are_independent",     test_record_slots_are_independent,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/invalid_slot_noop",         test_record_invalid_slot_noop,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/zero_elapsed_counted",      test_record_zero_elapsed_counted,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /record_ms                                                            */
/*                                                                       */
/* PERF_PWW_WRITE_GAP holds wall-clock milliseconds rather than clock_t  */
/* ticks, because it measures the delay BETWEEN client WRITE requests --  */
/* time the server spends idle in select(), during which clock() does    */
/* not advance.  These tests pin the arithmetic; whether pww_write feeds  */
/* it the right samples is a matter for the write-pool tests.            */
/* ==================================================================== */

/*
 * The worked example from the original request:
 *
 *   WRITE offset 0    count 4096   -- starts the sequence (no sample)
 *   WRITE offset 4096 count 4096   -- +120 ms  -> gap 120
 *   WRITE offset 8192 count 350    -- +260 ms  -> gap 140
 *
 * so min = 120, max = 140, avg = 130 over two samples.
 */
static MunitResult test_record_ms_worked_example(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record_ms(PERF_PWW_WRITE_GAP, 120UL);
    mvsprf_record_ms(PERF_PWW_WRITE_GAP, 140UL);

    mvsprf_get_stat(PERF_PWW_WRITE_GAP, &s);
    munit_assert_int((int)s.count, ==, 2);
    munit_assert_int((int)s.min,   ==, 120);
    munit_assert_int((int)s.max,   ==, 140);
    munit_assert_int((int)s.total, ==, 260);
    /* avg is derived at dump time as total/count */
    munit_assert_int((int)(s.total / (clock_t)s.count), ==, 130);
    return MUNIT_OK;
}

/* A member written by a single request yields no gap at all, so the slot
   must stay completely untouched -- it cannot skew min/max/avg. */
static MunitResult test_record_ms_no_samples_leaves_slot_empty(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();

    mvsprf_get_stat(PERF_PWW_WRITE_GAP, &s);
    munit_assert_int((int)s.count, ==, 0);
    munit_assert_int((int)s.total, ==, 0);
    return MUNIT_OK;
}

static MunitResult test_record_ms_first_sample_sets_min_max(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record_ms(PERF_PWW_WRITE_GAP, 75UL);

    mvsprf_get_stat(PERF_PWW_WRITE_GAP, &s);
    munit_assert_int((int)s.count, ==, 1);
    munit_assert_int((int)s.min,   ==, 75);
    munit_assert_int((int)s.max,   ==, 75);
    return MUNIT_OK;
}

/* A zero-millisecond gap is real data (two writes inside the same tick),
   not a missing sample, so it must be counted and must lower min. */
static MunitResult test_record_ms_zero_gap_counted(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record_ms(PERF_PWW_WRITE_GAP, 50UL);
    mvsprf_record_ms(PERF_PWW_WRITE_GAP, 0UL);

    mvsprf_get_stat(PERF_PWW_WRITE_GAP, &s);
    munit_assert_int((int)s.count, ==, 2);
    munit_assert_int((int)s.min,   ==, 0);
    munit_assert_int((int)s.max,   ==, 50);
    return MUNIT_OK;
}

static MunitResult test_record_ms_invalid_slot_noop(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record_ms(-1, 100UL);
    mvsprf_record_ms(PERF_NUM_SLOTS, 100UL);

    mvsprf_get_stat(PERF_PWW_WRITE_GAP, &s);
    munit_assert_int((int)s.count, ==, 0);
    return MUNIT_OK;
}

/* Dumping a millisecond slot must not go through the tick conversion.
   This only checks it does not crash or disturb the values; the printed
   figures are eyeballed on the console. */
static MunitResult test_record_ms_dump_no_crash(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t before;
    mvsprf_stat_t after;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record_ms(PERF_PWW_WRITE_GAP, 120UL);
    mvsprf_record_ms(PERF_PWW_WRITE_GAP, 140UL);

    mvsprf_get_stat(PERF_PWW_WRITE_GAP, &before);
    mvsprf_dump();
    mvsprf_get_stat(PERF_PWW_WRITE_GAP, &after);

    munit_assert_int((int)before.count, ==, (int)after.count);
    munit_assert_int((int)before.total, ==, (int)after.total);
    munit_assert_int((int)before.min,   ==, (int)after.min);
    munit_assert_int((int)before.max,   ==, (int)after.max);
    return MUNIT_OK;
}

static MunitTest record_ms_tests[] = {
    { "/worked_example",             test_record_ms_worked_example,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/no_samples_leaves_empty",    test_record_ms_no_samples_leaves_slot_empty,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/first_sample_sets_min_max",  test_record_ms_first_sample_sets_min_max,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/zero_gap_counted",           test_record_ms_zero_gap_counted,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/invalid_slot_noop",          test_record_ms_invalid_slot_noop,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/dump_no_crash",              test_record_ms_dump_no_crash,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /get_stat                                                             */
/* ==================================================================== */

static MunitResult test_get_stat_valid_slot(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    int           rc;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_MVSFSZ_HIT, (clock_t)77);

    rc = mvsprf_get_stat(PERF_MVSFSZ_HIT, &s);

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)s.count, ==, 1);
    munit_assert_int((int)s.total, ==, 77);
    return MUNIT_OK;
}

static MunitResult test_get_stat_invalid_slot(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    int           rc;
    (void)params; (void)data;

    mvsprf_init();

    rc = mvsprf_get_stat(-1, &s);
    munit_assert_int(rc, ==, -1);

    rc = mvsprf_get_stat(PERF_NUM_SLOTS, &s);
    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

static MunitResult test_get_stat_null_out(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    mvsprf_init();

    rc = mvsprf_get_stat(PERF_NFS3_READ, NULL);
    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

static MunitResult test_get_stat_name_matches_slot(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t s;
    (void)params; (void)data;

    mvsprf_init();

    mvsprf_get_stat(PERF_NFS3_READ,     &s);
    munit_assert_string_equal(s.name, "NFS3_READ");

    mvsprf_get_stat(PERF_NFS3_GETATTR,  &s);
    munit_assert_string_equal(s.name, "NFS3_GETATTR");

    mvsprf_get_stat(PERF_MVSFSZ_HIT,    &s);
    munit_assert_string_equal(s.name, "MVSFSZ_HIT");

    mvsprf_get_stat(PERF_MVSFSZ_MISS,   &s);
    munit_assert_string_equal(s.name, "MVSFSZ_MISS");

    mvsprf_get_stat(PERF_MVSFSZ_LOAD,   &s);
    munit_assert_string_equal(s.name, "MVSFSZ_LOAD");

    mvsprf_get_stat(PERF_VFS_STAT,      &s);
    munit_assert_string_equal(s.name, "VFS_STAT");
    return MUNIT_OK;
}

static MunitTest get_stat_tests[] = {
    { "/valid_slot",           test_get_stat_valid_slot,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/invalid_slot",         test_get_stat_invalid_slot,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/null_out",             test_get_stat_null_out,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/name_matches_slot",    test_get_stat_name_matches_slot, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /dump                                                                 */
/*                                                                       */
/* dump() writes to the logger; we cannot capture that output in a unit  */
/* test.  These tests verify that dump() runs without crashing both with */
/* no data and with data, and that it does not corrupt recorded stats.   */
/* ==================================================================== */

static MunitResult test_dump_no_data_no_crash(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_dump();  /* should log "no statistics recorded" and return */
    return MUNIT_OK;
}

static MunitResult test_dump_with_data_no_crash(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_NFS3_GETATTR,  (clock_t)10);
    mvsprf_record(PERF_NFS3_GETATTR,  (clock_t)20);
    mvsprf_record(PERF_NFS3_READ,     (clock_t)500);
    mvsprf_record(PERF_NFS3_RDIRPLUS, (clock_t)1000);
    mvsprf_record(PERF_MVSFSZ_HIT,    (clock_t)5);
    mvsprf_record(PERF_MVSFSZ_MISS,   (clock_t)850);
    mvsprf_record(PERF_MVSFSZ_LOAD,   (clock_t)200);

    mvsprf_dump();
    return MUNIT_OK;
}

static MunitResult test_dump_does_not_alter_stats(
    const MunitParameter params[], void *data)
{
    mvsprf_stat_t before;
    mvsprf_stat_t after;
    (void)params; (void)data;

    mvsprf_init();
    mvsprf_record(PERF_NFS3_WRITE, (clock_t)123);
    mvsprf_record(PERF_NFS3_WRITE, (clock_t)456);

    mvsprf_get_stat(PERF_NFS3_WRITE, &before);
    mvsprf_dump();
    mvsprf_get_stat(PERF_NFS3_WRITE, &after);

    munit_assert_int((int)after.count, ==, (int)before.count);
    munit_assert_int((int)after.total, ==, (int)before.total);
    munit_assert_int((int)after.min,   ==, (int)before.min);
    munit_assert_int((int)after.max,   ==, (int)before.max);
    return MUNIT_OK;
}

static MunitTest dump_tests[] = {
    { "/no_data_no_crash",      test_dump_no_data_no_crash,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/with_data_no_crash",    test_dump_with_data_no_crash,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/does_not_alter_stats",  test_dump_does_not_alter_stats,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/init",     init_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/record",    record_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/record_ms", record_ms_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/get_stat", get_stat_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/dump",     dump_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvsprf_suite = {
    "/mvsprf",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
