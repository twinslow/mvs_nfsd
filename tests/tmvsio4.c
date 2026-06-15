/*
 * tests/tmvsio4.c
 *
 * Unit tests for the mvsio.c read-cache helpers:
 *   mvs_rcache_init
 *   mvs_rcache_find_entry
 *   mvs_rcache_entry_reset
 *   mvs_rcache_get_free_entry
 *
 * These functions operate purely on the static g_mvs_rcache_entries[]
 * array (no file I/O), so they can be fully exercised on any platform.
 *
 * The cache state is shared (static) across all tests in this file, so
 * every test calls mvs_rcache_init() first to reset to a known, all-zero
 * baseline before populating any entries.
 *
 * JCC C89 compliance: all variable declarations precede executable
 * statements within each function body.  Only block comments are used.
 */

#include "munit.h"
#include "mvsio.h"
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------- */
/* Helper: grab a free cache entry and populate it as an in-use entry   */
/* with the given key fields and last_used_time.                        */
/* -------------------------------------------------------------------- */
static mvs_rcache_entry_t *fill_entry(
    const char *dsname,
    const char *member,
    int         export_idx,
    time_t      last_used_time)
{
    mvs_rcache_entry_t *entry;

    entry = mvs_rcache_get_free_entry();

    strncpy(entry->dsname, dsname, sizeof(entry->dsname) - 1);
    entry->dsname[sizeof(entry->dsname) - 1] = '\0';

    strncpy(entry->member_name, member, sizeof(entry->member_name) - 1);
    entry->member_name[sizeof(entry->member_name) - 1] = '\0';

    entry->export_idx     = export_idx;
    entry->status         = MVS_RCACHE_STATUS_USED;
    entry->last_used_time = last_used_time;

    return entry;
}

/* ==================================================================== */
/* mvs_rcache_get_free_entry / mvs_rcache_init                          */
/* ==================================================================== */

static MunitResult test_get_free_entry_zeroed(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *entry;
    (void)params; (void)data;

    mvs_rcache_init();
    entry = mvs_rcache_get_free_entry();

    munit_assert_not_null(entry);
    munit_assert_int(entry->status,         ==, MVS_RCACHE_STATUS_UNUSED);
    munit_assert_int(entry->dsname[0],      ==, '\0');
    munit_assert_int(entry->member_name[0], ==, '\0');
    munit_assert_int(entry->export_idx,     ==, 0);
    munit_assert_int((int)entry->last_offset,      ==, 0);
    munit_assert_int((int)entry->last_nread,       ==, 0);
    munit_assert_int((int)entry->has_last_getpos,  ==, 0);
    return MUNIT_OK;
}

static MunitResult test_get_free_entry_evicts_lru(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *entries[MVS_RCACHE_ENTRIES];
    mvs_rcache_entry_t *evicted;
    time_t now;
    int    i;
    int    lru_index;
    char   dsname[16];
    char   digit;

    (void)params; (void)data;

    mvs_rcache_init();
    now = time(NULL);
    lru_index = 7;

    /* Fill every slot as USED and unexpired (all within MAX_AGE). The
     * entry at lru_index is given a slightly older last_used_time so it
     * is the least-recently-used entry, but still not expired
     * (last_used_time + MAX_AGE >= now). */
    for (i = 0; i < MVS_RCACHE_ENTRIES; i++) {
        digit = (char)('0' + (i % 10));
        strcpy(dsname, "MVS.TEST.DS");
        dsname[11] = digit;
        dsname[12] = '\0';

        if (i == lru_index) {
            entries[i] = fill_entry(dsname, "MEMBER", i, now - (MVS_RCACHE_MAX_AGE_SECONDS - 1));
        } else {
            entries[i] = fill_entry(dsname, "MEMBER", i, now);
        }
    }

    /* All slots are USED and unexpired -> the LRU entry must be evicted */
    evicted = mvs_rcache_get_free_entry();

    munit_assert_ptr_equal(evicted, entries[lru_index]);
    munit_assert_int(evicted->status,    ==, MVS_RCACHE_STATUS_UNUSED);
    munit_assert_int(evicted->dsname[0], ==, '\0');
    return MUNIT_OK;
}

static MunitResult test_get_free_entry_reuses_expired(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *entries[MVS_RCACHE_ENTRIES];
    mvs_rcache_entry_t *reused;
    time_t now;
    int    i;
    int    expired_index;
    char   dsname[16];
    char   digit;

    (void)params; (void)data;

    mvs_rcache_init();
    now = time(NULL);
    expired_index = 3;

    /* Fill every slot as USED. All but one are fresh (unexpired); the
     * one at expired_index is well past MAX_AGE and should be reused
     * directly, without needing LRU eviction. */
    for (i = 0; i < MVS_RCACHE_ENTRIES; i++) {
        digit = (char)('0' + (i % 10));
        strcpy(dsname, "MVS.TEST.DS");
        dsname[11] = digit;
        dsname[12] = '\0';

        if (i == expired_index) {
            entries[i] = fill_entry(dsname, "MEMBER", i, now - (MVS_RCACHE_MAX_AGE_SECONDS + 100));
        } else {
            entries[i] = fill_entry(dsname, "MEMBER", i, now);
        }
    }

    reused = mvs_rcache_get_free_entry();

    munit_assert_ptr_equal(reused, entries[expired_index]);
    munit_assert_int(reused->status,    ==, MVS_RCACHE_STATUS_UNUSED);
    munit_assert_int(reused->dsname[0], ==, '\0');
    return MUNIT_OK;
}

