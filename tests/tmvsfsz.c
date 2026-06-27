/*
 * tests/tmvsfsz.c - Unit tests for mvsfsz.c (PDS member file-size cache).
 *
 * Suite prefix: /mvsfsz
 * Sub-suites  : /init  /put  /get  /invalidate
 *
 * The mvsfsz module is self-contained (no external stubs needed).
 * Each test calls mvsfsz_init() to start from a clean slate.
 *
 * Build (from project root):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c tests/tmvsfsz.c \
 *      src/mvsfsz.c tests/munit.c \
 *      -o tests/runall_fsz
 */

#include "munit.h"
#include "mvsfsz.h"
#include <string.h>

/* -------------------------------------------------------------------- */
/* Helper                                                               */
/* -------------------------------------------------------------------- */

/*
 * Convenience wrapper so test bodies stay concise.
 */
static int put_e(const char *ds, const char *mb,
                 uint64_t sz,
                 uint16_t tt, uint8_t r,
                 int32_t isz, int32_t ts)
{
    return mvsfsz_put(ds, mb, sz, tt, r, isz, ts);
}

/* ==================================================================== */
/* /init                                                                 */
/* ==================================================================== */

static MunitResult test_init_count_is_zero(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvsfsz_init();

    munit_assert_int(mvsfsz_count(), ==, 0);
    return MUNIT_OK;
}

static MunitResult test_init_get_not_found(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();

    rc = mvsfsz_get("MY.TEST.PDS", "MEMBER", &e);

    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

static MunitTest init_tests[] = {
    { "/count_is_zero",  test_init_count_is_zero,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/get_not_found",  test_init_get_not_found,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /put                                                                  */
/* ==================================================================== */

static MunitResult test_put_returns_zero(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    mvsfsz_init();

    rc = put_e("MY.TEST.PDS", "NFSD", 4096, 1, 2, 10, 1700000000);

    munit_assert_int(rc, ==, 0);
    return MUNIT_OK;
}

static MunitResult test_put_increments_count(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "ALPHA", 100, 0, 1, 5, 100);
    put_e("MY.TEST.PDS", "BETA",  200, 0, 2, 6, 200);

    munit_assert_int(mvsfsz_count(), ==, 2);
    return MUNIT_OK;
}

static MunitResult test_put_upsert_no_count_change(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "NFSD", 100, 1, 1, 5, 100);
    put_e("MY.TEST.PDS", "NFSD", 200, 2, 2, 6, 200); /* same key */

    munit_assert_int(mvsfsz_count(), ==, 1);
    return MUNIT_OK;
}

static MunitResult test_put_upsert_updates_all_fields(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "NFSD", 100,  1, 1, 5, 100);
    put_e("MY.TEST.PDS", "NFSD", 9999, 7, 3, 42, 1700001234);

    mvsfsz_get("MY.TEST.PDS", "NFSD", &e);

    munit_assert_int((int)e.file_size,  ==, 9999);
    munit_assert_int((int)e.ttr_tt,     ==, 7);
    munit_assert_int((int)e.ttr_r,      ==, 3);
    munit_assert_int((int)e.ispf_size,  ==, 42);
    munit_assert_int((int)e.ispf_mtime, ==, 1700001234);
    return MUNIT_OK;
}

