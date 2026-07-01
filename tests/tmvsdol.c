/*
 * tests/tmvsdol.c
 *
 * Unit tests for the mvsdol.c directory open-list pool:
 *   dir_openlist_init
 *   dir_openlist_find_free
 *   dir_openlist_free
 *   dir_openlist_search_members  (full operator set: EQ, GT, LT, GE, LE)
 *
 * Tests for the pool functions call dir_openlist_init() at the start of
 * each test to ensure a clean, all-free baseline; the static pool is
 * shared across calls within the same process.
 *
 * Tests for dir_openlist_search_members operate on a locally declared
 * vfs_dir_t populated directly, so they do not touch the pool at all.
 *
 * Member list used by all search tests (5 entries, sorted):
 *   [0] ALPHA   [1] DELTA   [2] HOTEL   [3] MIKE   [4] ZULU
 *
 * JCC C89 compliance: all variable declarations precede executable
 * statements within each function body.  Only block comments are used.
 */

#include "munit.h"
#include "nfsd.h"     /* MAX_OPEN_DIRS, vfs_dir_t forward decl */
#include "mvsdol.h"   /* full struct vfs_dir, SEARCH_OP_*, prototypes */
#include <string.h>
#include <errno.h>
#include <time.h>

/* -------------------------------------------------------------------- */
/* Helpers                                                               */
/* -------------------------------------------------------------------- */

/* Grab a free pool slot and mark it USED so subsequent find_free
 * calls skip it.  Returns the slot pointer.                            */
static vfs_dir_t *pool_alloc(void)
{
    vfs_dir_t *d;

    d = dir_openlist_find_free();
    if (d != NULL)
        d->status = MVSVFS_DIR_OPENLIST_USED;
    return d;
}

/* Grab a free pool slot, mark it USED, and copy a dataset name into
 * pds_dsname_ebcdic.  Returns the slot pointer.                        */
static vfs_dir_t *pool_alloc_named(const char *dsname)
{
    vfs_dir_t *d;

    d = pool_alloc();
    if (d != NULL) {
        strncpy(d->pds_dsname_ebcdic, dsname, 44);
        d->pds_dsname_ebcdic[44] = '\0';
    }
    return d;
}

/* Backdate last_used_time so that the entry appears to have timed out. */
static void expire_entry(vfs_dir_t *d)
{
    d->last_used_time = time(NULL) - MVSVFS_DIR_OPENLIST_TIMEOUT_SECS - 1;
}

/* Populate one pds_member_entry_t with the given name. */
static void set_member(pds_member_entry_t *m, const char *name)
{
    memset(m, 0, sizeof(pds_member_entry_t));
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->name[sizeof(m->name) - 1] = '\0';
}

/* Build a local vfs_dir_t with five sorted members for search tests.
 * The member_list points at a static buffer (the search tests are
 * read-only, so they never expand or free the list). */
static pds_member_entry_t five_members[5];

static void make_five_member_dir(vfs_dir_t *d)
{
    memset(d, 0, sizeof(vfs_dir_t));
    set_member(&five_members[0], "ALPHA");
    set_member(&five_members[1], "DELTA");
    set_member(&five_members[2], "HOTEL");
    set_member(&five_members[3], "MIKE");
    set_member(&five_members[4], "ZULU");
    d->member_list.list           = five_members;
    d->member_list.list_size      = 5;
    d->member_list.number_in_list = 5;
}

/* ==================================================================== */
/* dir_openlist_init                                                     */
/* ==================================================================== */

static MunitResult test_init_zeros_pool(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *d;
    int        i;
    (void)params; (void)data;

    /* Dirty the pool first. */
    dir_openlist_init();
    d = dir_openlist_find_free();
    munit_assert_not_null(d);
    d->status = MVSVFS_DIR_OPENLIST_USED;

    /* Re-initialise: every slot must be obtainable as FREE. */
    dir_openlist_init();
    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        d = dir_openlist_find_free();
        munit_assert_not_null(d);
        munit_assert_int(d->status, ==, MVSVFS_DIR_OPENLIST_FREE);
        d->status = MVSVFS_DIR_OPENLIST_USED;  /* consume slot */
    }
    return MUNIT_OK;
}

