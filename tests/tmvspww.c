/*
 * tests/tmvspww.c
 *
 * Unit tests for mvspww.c -- the pending-member write pool.
 *
 * These exercise the in-memory buffer management only:
 *   pww_init, pww_create, pww_write, pww_truncate, pww_find, pww_discard,
 *   and the non-flushing paths of pww_flush_member.
 *
 * The flush/STOW path (pdsflush_slot -> fopen/STOW/mvs_dynalloc/mvs_stow) is
 * MVS-only real I/O and is NOT exercised here; every test is careful never to
 * trigger a flush (no dirty-slot flush, no pool eviction).  Where a test needs
 * a "just-flushed" (clean) slot it clears pm->dirty directly through the
 * pending_member_t pointer that pww_find() returns.
 *
 * Exception: /write/grows_buffer crosses PWW_SPILL_THRESHOLD and so exercises
 * the Phase 2 spill (mvsspl.c) -- a real temp file (tmpfile() on the dev host,
 * a &&-temp dataset on MVS).  It reads the content back through
 * pww_read_range() and discards the member at the end to close the scratch.
 *
 * pww_init() memset()s the pool without freeing buffers, so a test that
 * re-inits leaks any prior malloc'd buffer -- acceptable for a short-lived
 * test process (same pattern as the mvs_rcache tests).
 *
 * JCC C89 compliance: all declarations precede statements; block comments only.
 */

#include <string.h>
#include <errno.h>

#include "munit.h"
#include "mvspww.h"
#include "mvsspl.h"    /* has_spill_file_open() -- do not test pm->spill directly */

#define DS   "TEMP.TEST.CNTL"
#define MEM  "MEMBER1"

/* ==================================================================== */
/* pww_init                                                             */
/* ==================================================================== */

static MunitResult test_init_clears(const MunitParameter p[], void *d)
{
    (void)p; (void)d;
    pww_init();
    pww_create(0, 0, DS, MEM);
    munit_assert_ptr_not_null(pww_find(DS, MEM));

    pww_init();
    munit_assert_ptr_null(pww_find(DS, MEM));
    return MUNIT_OK;
}

