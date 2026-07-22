/*
 * tests/tmvsutl.c - Unit tests for mvsutl.c (timezone epoch conversions).
 *
 * Suite prefix: /mvsutl
 * Sub-suites  : /offset /convert
 *
 * Scope: the PURE part of mvsutl.c -- the cached-offset accessor and the
 * LOCAL<->UTC epoch conversions used to correct ISPF stat times (see the
 * "MVS TOD is local" work).  The conversions depend only on the cached offset,
 * which mvs_tz_set_offset() sets directly, so the tests are deterministic and
 * platform-independent.
 *
 * NOT tested here: get_jes2_jobid / get_int_cvt_val / get_tz_offset and the CVT
 * read inside mvs_tz_init() -- they read fixed low-storage / CVT addresses and
 * only mean anything on a live MVS system.
 *
 * Sign convention (mvsutl.h): offset = local - GMT (negative west of GMT).
 *   mvs_local_epoch_to_utc(t) = t - offset   (a decoded ISPF local time -> UTC)
 *   mvs_utc_to_local_epoch(t) = t + offset   (UTC -> local, for encoding stats)
 *
 * Each test sets the offset it needs; a shared teardown restores 0 so the
 * offset never leaks into another suite.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include "munit.h"
#include "mvsutl.h"   /* mvs_tz_offset / set_offset / local<->utc conversions */

static int teq(time_t a, time_t b) { return a == b ? 1 : 0; }

/* Shared teardown: leave the global offset back at 0. */
static void reset_offset(void *fixture)
{
    (void)fixture;
    mvs_tz_set_offset(0);
}

/* ==================================================================== */
/* /offset -- mvs_tz_set_offset / mvs_tz_offset                         */
/* ==================================================================== */

static MunitResult test_set_get(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvs_tz_set_offset(-18000);
    munit_assert_int(mvs_tz_offset(), ==, -18000);

    mvs_tz_set_offset(19800);
    munit_assert_int(mvs_tz_offset(), ==, 19800);

    mvs_tz_set_offset(0);
    munit_assert_int(mvs_tz_offset(), ==, 0);
    return MUNIT_OK;
}

static MunitTest offset_tests[] = {
    { "/set_get", test_set_get, NULL, reset_offset, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /convert -- mvs_local_epoch_to_utc / mvs_utc_to_local_epoch         */
/* ==================================================================== */

/* Offset 0: both conversions are the identity. */
static MunitResult test_zero_identity(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvs_tz_set_offset(0);
    munit_assert_int(teq(mvs_local_epoch_to_utc((time_t)1000000), (time_t)1000000), ==, 1);
    munit_assert_int(teq(mvs_utc_to_local_epoch((time_t)1000000), (time_t)1000000), ==, 1);
    return MUNIT_OK;
}

/* West of GMT (e.g. US Eastern, -5h): local time is BEHIND UTC, so a local
   stamp converts to a LATER utc value (t - (-18000) = t + 18000). */
static MunitResult test_west_offset(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvs_tz_set_offset(-18000);
    munit_assert_int(teq(mvs_local_epoch_to_utc((time_t)1000000), (time_t)1018000), ==, 1);
    munit_assert_int(teq(mvs_utc_to_local_epoch((time_t)1000000), (time_t) 982000), ==, 1);
    return MUNIT_OK;
}

/* East of GMT (e.g. +5:30, 19800s): local is AHEAD, so local -> utc subtracts. */
static MunitResult test_east_offset(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvs_tz_set_offset(19800);
    munit_assert_int(teq(mvs_local_epoch_to_utc((time_t)1000000), (time_t) 980200), ==, 1);
    munit_assert_int(teq(mvs_utc_to_local_epoch((time_t)1000000), (time_t)1019800), ==, 1);
    return MUNIT_OK;
}

/* The two conversions are exact inverses, for any offset. */
static MunitResult test_inverse(const MunitParameter params[], void *data)
{
    static const int    offs[5] = { 0, 3600, -3600, -18000, 19800 };
    static const time_t ts[3]   = { (time_t)0, (time_t)1234567, (time_t)2000000000 };
    int i, j;
    (void)params; (void)data;

    for (i = 0; i < 5; i++) {
        mvs_tz_set_offset(offs[i]);
        for (j = 0; j < 3; j++) {
            time_t t = ts[j];
            munit_assert_int(teq(mvs_utc_to_local_epoch(mvs_local_epoch_to_utc(t)), t), ==, 1);
            munit_assert_int(teq(mvs_local_epoch_to_utc(mvs_utc_to_local_epoch(t)), t), ==, 1);
        }
    }
    return MUNIT_OK;
}

/* The conversions read the CURRENT cached offset (a change takes effect). */
static MunitResult test_uses_current_offset(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvs_tz_set_offset(3600);
    munit_assert_int(teq(mvs_local_epoch_to_utc((time_t)5000), (time_t)1400), ==, 1);
    mvs_tz_set_offset(-3600);
    munit_assert_int(teq(mvs_local_epoch_to_utc((time_t)5000), (time_t)8600), ==, 1);
    return MUNIT_OK;
}

static MunitTest convert_tests[] = {
    { "/zero_identity",       test_zero_identity,       NULL, reset_offset, MUNIT_TEST_OPTION_NONE, NULL },
    { "/west_offset",         test_west_offset,         NULL, reset_offset, MUNIT_TEST_OPTION_NONE, NULL },
    { "/east_offset",         test_east_offset,         NULL, reset_offset, MUNIT_TEST_OPTION_NONE, NULL },
    { "/inverse",             test_inverse,             NULL, reset_offset, MUNIT_TEST_OPTION_NONE, NULL },
    { "/uses_current_offset", test_uses_current_offset, NULL, reset_offset, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/offset",  offset_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/convert", convert_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvsutl_suite = {
    "/mvsutl",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