static MunitTest get_free_entry_tests[] = {
    { "/zeroed",         test_get_free_entry_zeroed,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/evicts_lru",     test_get_free_entry_evicts_lru,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/reuses_expired", test_get_free_entry_reuses_expired, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* mvs_rcache_find_entry                                                 */
/* ==================================================================== */

static MunitResult test_find_entry_empty_cache(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *found;
    (void)params; (void)data;

    mvs_rcache_init();
    found = mvs_rcache_find_entry("MVS.TEST.PDS", "MEMBER1", 0);

    munit_assert_null(found);
    return MUNIT_OK;
}

static MunitResult test_find_entry_match(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *entry;
    mvs_rcache_entry_t *found;
    (void)params; (void)data;

    mvs_rcache_init();
    entry = fill_entry("MVS.TEST.PDS", "MEMBER1", 2, time(NULL));

    found = mvs_rcache_find_entry("MVS.TEST.PDS", "MEMBER1", 2);

    munit_assert_ptr_equal(found, entry);
    return MUNIT_OK;
}

static MunitResult test_find_entry_dsname_mismatch(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *found;
    (void)params; (void)data;

    mvs_rcache_init();
    fill_entry("MVS.TEST.PDS", "MEMBER1", 0, time(NULL));

    found = mvs_rcache_find_entry("MVS.OTHER.PDS", "MEMBER1", 0);

    munit_assert_null(found);
    return MUNIT_OK;
}

static MunitResult test_find_entry_member_mismatch(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *found;
    (void)params; (void)data;

    mvs_rcache_init();
    fill_entry("MVS.TEST.PDS", "MEMBER1", 0, time(NULL));

    found = mvs_rcache_find_entry("MVS.TEST.PDS", "MEMBER2", 0);

    munit_assert_null(found);
    return MUNIT_OK;
}

static MunitResult test_find_entry_export_idx_mismatch(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *found;
    (void)params; (void)data;

    mvs_rcache_init();
    fill_entry("MVS.TEST.PDS", "MEMBER1", 0, time(NULL));

    found = mvs_rcache_find_entry("MVS.TEST.PDS", "MEMBER1", 1);

    munit_assert_null(found);
    return MUNIT_OK;
}

static MunitResult test_find_entry_expired(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *found;
    time_t now;
    (void)params; (void)data;

    mvs_rcache_init();
    now = time(NULL);

    /* last_used_time + MAX_AGE < now -> entry has expired and must not
     * be returned as a cache hit. */
    fill_entry("MVS.TEST.PDS", "MEMBER1", 0, now - (MVS_RCACHE_MAX_AGE_SECONDS + 100));

    found = mvs_rcache_find_entry("MVS.TEST.PDS", "MEMBER1", 0);

    munit_assert_null(found);
    return MUNIT_OK;
}

static MunitTest find_entry_tests[] = {
    { "/empty_cache",       test_find_entry_empty_cache,       NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/match",              test_find_entry_match,             NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/dsname_mismatch",    test_find_entry_dsname_mismatch,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_mismatch",    test_find_entry_member_mismatch,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/export_idx_mismatch",test_find_entry_export_idx_mismatch, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/expired",            test_find_entry_expired,           NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* mvs_rcache_entry_reset                                                */
/* ==================================================================== */

static MunitResult test_entry_reset(
    const MunitParameter params[], void *data)
{
    mvs_rcache_entry_t *entry;
    (void)params; (void)data;

    mvs_rcache_init();
    entry = fill_entry("MVS.TEST.PDS", "MEMBER1", 1, time(NULL));

    entry->last_offset     = 100;
    entry->last_nread      = 50;
    entry->has_last_getpos = 1;

    mvs_rcache_entry_reset(entry);

    munit_assert_int(entry->status,         ==, MVS_RCACHE_STATUS_UNUSED);
    munit_assert_int(entry->dsname[0],      ==, '\0');
    munit_assert_int(entry->member_name[0], ==, '\0');
    munit_assert_int(entry->export_idx,     ==, 0);
    munit_assert_int((int)entry->last_offset,     ==, 0);
    munit_assert_int((int)entry->last_nread,      ==, 0);
    munit_assert_int((int)entry->has_last_getpos, ==, 0);
    return MUNIT_OK;
}

static MunitTest entry_reset_tests[] = {
    { "/clears_all_fields", test_entry_reset, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                   */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/get_free_entry", get_free_entry_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/find_entry",     find_entry_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/entry_reset",    entry_reset_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvsio4_suite = {
    "/mvsio/rcache",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
