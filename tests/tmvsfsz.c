/*
 * tests/tmvsfsz.c - Unit tests for mvsfsz.c (PDS member file-size cache).
 *
 * Suite prefix: /mvsfsz
 * Sub-suites  : /init  /put  /get  /invalidate  /lru  /get_member_size  /load
 *
 * The mvsfsz module is self-contained (no external stubs needed).
 * Each test calls mvsfsz_init() to start from a clean slate.
 *
 * Tests that exercise mvsfsz_get_member_size() on a cache MISS attempt to
 * open a PDS member via fopen("//DSN:...","rt").  Tests that use a
 * deliberately non-existent dataset verify that a failed open returns -1
 * without crashing.  Tests that use a real MVS dataset are guarded with
 * MUNIT_SKIP when the dataset is not accessible.
 *
 * The /load sub-suite writes a temporary file.  On MVS, if the temp path
 * is not writable the individual tests return MUNIT_SKIP automatically.
 *
 * Build (from project root, MVS JCL or Linux):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c tests/tmvsfsz.c \
 *      src/mvsfsz.c src/mvspdir.c src/mvsio.c tests/munit.c \
 *      -o tests/runall_fsz
 */

#include "munit.h"
#include "mvsfsz.h"
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------- */
/* Helpers                                                               */
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

/*
 * Build a pds_member_entry_t with the four validity fields set.
 * All other fields are left zeroed.
 */