static MunitTest init_tests[] = {
    { "/clears", test_init_clears, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* pww_create                                                           */
/* ==================================================================== */

static MunitResult test_create_new(const MunitParameter p[], void *d)
{
    pending_member_t *pm;
    (void)p; (void)d;

    pww_init();
    munit_assert_int(pww_create(0, 0, DS, MEM), ==, 0);

    pm = pww_find(DS, MEM);
    munit_assert_ptr_not_null(pm);
    munit_assert_int(pm->high_water, ==, 0);
    munit_assert_int(pm->dirty,      ==, 1);   /* empty create still stows */
    return MUNIT_OK;
}

static MunitResult test_create_recreate_truncates(const MunitParameter p[], void *d)
{
    static const uint8_t data[5] = { 1, 2, 3, 4, 5 };
    pending_member_t *pm;
    (void)p; (void)d;

    pww_init();
    pww_create(0, 0, DS, MEM);
    pww_write(0, 0, DS, MEM, data, 5, 0);
    munit_assert_int(pww_find(DS, MEM)->high_water, ==, 5);

    pww_create(0, 0, DS, MEM);              /* re-create truncates contents */
    pm = pww_find(DS, MEM);
    munit_assert_int(pm->high_water, ==, 0);
    munit_assert_int(pm->dirty,      ==, 1);
    return MUNIT_OK;
}

static MunitTest create_tests[] = {
    { "/new",       test_create_new,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/recreate",  test_create_recreate_truncates, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* pww_write                                                            */
/* ==================================================================== */

static MunitResult test_write_basic(const MunitParameter p[], void *d)
{
    static const uint8_t data[5] = { 'H', 'e', 'l', 'l', 'o' };
    pending_member_t *pm;
    (void)p; (void)d;

    pww_init();
    munit_assert_int(pww_write(0, 0, DS, MEM, data, 5, 0), ==, 0);

    pm = pww_find(DS, MEM);
    munit_assert_ptr_not_null(pm);
    munit_assert_int(pm->high_water, ==, 5);
    munit_assert_memory_equal(5, pm->buf, data);
    return MUNIT_OK;
}

static MunitResult test_write_append(const MunitParameter p[], void *d)
{
    static const uint8_t a[5] = { 'A', 'B', 'C', 'D', 'E' };
    static const uint8_t b[3] = { 'F', 'G', 'H' };
    pending_member_t *pm;
    (void)p; (void)d;

    pww_init();
    pww_write(0, 0, DS, MEM, a, 5, 0);
    pww_write(0, 0, DS, MEM, b, 3, 5);      /* append at offset 5 */

    pm = pww_find(DS, MEM);
    munit_assert_int(pm->high_water, ==, 8);
    munit_assert_memory_equal(5, pm->buf,     a);
    munit_assert_memory_equal(3, pm->buf + 5, b);
    return MUNIT_OK;
}

static MunitResult test_write_offset_gap(const MunitParameter p[], void *d)
{
    static const uint8_t data[4] = { 'W', 'X', 'Y', 'Z' };
    pending_member_t *pm;
    int i;
    (void)p; (void)d;

    pww_init();
    pww_write(0, 0, DS, MEM, data, 4, 10);  /* write past a hole */

    pm = pww_find(DS, MEM);
    munit_assert_int(pm->high_water, ==, 14);
    for (i = 0; i < 10; i++)                 /* gap is zero-filled */
        munit_assert_int(pm->buf[i], ==, 0);
    munit_assert_memory_equal(4, pm->buf + 10, data);
    return MUNIT_OK;
}

static MunitResult test_write_overwrite(const MunitParameter p[], void *d)
{
    static const uint8_t base[10] = { 'A','B','C','D','E','F','G','H','I','J' };
    static const uint8_t over[2]  = { 'x', 'y' };
    pending_member_t *pm;
    (void)p; (void)d;

    pww_init();
    pww_write(0, 0, DS, MEM, base, 10, 0);
    pww_write(0, 0, DS, MEM, over, 2, 3);   /* overwrite bytes 3..4 */

    pm = pww_find(DS, MEM);
    munit_assert_int(pm->high_water, ==, 10);  /* size unchanged */
    munit_assert_int(pm->buf[3], ==, 'x');
    munit_assert_int(pm->buf[4], ==, 'y');
    munit_assert_int(pm->buf[2], ==, 'C');
    munit_assert_int(pm->buf[5], ==, 'F');
    return MUNIT_OK;
}

static MunitResult test_write_grows_buffer(const MunitParameter p[], void *d)
{
    uint8_t           b = 0xAB;
    uint8_t           rb[1];
    pending_member_t *pm;
    (void)p; (void)d;

    /* One byte at a high offset (70000 > PWW_SPILL_THRESHOLD) forces the member
       to SPILL to the temp dataset, zero-filling the hole on disk, without a
       huge source array. */
    pww_init();
    munit_assert_int(pww_write(0, 0, DS, MEM, &b, 1, 69999), ==, 0);

    pm = pww_find(DS, MEM);
    munit_assert_ptr_not_null(pm);
    munit_assert_int(pm->high_water,   ==, 70000);
    munit_assert_int(has_spill_file_open(pm), !=, 0);  /* spilled to disk */
    munit_assert_ptr_null(pm->buf);            /* in-memory buffer freed */

    /* Read the content back through the accessor: the written byte, and a
       zero from the hole the spill zero-filled. */
    munit_assert_int(pww_read_range(pm, 69999, rb, 1), ==, 0);
    munit_assert_int(rb[0], ==, 0xAB);
    munit_assert_int(pww_read_range(pm, 0, rb, 1), ==, 0);
    munit_assert_int(rb[0], ==, 0);

    pww_discard(DS, MEM);   /* close + release the scratch dataset */
    return MUNIT_OK;
}

/* Spilled member: a large first write (spills immediately), then an overwrite
   within it, then an append past a hole -- all read back via pww_read_range,
   exercising spill_write's overwrite / zero-fill / extend paths. */
static MunitResult test_write_spill_segments(const MunitParameter p[], void *d)
{
    static const uint8_t a[4] = { 'A', 'A', 'A', 'A' };
    static const uint8_t z[4] = { 'Z', 'Z', 'Z', 'Z' };
    static uint8_t   big[20000];
    uint8_t          rb[4];
    pending_member_t *pm;
    int               i;
    (void)p; (void)d;

    pww_init();

    for (i = 0; i < (int)sizeof(big); i++)
        big[i] = (uint8_t)i;
    /* 20000 > PWW_SPILL_THRESHOLD -> spills on this first write. */
    munit_assert_int(pww_write(0, 0, DS, MEM, big, sizeof(big), 0), ==, 0);
    pm = pww_find(DS, MEM);
    munit_assert_int(has_spill_file_open(pm), !=, 0);

    /* Overwrite 4 bytes within, and append 4 bytes past a 100-byte hole. */
    munit_assert_int(pww_write(0, 0, DS, MEM, a, 4, 50),    ==, 0);
    munit_assert_int(pww_write(0, 0, DS, MEM, z, 4, 20100), ==, 0);
    pm = pww_find(DS, MEM);
    munit_assert_int(pm->high_water, ==, 20104);

    munit_assert_int(pww_read_range(pm, 50, rb, 4), ==, 0);   /* overwrite   */
    munit_assert_memory_equal(4, rb, a);
    munit_assert_int(pww_read_range(pm, 10, rb, 1), ==, 0);   /* original    */
    munit_assert_int(rb[0], ==, (uint8_t)10);
    munit_assert_int(pww_read_range(pm, 20050, rb, 1), ==, 0);/* hole = zero */
    munit_assert_int(rb[0], ==, 0);
    munit_assert_int(pww_read_range(pm, 20100, rb, 4), ==, 0);/* appended    */
    munit_assert_memory_equal(4, rb, z);

    pww_discard(DS, MEM);
    return MUNIT_OK;
}

static MunitResult test_write_over_cap(const MunitParameter p[], void *d)
{
    uint8_t b = 1;
    (void)p; (void)d;

    pww_init();
    errno = 0;
    munit_assert_int(pww_write(0, 0, DS, MEM, &b, 1,
                               (uint64_t)PWW_MAX_MEMBER_BYTES), ==, -1);
    munit_assert_int(errno, ==, ENOSPC);
    munit_assert_ptr_null(pww_find(DS, MEM));   /* nothing was allocated */
    return MUNIT_OK;
}

static MunitTest write_tests[] = {
    { "/basic",       test_write_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/append",      test_write_append,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/offset_gap",  test_write_offset_gap,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/overwrite",   test_write_overwrite,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/grows_buffer",test_write_grows_buffer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/spill_segments", test_write_spill_segments, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/over_cap",    test_write_over_cap,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* pww_truncate                                                         */
/* ==================================================================== */

/* Helper: make a clean (as-if just-flushed) member of 'size' bytes. */
static pending_member_t *make_clean_member(uint32_t size)
{
    uint8_t b = 0xEE;
    pending_member_t *pm;

    pww_init();
    if (size > 0)
        pww_write(0, 0, DS, MEM, &b, 1, (uint64_t)size - 1);  /* high_water=size */
    else
        pww_create(0, 0, DS, MEM);
    pm = pww_find(DS, MEM);
    pm->dirty = 0;                          /* pretend it has been stowed */
    return pm;
}

/* A truncate to the current size must NOT re-dirty the slot (the fix that
   stopped the double-STOW after WRITE->COMMIT->SETATTR). */
static MunitResult test_truncate_noop_same_size(const MunitParameter p[], void *d)
{
    pending_member_t *pm;
    (void)p; (void)d;

    pm = make_clean_member(32);
    munit_assert_int(pww_truncate(0, 0, DS, MEM, 32), ==, 0);

    pm = pww_find(DS, MEM);
    munit_assert_int(pm->dirty,      ==, 0);    /* still clean */
    munit_assert_int(pm->high_water, ==, 32);
    return MUNIT_OK;
}

static MunitResult test_truncate_shrink(const MunitParameter p[], void *d)
{
    pending_member_t *pm;
    (void)p; (void)d;

    make_clean_member(32);
    munit_assert_int(pww_truncate(0, 0, DS, MEM, 16), ==, 0);

    pm = pww_find(DS, MEM);
    munit_assert_int(pm->high_water, ==, 16);
    munit_assert_int(pm->dirty,      ==, 1);    /* a real change re-dirties */
    return MUNIT_OK;
}

static MunitResult test_truncate_grow(const MunitParameter p[], void *d)
{
    static const uint8_t data[10] = { 1,2,3,4,5,6,7,8,9,10 };
    pending_member_t *pm;
    int i;
    (void)p; (void)d;

    pww_init();
    pww_write(0, 0, DS, MEM, data, 10, 0);
    pm = pww_find(DS, MEM);
    pm->dirty = 0;

    munit_assert_int(pww_truncate(0, 0, DS, MEM, 20), ==, 0);
    pm = pww_find(DS, MEM);
    munit_assert_int(pm->high_water, ==, 20);
    munit_assert_int(pm->dirty,      ==, 1);
    for (i = 10; i < 20; i++)                /* grown region is zero-filled */
        munit_assert_int(pm->buf[i], ==, 0);
    return MUNIT_OK;
}

static MunitResult test_truncate_zero_creates(const MunitParameter p[], void *d)
{
    pending_member_t *pm;
    (void)p; (void)d;

    /* Truncate-to-0 of a not-yet-pending member starts a fresh empty member
       (the O_TRUNC-before-write case). */
    pww_init();
    munit_assert_int(pww_truncate(0, 0, DS, MEM, 0), ==, 0);

    pm = pww_find(DS, MEM);
    munit_assert_ptr_not_null(pm);
    munit_assert_int(pm->high_water, ==, 0);
    munit_assert_int(pm->dirty,      ==, 1);
    return MUNIT_OK;
}

static MunitResult test_truncate_nonzero_no_slot(const MunitParameter p[], void *d)
{
    (void)p; (void)d;

    /* A non-zero truncate of an on-disk-only member is a Phase-1 no-op. */
    pww_init();
    munit_assert_int(pww_truncate(0, 0, DS, MEM, 100), ==, 0);
    munit_assert_ptr_null(pww_find(DS, MEM));   /* no slot created */
    return MUNIT_OK;
}

static MunitResult test_truncate_over_cap(const MunitParameter p[], void *d)
{
    (void)p; (void)d;

    pww_init();
    errno = 0;
    munit_assert_int(pww_truncate(0, 0, DS, MEM,
                                  (uint32_t)PWW_MAX_MEMBER_BYTES + 1), ==, -1);
    munit_assert_int(errno, ==, ENOSPC);
    return MUNIT_OK;
}

static MunitTest truncate_tests[] = {
    { "/noop_same_size", test_truncate_noop_same_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/shrink",         test_truncate_shrink,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/grow",           test_truncate_grow,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/zero_creates",   test_truncate_zero_creates,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nonzero_no_slot",test_truncate_nonzero_no_slot,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/over_cap",       test_truncate_over_cap,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* pww_find / pww_discard / pww_flush_member (non-flushing paths)        */
/* ==================================================================== */

static MunitResult test_find_absent(const MunitParameter p[], void *d)
{
    (void)p; (void)d;
    pww_init();
    munit_assert_ptr_null(pww_find(DS, "NOSUCH"));
    return MUNIT_OK;
}

static MunitResult test_discard_present(const MunitParameter p[], void *d)
{
    (void)p; (void)d;
    pww_init();
    pww_create(0, 0, DS, MEM);
    munit_assert_int(pww_discard(DS, MEM), ==, 1);   /* dropped one */
    munit_assert_ptr_null(pww_find(DS, MEM));        /* and it's gone */
    return MUNIT_OK;
}

static MunitResult test_discard_absent(const MunitParameter p[], void *d)
{
    (void)p; (void)d;
    pww_init();
    munit_assert_int(pww_discard(DS, MEM), ==, 0);   /* nothing to drop */
    return MUNIT_OK;
}

static MunitResult test_flush_member_not_found(const MunitParameter p[], void *d)
{
    (void)p; (void)d;
    pww_init();
    munit_assert_int(pww_flush_member(DS, MEM), ==, 0);  /* nothing pending */
    return MUNIT_OK;
}

static MunitResult test_flush_member_not_dirty(const MunitParameter p[], void *d)
{
    pending_member_t *pm;
    (void)p; (void)d;

    pww_init();
    pww_create(0, 0, DS, MEM);
    pm = pww_find(DS, MEM);
    pm->dirty = 0;                                    /* as if already stowed */
    munit_assert_int(pww_flush_member(DS, MEM), ==, 0);  /* nothing to flush */
    return MUNIT_OK;
}

static MunitTest find_tests[] = {
    { "/find_absent",           test_find_absent,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/discard_present",       test_discard_present,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/discard_absent",        test_discard_absent,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/flush_not_found",       test_flush_member_not_found,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/flush_not_dirty",       test_flush_member_not_dirty,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                   */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/init",     init_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/create",   create_tests,   NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/write",    write_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/truncate", truncate_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/find",     find_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvspww_suite = {
    "/mvspww",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