static MunitTest init_tests[] = {
    { "/zeros_pool", test_init_zeros_pool, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* dir_openlist_find_free                                                */
/* ==================================================================== */

static MunitResult test_find_free_returns_entry(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *d;
    (void)params; (void)data;

    dir_openlist_init();
    d = dir_openlist_find_free();

    munit_assert_not_null(d);
    return MUNIT_OK;
}

static MunitResult test_find_free_entry_is_zeroed(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *d;
    (void)params; (void)data;

    dir_openlist_init();
    d = dir_openlist_find_free();

    munit_assert_not_null(d);
    munit_assert_int(d->status,                     ==, MVSVFS_DIR_OPENLIST_FREE);
    munit_assert_int(d->export_idx,                 ==, 0);
    munit_assert_int(d->member_list.number_in_list, ==, 0);
    munit_assert_int(d->end_of_dir_read,            ==, 0);
    munit_assert_int(d->pds_dsname_ebcdic[0],       ==, '\0');
    return MUNIT_OK;
}

static MunitResult test_find_free_evicts_lru_when_full(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *slots[MAX_OPEN_DIRS];
    vfs_dir_t *evicted;
    int        i;
    (void)params; (void)data;

    dir_openlist_init();

    /* Fill every slot. */
    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        slots[i] = pool_alloc();
        munit_assert_not_null(slots[i]);
    }

    /* Make slot 0 the least-recently-used, but still within the idle
       timeout so it is chosen by LRU eviction rather than reclaimed as
       an expired entry. */
    slots[0]->last_used_time = time(NULL) - 5;

    /* The pool is full of fresh USED slots: find_free does not fail -- it
       evicts and returns the least-recently-used slot (slot 0). */
    evicted = dir_openlist_find_free();

    munit_assert_not_null(evicted);
    munit_assert_ptr_equal(evicted, slots[0]);
    return MUNIT_OK;
}

static MunitResult test_find_free_distinct_slots(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *slots[MAX_OPEN_DIRS];
    int        i, j;
    (void)params; (void)data;

    dir_openlist_init();

    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        slots[i] = pool_alloc();
        munit_assert_not_null(slots[i]);
    }

    /* Every returned pointer must be unique. */
    for (i = 0; i < MAX_OPEN_DIRS - 1; i++) {
        for (j = i + 1; j < MAX_OPEN_DIRS; j++) {
            munit_assert(slots[i] != slots[j]);
        }
    }
    return MUNIT_OK;
}

static MunitResult test_find_free_reclaims_timed_out(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *slots[MAX_OPEN_DIRS];
    vfs_dir_t *reclaimed;
    int        i;
    (void)params; (void)data;

    dir_openlist_init();

    /* Fill every pool slot. */
    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        slots[i] = pool_alloc();
        munit_assert_not_null(slots[i]);
    }

    /* Expire the first slot. */
    expire_entry(slots[0]);

    /* find_free must reclaim the expired slot in preference to evicting a
       live one, so the reclaimed entry is slot 0. */
    reclaimed = dir_openlist_find_free();
    munit_assert_not_null(reclaimed);
    munit_assert_ptr_equal(reclaimed, slots[0]);
    return MUNIT_OK;
}