static pds_member_entry_t make_entry(int32_t   size,
                                     int32_t   chgdate,
                                     uint16_t  first_block_tt,
                                     uint8_t   first_block_rec)
{
    pds_member_entry_t e;
    memset(&e, 0, sizeof(e));
    e.size           = size;
    e.chgdate        = chgdate;
    e.first_block_tt = first_block_tt;
    e.first_block_rec= first_block_rec;
    return e;
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

static MunitTest put_tests[] = {
    { "/returns_zero",              test_put_returns_zero,              NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/increments_count",          test_put_increments_count,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/upsert_no_count_change",    test_put_upsert_no_count_change,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/upsert_updates_all_fields", test_put_upsert_updates_all_fields, NULL, NULL,
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

static MunitTest get_tests[] = {
    { "/all_fields",        test_get_all_fields,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_not_found",  test_get_member_not_found,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/wrong_dsname",      test_get_wrong_dsname,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /invalidate                                                           */
/* ==================================================================== */

static MunitResult test_invalidate_removes_entry(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e("MY.TEST.PDS", "NFSD", 100, 1, 1, 5, 100);

    munit_assert_int(mvsfsz_invalidate("MY.TEST.PDS", "NFSD"), ==, 0);
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
    (void)params; (void)data;

    mvsfsz_init();

    munit_assert_int(mvsfsz_invalidate("MY.TEST.PDS", "MISSING"), ==, -1);
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

    rc = put_e("MY.TEST.PDS", "NEWMBR", 777, 9, 5, 20, 200);
    munit_assert_int(rc, ==, 0);

    rc = mvsfsz_get("MY.TEST.PDS", "NEWMBR", &e);
    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size, ==, 777);
    return MUNIT_OK;
}

static MunitTest invalidate_tests[] = {
    { "/removes_entry",     test_invalidate_removes_entry,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/decrements_count",  test_invalidate_decrements_count,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/not_found",         test_invalidate_not_found,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/slot_reusable",     test_invalidate_slot_reusable,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /lru                                                                  */
/*                                                                       */
/* Fill the cache to capacity and verify that a subsequent put evicts    */
/* rather than failing.                                                  */
/* ==================================================================== */

static MunitResult test_lru_evicts_when_full(
    const MunitParameter params[], void *data)
{
    int  i;
    int  rc;
    char member[MVSFSZ_MEMBER_LEN];
    (void)params; (void)data;

    mvsfsz_init();

    /* Fill every slot.  Member names: "M0000" .. "M1999". */
    for (i = 0; i < MVSFSZ_CACHE_CAPACITY; i++) {
        member[0] = 'M';
        member[1] = (char)('0' + (i / 1000) % 10);
        member[2] = (char)('0' + (i /  100) % 10);
        member[3] = (char)('0' + (i /   10) % 10);
        member[4] = (char)('0' +  i         % 10);
        member[5] = '\0';
        rc = put_e("MY.TEST.PDS", member, (uint64_t)i, 0, 0, (int32_t)i, 0);
        munit_assert_int(rc, ==, 0);
    }

    munit_assert_int(mvsfsz_count(), ==, MVSFSZ_CACHE_CAPACITY);

    /* Adding a brand-new key must succeed via LRU eviction. */
    rc = put_e("MY.TEST.PDS", "ZNEW", 9999, 0, 0, 0, 0);

    munit_assert_int(rc, ==, 0);
    munit_assert_int(mvsfsz_count(), ==, MVSFSZ_CACHE_CAPACITY);
    return MUNIT_OK;
}

static MunitResult test_lru_new_entry_retrievable(
    const MunitParameter params[], void *data)
{
    int            i;
    char           member[MVSFSZ_MEMBER_LEN];
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();

    for (i = 0; i < MVSFSZ_CACHE_CAPACITY; i++) {
        member[0] = 'M';
        member[1] = (char)('0' + (i / 1000) % 10);
        member[2] = (char)('0' + (i /  100) % 10);
        member[3] = (char)('0' + (i /   10) % 10);
        member[4] = (char)('0' +  i         % 10);
        member[5] = '\0';
        put_e("MY.TEST.PDS", member, (uint64_t)i, 0, 0, (int32_t)i, 0);
    }

    put_e("MY.TEST.PDS", "ZNEW", 8888, 0, 0, 0, 0);

    rc = mvsfsz_get("MY.TEST.PDS", "ZNEW", &e);

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size, ==, 8888);
    return MUNIT_OK;
}

static MunitTest lru_tests[] = {
    { "/evicts_when_full",     test_lru_evicts_when_full,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/new_entry_retrievable",test_lru_new_entry_retrievable,NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /get_member_size                                                      */
/*                                                                       */
/* Tests for mvsfsz_get_member_size().                                   */
/*                                                                       */
/* Cache-hit tests exercise only the cache logic and do not open any    */
/* dataset.  Cache-miss / stale tests use a deliberately non-existent   */
/* dataset ("ZZZZZ.NOTEXIST") so that fopen() fails with -1 on both     */
/* MVS and Linux, verifying the error path without needing real DASD.   */
/* ==================================================================== */

/* Sentinel dataset name guaranteed not to exist on any system. */
#define NO_SUCH_DS "ZZZZZ.NOTEXIST"
#define NO_SUCH_MB "NOEXIST"

static MunitResult test_gsm_cache_hit_valid(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t m;
    uint64_t           sz;
    int                rc;
    (void)params; (void)data;

    mvsfsz_init();

    /* Put a cache entry: ttr_tt=5, ttr_r=3, ispf_size=100, ispf_mtime=12345 */
    put_e("MY.TEST.PDS", "ALPHA", 4096, 5, 3, 100, 12345);

    /* Build a directory entry whose validity fields MATCH the cache. */
    m = make_entry(100, 12345, 5, 3);

    rc = mvsfsz_get_member_size("MY.TEST.PDS", "ALPHA", &m, &sz);

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)sz, ==, 4096);
    return MUNIT_OK;
}

static MunitResult test_gsm_cache_hit_valid_no_file_io(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t m;
    uint64_t           sz;
    int                rc;
    (void)params; (void)data;

    mvsfsz_init();

    /* Use the non-existent dataset as the dsname -- if the cache is valid
     * the function must return 0 WITHOUT attempting to open the file. */
    put_e(NO_SUCH_DS, "ALPHA", 1234, 7, 2, 50, 99999);
    m = make_entry(50, 99999, 7, 2);

    rc = mvsfsz_get_member_size(NO_SUCH_DS, "ALPHA", &m, &sz);

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)sz, ==, 1234);
    return MUNIT_OK;
}

static MunitResult test_gsm_stale_ispf_size(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t m;
    uint64_t           sz;
    int                rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e(NO_SUCH_DS, NO_SUCH_MB, 4096, 5, 3, 100, 12345);

    /* ispf_size mismatch: 100 in cache vs 200 in directory entry. */
    m = make_entry(200, 12345, 5, 3);
    rc = mvsfsz_get_member_size(NO_SUCH_DS, NO_SUCH_MB, &m, &sz);

    /* Stale entry evicted; fopen fails on non-existent dataset. */
    munit_assert_int(rc, ==, -1);
    munit_assert_int(mvsfsz_count(), ==, 0);
    return MUNIT_OK;
}

static MunitResult test_gsm_stale_ispf_mtime(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t m;
    uint64_t           sz;
    int                rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e(NO_SUCH_DS, NO_SUCH_MB, 4096, 5, 3, 100, 12345);

    /* chgdate mismatch: 12345 in cache vs 99999 in directory entry. */
    m = make_entry(100, 99999, 5, 3);
    rc = mvsfsz_get_member_size(NO_SUCH_DS, NO_SUCH_MB, &m, &sz);

    munit_assert_int(rc, ==, -1);
    munit_assert_int(mvsfsz_count(), ==, 0);
    return MUNIT_OK;
}

static MunitResult test_gsm_stale_ttr_tt(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t m;
    uint64_t           sz;
    int                rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e(NO_SUCH_DS, NO_SUCH_MB, 4096, 5, 3, 100, 12345);

    /* first_block_tt mismatch: 5 in cache vs 9 in directory entry. */
    m = make_entry(100, 12345, 9, 3);
    rc = mvsfsz_get_member_size(NO_SUCH_DS, NO_SUCH_MB, &m, &sz);

    munit_assert_int(rc, ==, -1);
    munit_assert_int(mvsfsz_count(), ==, 0);
    return MUNIT_OK;
}

static MunitResult test_gsm_stale_ttr_r(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t m;
    uint64_t           sz;
    int                rc;
    (void)params; (void)data;

    mvsfsz_init();
    put_e(NO_SUCH_DS, NO_SUCH_MB, 4096, 5, 3, 100, 12345);

    /* first_block_rec mismatch: 3 in cache vs 7 in directory entry. */
    m = make_entry(100, 12345, 5, 7);
    rc = mvsfsz_get_member_size(NO_SUCH_DS, NO_SUCH_MB, &m, &sz);

    munit_assert_int(rc, ==, -1);
    munit_assert_int(mvsfsz_count(), ==, 0);
    return MUNIT_OK;
}

static MunitResult test_gsm_cache_miss_no_file(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t m;
    uint64_t           sz;
    int                rc;
    (void)params; (void)data;

    mvsfsz_init();

    /* No cache entry at all -- fopen of non-existent dataset must fail. */
    m = make_entry(100, 12345, 5, 3);
    rc = mvsfsz_get_member_size(NO_SUCH_DS, NO_SUCH_MB, &m, &sz);

    munit_assert_int(rc, ==, -1);
    munit_assert_int(mvsfsz_count(), ==, 0);
    return MUNIT_OK;
}

static MunitTest get_member_size_tests[] = {
    { "/cache_hit_valid",         test_gsm_cache_hit_valid,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/cache_hit_valid_no_io",   test_gsm_cache_hit_valid_no_file_io, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/stale_ispf_size",         test_gsm_stale_ispf_size,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/stale_ispf_mtime",        test_gsm_stale_ispf_mtime,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/stale_ttr_tt",            test_gsm_stale_ttr_tt,            NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/stale_ttr_r",             test_gsm_stale_ttr_r,             NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/cache_miss_no_file",      test_gsm_cache_miss_no_file,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /load                                                                 */
/*                                                                       */
/* Uses a temporary file.  write_load_file() returns 0 if the temp      */
/* path is not writable (e.g. no /tmp on MVS); each test returns        */
/* MUNIT_SKIP in that case.                                              */
/* ==================================================================== */

#define LOAD_TMP_FILE "/tmp/tmvsfsz_load.txt"

static int write_load_file(const char *contents)
{
    FILE *fp;
    fp = fopen(LOAD_TMP_FILE, "w");
    if (fp == NULL)
        return 0;
    fputs(contents, fp);
    fclose(fp);
    return 1;
}

static MunitResult test_load_basic(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            loaded;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    if (!write_load_file(
            "# comment line\n"
            "MY.TEST.PDS ALPHA 1234\n"
            "MY.TEST.PDS BETA  5678\n"))
        return MUNIT_SKIP;

    loaded = mvsfsz_load(LOAD_TMP_FILE);

    munit_assert_int(loaded, ==, 2);
    munit_assert_int(mvsfsz_count(), ==, 2);

    rc = mvsfsz_get("MY.TEST.PDS", "ALPHA", &e);
    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size, ==, 1234);

    rc = mvsfsz_get("MY.TEST.PDS", "BETA", &e);
    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size, ==, 5678);

    remove(LOAD_TMP_FILE);
    return MUNIT_OK;
}

static MunitResult test_load_comments_and_blanks_skipped(
    const MunitParameter params[], void *data)
{
    int loaded;
    (void)params; (void)data;

    mvsfsz_init();
    if (!write_load_file(
            "# first comment\n"
            "# second comment\n"
            "\n"
            "MY.TEST.PDS NFSD 999\n"
            "\n"
            "# trailing comment\n"))
        return MUNIT_SKIP;

    loaded = mvsfsz_load(LOAD_TMP_FILE);

    munit_assert_int(loaded, ==, 1);
    munit_assert_int(mvsfsz_count(), ==, 1);

    remove(LOAD_TMP_FILE);
    return MUNIT_OK;
}

static MunitResult test_load_malformed_lines_skipped(
    const MunitParameter params[], void *data)
{
    int loaded;
    (void)params; (void)data;

    mvsfsz_init();
    if (!write_load_file(
            "MY.TEST.PDS NFSD 100\n"
            "ONLY_ONE_FIELD\n"
            "DS.NAME MEMBER\n"
            "MY.TEST.PDS GOOD 200\n"))
        return MUNIT_SKIP;

    loaded = mvsfsz_load(LOAD_TMP_FILE);

    munit_assert_int(loaded, ==, 2);

    remove(LOAD_TMP_FILE);
    return MUNIT_OK;
}

static MunitResult test_load_validity_fields_zeroed(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();
    if (!write_load_file("SYS1.MACLIB IEFZB4D0 81920\n"))
        return MUNIT_SKIP;

    mvsfsz_load(LOAD_TMP_FILE);

    rc = mvsfsz_get("SYS1.MACLIB", "IEFZB4D0", &e);
    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size,  ==, 81920);
    munit_assert_int((int)e.ttr_tt,     ==, 0);
    munit_assert_int((int)e.ttr_r,      ==, 0);
    munit_assert_int((int)e.ispf_size,  ==, 0);
    munit_assert_int((int)e.ispf_mtime, ==, 0);

    remove(LOAD_TMP_FILE);
    return MUNIT_OK;
}

static MunitResult test_load_file_not_found(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    mvsfsz_init();

    rc = mvsfsz_load("/tmp/no_such_file_mvsfsz.txt");

    munit_assert_int(rc, ==, -1);
    munit_assert_int(mvsfsz_count(), ==, 0);
    return MUNIT_OK;
}

static MunitResult test_load_sample_file(
    const MunitParameter params[], void *data)
{
    mvsfsz_entry_t e;
    int            loaded;
    int            rc;
    (void)params; (void)data;

    mvsfsz_init();

    /* Load the sample file shipped with the project.  On MVS this would
     * be submitted as a sequential dataset; the path below works when the
     * test runner's CWD is the project root on Linux. */
    loaded = mvsfsz_load("jcl/tstfsiz.jcl");
    if (loaded < 0)
        return MUNIT_SKIP;

    munit_assert_int(loaded, >, 0);

    rc = mvsfsz_get("TEMP.TESTPROJ.JCLLIB", "SHUTDOWN", &e);
    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)e.file_size, ==, 727);

    return MUNIT_OK;
}

static MunitTest load_tests[] = {
    { "/basic",                   test_load_basic,                   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/comments_and_blanks",     test_load_comments_and_blanks_skipped, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/malformed_lines_skipped", test_load_malformed_lines_skipped, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/validity_fields_zeroed",  test_load_validity_fields_zeroed,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/file_not_found",          test_load_file_not_found,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/sample_file",             test_load_sample_file,             NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/init",            init_tests,            NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/put",             put_tests,             NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/get",             get_tests,             NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/invalidate",      invalidate_tests,      NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/lru",             lru_tests,             NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/get_member_size", get_member_size_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/load",            load_tests,            NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvsfsz_suite = {
    "/mvsfsz",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