static MunitResult test_put_full_cache_new_key_fails(
    const MunitParameter params[], void *data)
{
    int  i;
    int  rc;
    char member[MVSFSZ_MEMBER_LEN];
    (void)params; (void)data;

    mvsfsz_init();

    /* Fill every slot with a unique member name "M000" .. "M511". */
    for (i = 0; i < MVSFSZ_CACHE_CAPACITY; i++) {
        member[0] = 'M';
        member[1] = (char)('0' + (i / 100) % 10);
        member[2] = (char)('0' + (i /  10) % 10);
        member[3] = (char)('0' +  i        % 10);
        member[4] = '\0';
        rc = put_e("MY.TEST.PDS", member, (uint64_t)i, 0, 0, (int32_t)i, 0);
        munit_assert_int(rc, ==, 0);
    }

    /* A brand-new key must be rejected. */
    rc = put_e("MY.TEST.PDS", "ZZZZ", 0, 0, 0, 0, 0);

    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

static MunitResult test_put_full_cache_existing_key_succeeds(
    const MunitParameter params[], void *data)
{
    int  i;
    int  rc;
    char member[MVSFSZ_MEMBER_LEN];
    (void)params; (void)data;

    mvsfsz_init();

    for (i = 0; i < MVSFSZ_CACHE_CAPACITY; i++) {
        member[0] = 'M';
        member[1] = (char)('0' + (i / 100) % 10);
        member[2] = (char)('0' + (i /  10) % 10);
        member[3] = (char)('0' +  i        % 10);
        member[4] = '\0';
        put_e("MY.TEST.PDS", member, (uint64_t)i, 0, 0, (int32_t)i, 0);
    }

    /* Updating M000 (an existing key) must succeed even when full. */
    rc = put_e("MY.TEST.PDS", "M000", 9999, 1, 1, 1, 1);

    munit_assert_int(rc, ==, 0);
    munit_assert_int(mvsfsz_count(), ==, MVSFSZ_CACHE_CAPACITY);
    return MUNIT_OK;
}

static MunitTest put_tests[] = {
    { "/returns_zero",                test_put_returns_zero,                NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/increments_count",            test_put_increments_count,            NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/upsert_no_count_change",      test_put_upsert_no_count_change,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/upsert_updates_all_fields",   test_put_upsert_updates_all_fields,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/full_cache_new_key_fails",    test_put_full_cache_new_key_fails,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/full_cache_existing_key_ok",  test_put_full_cache_existing_key_succeeds, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /get                                                                  */
/* ==================================================================== */

static MunitResult test_get_all_fields(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("SYS1.MACLIB", "IEFZB4D0", 81920, 42, 3, 16, 1700001234);

    rc = mvsfsz_get("SYS1.MACLIB", "IEFZB4D0", &e);

    munit_assert_int(rc, ==, 0);
    munit_assert_string_equal(e.dsname,      "SYS1.MACLIB");
    munit_assert_string_equal(e.member_name, "IEFZB4D0");
    munit_assert_int((int)e.file_size,       ==, 81920);
    munit_assert_int((int)e.ttr_tt,          ==, 42);
    munit_assert_int((int)e.ttr_r,           ==, 3);
    munit_assert_int((int)e.ispf_size,       ==, 16);
    munit_assert_int((int)e.ispf_mtime,      ==, 1700001234);
    return MUNIT_OK;
}

static MunitResult test_get_member_not_found(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "NFSD", 100, 1, 1, 5, 100);

    rc = mvsfsz_get("MY.TEST.PDS", "MISSING", &e);

    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

static MunitResult test_get_wrong_dsname(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "NFSD", 100, 1, 1, 5, 100);

    rc = mvsfsz_get("OTHER.PDS", "NFSD", &e);

    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

static MunitResult test_get_multiple_dsnames(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("DS.ONE", "ALPHA", 111, 1, 1, 1, 1);
    put_e("DS.TWO", "ALPHA", 222, 2, 2, 2, 2);

    rc = mvsfsz_get("DS.TWO", "ALPHA", &e);

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size, ==, 222);
    munit_assert_string_equal(e.dsname, "DS.TWO");
    return MUNIT_OK;
}

static MunitTest get_tests[] = {
    { "/all_fields",        test_get_all_fields,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_not_found",  test_get_member_not_found,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/wrong_dsname",      test_get_wrong_dsname,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/multiple_dsnames",  test_get_multiple_dsnames,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /invalidate                                                           */
/* ==================================================================== */

static MunitResult test_invalidate_returns_zero(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "NFSD", 100, 1, 1, 5, 100);

    rc = mvsfsz_invalidate("MY.TEST.PDS", "NFSD");

    munit_assert_int(rc, ==, 0);
    return MUNIT_OK;
}

static MunitResult test_invalidate_removes_entry(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "NFSD", 100, 1, 1, 5, 100);

    mvsfsz_invalidate("MY.TEST.PDS", "NFSD");
    rc = mvsfsz_get("MY.TEST.PDS", "NFSD", &e);

    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

static MunitResult test_invalidate_decrements_count(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "ALPHA", 100, 1, 1, 5, 100);
    put_e("MY.TEST.PDS", "BETA",  200, 2, 2, 6, 200);

    mvsfsz_invalidate("MY.TEST.PDS", "ALPHA");

    munit_assert_int(mvsfsz_count(), ==, 1);
    return MUNIT_OK;
}

static MunitResult test_invalidate_not_found(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    mvsfsz_init();

    rc = mvsfsz_invalidate("MY.TEST.PDS", "MISSING");

    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

static MunitResult test_invalidate_slot_reusable(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "NFSD", 100, 1, 1, 5, 100);
    mvsfsz_invalidate("MY.TEST.PDS", "NFSD");

    /* The freed slot must be available for a different entry. */
    rc = put_e("MY.TEST.PDS", "NEWMBR", 777, 9, 5, 20, 200);
    munit_assert_int(rc, ==, 0);

    rc = mvsfsz_get("MY.TEST.PDS", "NEWMBR", &e);
    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size, ==, 777);
    return MUNIT_OK;
}

static MunitResult test_invalidate_leaves_others_intact(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "ALPHA", 111, 1, 1, 1, 1);
    put_e("MY.TEST.PDS", "BETA",  222, 2, 2, 2, 2);
    put_e("MY.TEST.PDS", "GAMMA", 333, 3, 3, 3, 3);

    mvsfsz_invalidate("MY.TEST.PDS", "BETA");

    /* ALPHA and GAMMA must still be retrievable. */
    rc = mvsfsz_get("MY.TEST.PDS", "ALPHA", &e);
    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size, ==, 111);

    rc = mvsfsz_get("MY.TEST.PDS", "GAMMA", &e);
    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size, ==, 333);

    return MUNIT_OK;
}

static MunitTest invalidate_tests[] = {
    { "/returns_zero",          test_invalidate_returns_zero,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/removes_entry",         test_invalidate_removes_entry,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/decrements_count",      test_invalidate_decrements_count,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/not_found",             test_invalidate_not_found,             NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/slot_reusable",         test_invalidate_slot_reusable,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/leaves_others_intact",  test_invalidate_leaves_others_intact,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/init",       init_tests,       NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/put",        put_tests,        NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/get",        get_tests,        NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/invalidate", invalidate_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvsfsz_suite = {
    "/mvsfsz",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