static MunitTest find_free_tests[] = {
    { "/returns_entry",        test_find_free_returns_entry,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/entry_is_zeroed",      test_find_free_entry_is_zeroed,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/evicts_lru_when_full", test_find_free_evicts_lru_when_full, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/distinct_slots",       test_find_free_distinct_slots,       NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/reclaims_timed_out",   test_find_free_reclaims_timed_out,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* dir_openlist_free                                                     */
/* ==================================================================== */

static MunitResult test_free_clears_entry(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *d;
    (void)params; (void)data;

    dir_openlist_init();
    d = dir_openlist_find_free();
    munit_assert_not_null(d);

    d->status                     = MVSVFS_DIR_OPENLIST_USED;
    d->export_idx                 = 3;
    d->member_list.number_in_list = 7;
    d->end_of_dir_read            = 1;
    strncpy(d->pds_dsname_ebcdic, "MY.TEST.PDS", 44);

    dir_openlist_free(d);

    munit_assert_int(d->status,                     ==, MVSVFS_DIR_OPENLIST_FREE);
    munit_assert_int(d->export_idx,                 ==, 0);
    munit_assert_int(d->member_list.number_in_list, ==, 0);
    munit_assert_int(d->end_of_dir_read,            ==, 0);
    munit_assert_int(d->pds_dsname_ebcdic[0],       ==, '\0');
    return MUNIT_OK;
}

static MunitResult test_free_makes_slot_reusable(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *first;
    vfs_dir_t *reused;
    (void)params; (void)data;

    dir_openlist_init();

    first = pool_alloc();
    munit_assert_not_null(first);

    /* Free the slot; the next find_free must return it again. */
    dir_openlist_free(first);
    reused = dir_openlist_find_free();

    munit_assert_ptr_equal(reused, first);
    return MUNIT_OK;
}

static MunitTest free_tests[] = {
    { "/clears_entry",        test_free_clears_entry,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/makes_slot_reusable", test_free_makes_slot_reusable, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* dir_openlist_search_members -- empty list                             */
/* ==================================================================== */

static MunitResult test_search_empty_list(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    memset(&dir, 0, sizeof(dir));  /* member_list.number_in_list = 0 */
    info = NULL;

    rc = dir_openlist_search_members(&dir, "ALPHA", SEARCH_OP_EQ, &info);

    munit_assert_int(rc, ==, -1);
    munit_assert_null(info);
    return MUNIT_OK;
}

static MunitTest search_empty_tests[] = {
    { "/empty_list", test_search_empty_list, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* dir_openlist_search_members -- SEARCH_OP_EQ                          */
/* ==================================================================== */

static MunitResult test_search_eq_found(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    rc = dir_openlist_search_members(&dir, "HOTEL", SEARCH_OP_EQ, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "HOTEL");
    return MUNIT_OK;
}

static MunitResult test_search_eq_first(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    rc = dir_openlist_search_members(&dir, "ALPHA", SEARCH_OP_EQ, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "ALPHA");
    return MUNIT_OK;
}

static MunitResult test_search_eq_last(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    rc = dir_openlist_search_members(&dir, "ZULU", SEARCH_OP_EQ, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "ZULU");
    return MUNIT_OK;
}

static MunitResult test_search_eq_not_found(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    rc = dir_openlist_search_members(&dir, "GAMMA", SEARCH_OP_EQ, &info);

    munit_assert_int(rc, ==, -1);
    munit_assert_null(info);
    return MUNIT_OK;
}

static MunitTest search_eq_tests[] = {
    { "/found",     test_search_eq_found,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/first",     test_search_eq_first,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/last",      test_search_eq_last,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/not_found", test_search_eq_not_found, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* dir_openlist_search_members -- SEARCH_OP_GT                          */
/* ==================================================================== */

static MunitResult test_search_gt_between(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* GAMMA sorts between DELTA and HOTEL; GT must return HOTEL. */
    rc = dir_openlist_search_members(&dir, "GAMMA", SEARCH_OP_GT, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "HOTEL");
    return MUNIT_OK;
}

static MunitResult test_search_gt_on_exact(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* GT on an exact match must skip it and return the next entry. */
    rc = dir_openlist_search_members(&dir, "DELTA", SEARCH_OP_GT, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "HOTEL");
    return MUNIT_OK;
}

static MunitResult test_search_gt_before_all(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* Key before all entries: GT must return the first entry. */
    rc = dir_openlist_search_members(&dir, "AAAA", SEARCH_OP_GT, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "ALPHA");
    return MUNIT_OK;
}

static MunitResult test_search_gt_after_all(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* GT on the last entry: no greater entry exists. */
    rc = dir_openlist_search_members(&dir, "ZULU", SEARCH_OP_GT, &info);

    munit_assert_int(rc, ==, -1);
    munit_assert_null(info);
    return MUNIT_OK;
}

static MunitTest search_gt_tests[] = {
    { "/between",    test_search_gt_between,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/on_exact",   test_search_gt_on_exact,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/before_all", test_search_gt_before_all, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/after_all",  test_search_gt_after_all,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* dir_openlist_search_members -- SEARCH_OP_LT                          */
/* ==================================================================== */

static MunitResult test_search_lt_between(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* GAMMA sorts between DELTA and HOTEL; LT must return DELTA. */
    rc = dir_openlist_search_members(&dir, "GAMMA", SEARCH_OP_LT, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "DELTA");
    return MUNIT_OK;
}

static MunitResult test_search_lt_on_exact(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* LT on an exact match must skip it and return the previous entry. */
    rc = dir_openlist_search_members(&dir, "HOTEL", SEARCH_OP_LT, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "DELTA");
    return MUNIT_OK;
}

static MunitResult test_search_lt_before_all(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* Key before all entries: no lesser entry exists. */
    rc = dir_openlist_search_members(&dir, "AAAA", SEARCH_OP_LT, &info);

    munit_assert_int(rc, ==, -1);
    munit_assert_null(info);
    return MUNIT_OK;
}

static MunitTest search_lt_tests[] = {
    { "/between",    test_search_lt_between,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/on_exact",   test_search_lt_on_exact,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/before_all", test_search_lt_before_all, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* dir_openlist_search_members -- SEARCH_OP_GE                          */
/* ==================================================================== */

static MunitResult test_search_ge_exact(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* GE on an exact match must return that entry. */
    rc = dir_openlist_search_members(&dir, "DELTA", SEARCH_OP_GE, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "DELTA");
    return MUNIT_OK;
}

static MunitResult test_search_ge_between(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* GAMMA falls between DELTA and HOTEL; GE must return HOTEL. */
    rc = dir_openlist_search_members(&dir, "GAMMA", SEARCH_OP_GE, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "HOTEL");
    return MUNIT_OK;
}

static MunitResult test_search_ge_after_all(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* Key strictly past the last entry: no GE entry exists. */
    rc = dir_openlist_search_members(&dir, "ZZZZZ", SEARCH_OP_GE, &info);

    munit_assert_int(rc, ==, -1);
    munit_assert_null(info);
    return MUNIT_OK;
}

static MunitTest search_ge_tests[] = {
    { "/exact",     test_search_ge_exact,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/between",   test_search_ge_between,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/after_all", test_search_ge_after_all, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* dir_openlist_search_members -- SEARCH_OP_LE                          */
/* ==================================================================== */

static MunitResult test_search_le_exact(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* LE on an exact match must return that entry. */
    rc = dir_openlist_search_members(&dir, "MIKE", SEARCH_OP_LE, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "MIKE");
    return MUNIT_OK;
}

static MunitResult test_search_le_between(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* GAMMA falls between DELTA and HOTEL; LE must return DELTA. */
    rc = dir_openlist_search_members(&dir, "GAMMA", SEARCH_OP_LE, &info);

    munit_assert_int(rc, ==, 0);
    munit_assert_not_null(info);
    munit_assert_string_equal(info->name, "DELTA");
    return MUNIT_OK;
}

static MunitResult test_search_le_before_all(
    const MunitParameter params[], void *data)
{
    vfs_dir_t           dir;
    pds_member_entry_t *info;
    int                 rc;
    (void)params; (void)data;

    make_five_member_dir(&dir);

    /* Key before all entries: no lesser-or-equal entry exists. */
    rc = dir_openlist_search_members(&dir, "AAAA", SEARCH_OP_LE, &info);

    munit_assert_int(rc, ==, -1);
    munit_assert_null(info);
    return MUNIT_OK;
}

static MunitTest search_le_tests[] = {
    { "/exact",      test_search_le_exact,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/between",    test_search_le_between,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/before_all", test_search_le_before_all, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* dir_openlist_find_by_dsname                                          */
/* ==================================================================== */

/* Found: USED, non-expired entry with matching name is returned. */
static MunitResult test_find_by_dsname_found(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *d;
    vfs_dir_t *found;
    (void)params; (void)data;

    dir_openlist_init();
    d = pool_alloc_named("MY.TEST.PDS");
    munit_assert_not_null(d);

    found = dir_openlist_find_by_dsname("MY.TEST.PDS");

    munit_assert_ptr_equal(found, d);
    return MUNIT_OK;
}

/* Not found: no entry with the requested name. */
static MunitResult test_find_by_dsname_not_found(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *found;
    (void)params; (void)data;

    dir_openlist_init();
    pool_alloc_named("MY.TEST.PDS");

    found = dir_openlist_find_by_dsname("OTHER.PDS");

    munit_assert_null(found);
    return MUNIT_OK;
}

/* Timed out: expired entry is not returned. */
static MunitResult test_find_by_dsname_timed_out(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *d;
    vfs_dir_t *found;
    (void)params; (void)data;

    dir_openlist_init();
    d = pool_alloc_named("MY.TEST.PDS");
    munit_assert_not_null(d);
    expire_entry(d);

    found = dir_openlist_find_by_dsname("MY.TEST.PDS");

    munit_assert_null(found);
    return MUNIT_OK;
}

/* Ignores FREE: a FREE entry whose dsname happens to match is skipped. */
static MunitResult test_find_by_dsname_ignores_free(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *d;
    vfs_dir_t *found;
    (void)params; (void)data;

    dir_openlist_init();
    /* Allocate, write name, then release back to FREE. */
    d = pool_alloc_named("MY.TEST.PDS");
    munit_assert_not_null(d);
    dir_openlist_free(d);

    found = dir_openlist_find_by_dsname("MY.TEST.PDS");

    munit_assert_null(found);
    return MUNIT_OK;
}

/* Multiple entries: correct entry is returned when pool has several. */
static MunitResult test_find_by_dsname_multiple(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *d0;
    vfs_dir_t *d1;
    vfs_dir_t *d2;
    vfs_dir_t *found;
    (void)params; (void)data;

    dir_openlist_init();
    d0 = pool_alloc_named("FIRST.PDS");
    d1 = pool_alloc_named("TARGET.PDS");
    d2 = pool_alloc_named("THIRD.PDS");
    munit_assert_not_null(d0);
    munit_assert_not_null(d1);
    munit_assert_not_null(d2);

    found = dir_openlist_find_by_dsname("TARGET.PDS");

    munit_assert_ptr_equal(found, d1);
    return MUNIT_OK;
}

/* Hit refreshes timestamp: after a find, last_used_time is not earlier
 * than it was before the call (guards against silent expiry of active
 * entries between two consecutive find calls). */
static MunitResult test_find_by_dsname_refreshes_timestamp(
    const MunitParameter params[], void *data)
{
    vfs_dir_t *d;
    time_t     before;
    (void)params; (void)data;

    dir_openlist_init();
    d = pool_alloc_named("MY.TEST.PDS");
    munit_assert_not_null(d);

    before = time(NULL);
    dir_openlist_find_by_dsname("MY.TEST.PDS");

    munit_assert(d->last_used_time >= before);
    return MUNIT_OK;
}

static MunitTest find_by_dsname_tests[] = {
    { "/found",               test_find_by_dsname_found,               NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/not_found",           test_find_by_dsname_not_found,           NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/timed_out",           test_find_by_dsname_timed_out,           NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/ignores_free",        test_find_by_dsname_ignores_free,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/multiple",            test_find_by_dsname_multiple,            NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/refreshes_timestamp", test_find_by_dsname_refreshes_timestamp, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/init",            init_tests,             NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/find_free",       find_free_tests,        NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/free",            free_tests,             NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/find_by_dsname",  find_by_dsname_tests,   NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/search/empty",    search_empty_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/search/eq",       search_eq_tests,        NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/search/gt",       search_gt_tests,        NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/search/lt",       search_lt_tests,        NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/search/ge",       search_ge_tests,        NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/search/le",       search_le_tests,        NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvsdol_suite = {
    "/mvsdol",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
