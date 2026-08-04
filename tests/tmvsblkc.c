/*
 * tests/tmvsblkc.c - Unit tests for mvsblkc.c (PDS space prediction).
 *
 * Suite prefix: /mvsblkc
 * Sub-suites  : /recs_fb  /recs_vb  /incremental  /fit  /dsinit
 *
 * These cover the two pure halves of the module: turning a byte stream into
 * blocks, and turning blocks into a yes/no against the dataset's geometry.
 * Neither needs an export table, a config file or a VTOC -- which is the
 * whole reason blkcalc_will_member_fit() takes a dataset_dscb_info_t * and
 * not a pair of table indexes.
 *
 * blkcalc_admit_write() is NOT covered here: it reaches into the pending
 * write pool and the real VTOC, so it belongs to the integration suite.
 *
 * The expected values below come from the measured JCC behaviour recorded in
 * doc/design_pds_full_prediction.md Sec 7.3 -- notably that a long line WRAPS
 * on both F/FB and V/VB, that an empty line is still a record, and that a
 * trailing run with no newline is written as a record too.
 *
 * ASCII, NOT EBCDIC.  A pending member's byte stream is ASCII (it is
 * translated only as the flush writes it), so the line terminator the code
 * looks for is 0x0A.  On JCC a '\n' literal is EBCDIC 0x15, so test data is
 * built from explicit byte values here rather than from string literals --
 * "hello\n" in a source literal would NOT contain a terminator the module
 * recognises.
 *
 * Build (from project root):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c tests/tmvsblkc.c \
 *      src/mvsblkc.c src/mvsutl.c src/logger.c tests/munit.c \
 *      -o tests/runall_blkc
 */

#include "munit.h"
#include "mvsblkc.h"
#include "mvsutl.h"
#include <string.h>

#define ASCII_LF  0x0A
#define ASCII_CR  0x0D
#define FILLER    0x78          /* ASCII 'x' -- any non-terminator byte */

/* ==================================================================== */
/* Helpers -- all test data is built byte by byte, see the note above    */
/* ==================================================================== */

static char g_tbuf[1024];

/* n filler characters followed by a line terminator. */
static void add_line(blkcalc_info_t *bi, int n)
{
    int i;

    for (i = 0; i < n && i < (int)sizeof(g_tbuf) - 1; i++)
        g_tbuf[i] = (char)FILLER;
    g_tbuf[i++] = (char)ASCII_LF;
    blkcalc_add_blocks_for_data(bi, g_tbuf, i);
}

/* n filler characters with NO terminator. */
static void add_bytes(blkcalc_info_t *bi, int n)
{
    int i;

    for (i = 0; i < n && i < (int)sizeof(g_tbuf); i++)
        g_tbuf[i] = (char)FILLER;
    blkcalc_add_blocks_for_data(bi, g_tbuf, i);
}

/* Fill 'dst' with n filler characters then a terminator; returns the length. */
static int make_line(char *dst, int n)
{
    int i;

    for (i = 0; i < n; i++)
        dst[i] = (char)FILLER;
    dst[i++] = (char)ASCII_LF;
    return i;
}

/* A 3390 PDS with a usable geometry, for the fit tests. */
static void ds_3390(dataset_dscb_info_t *ds, uint16_t blksize,
                    uint32_t tracks, uint32_t lstar_tt, uint8_t lstar_r)
{
    memset(ds, 0, sizeof(*ds));
    ds->valid    = 1;
    ds->blksize  = blksize;
    ds->lrecl    = 80;
    ds->devcode  = 0x0F;             /* 3390 */
    ds->trklen   = 58786;
    ds->trkcyl   = 15;
    ds->tracks   = tracks;
    ds->nextents = 1;
    ds->lstar_tt = lstar_tt;
    ds->lstar_r  = lstar_r;
    ds->trbal    = 0xFFFF;           /* plenty: do not let it be the limit */
    ds->blocks_per_track =
        mvs_blocks_per_track(ds->devcode, ds->trklen, ds->blksize);
}

/* ==================================================================== */
/* /recs_fb -- fixed format record counting                              */
/* ==================================================================== */

/* A short line is one record; 10 of them fill exactly one 800-byte block. */
static MunitResult test_fb_exact_block(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    int            i;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    for (i = 0; i < 10; i++)
        add_line(&bi, 5);

    munit_assert_int(bi.count_full_blocks, ==, 1);
    munit_assert_int(bi.size_last_partial_block, ==, 0);
    munit_assert_int(blkcalc_total_blocks(&bi), ==, 1);
    return MUNIT_OK;
}

/* One more record starts a second, partial block. */
static MunitResult test_fb_partial_block(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    int            i;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    for (i = 0; i < 11; i++)
        add_line(&bi, 5);

    munit_assert_int(bi.count_full_blocks, ==, 1);
    munit_assert_int(bi.size_last_partial_block, ==, 80);
    munit_assert_int(blkcalc_total_blocks(&bi), ==, 2);
    return MUNIT_OK;
}

/* The measured case from the design: 100 chars at LRECL=80 wraps into TWO
   records, occupying 160 bytes -- it does not truncate to one. */
static MunitResult test_fb_long_line_wraps(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    add_line(&bi, 100);

    munit_assert_int(bi.size_last_partial_block, ==, 160);
    return MUNIT_OK;
}

/* Exactly LRECL is one record, not two: the boundary must not round up. */
static MunitResult test_fb_line_exactly_lrecl(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    add_line(&bi, 80);

    munit_assert_int(bi.size_last_partial_block, ==, 80);
    return MUNIT_OK;
}

/* Consecutive newlines are blank records, one each -- not dropped. */
static MunitResult test_fb_empty_lines_are_records(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    int            i;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    for (i = 0; i < 3; i++)
        add_line(&bi, 0);

    munit_assert_int(bi.size_last_partial_block, ==, 240);
    return MUNIT_OK;
}

/* A trailing run with no newline is written as a record, so it counts --
   but only when the total is asked for, never in the running state. */
static MunitResult test_fb_unterminated_tail(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    add_bytes(&bi, 3);

    munit_assert_int(bi.last_unterm_text_line_chars, ==, 3);
    munit_assert_int(bi.size_last_partial_block, ==, 0);
    munit_assert_int(blkcalc_total_blocks(&bi), ==, 1);

    /* And asking must not have changed anything. */
    munit_assert_int(bi.size_last_partial_block, ==, 0);
    munit_assert_int(blkcalc_total_blocks(&bi), ==, 1);
    return MUNIT_OK;
}

/* Empty member: no records at all. */
static MunitResult test_fb_empty(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    munit_assert_int(blkcalc_total_blocks(&bi), ==, 0);
    return MUNIT_OK;
}

/* A CR is ordinary data, not a terminator: CRLF text keeps the CR in the
   record, which is what the flush actually writes. */
static MunitResult test_fb_cr_is_data(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    char           buf[4];
    (void)params; (void)data;

    buf[0] = (char)FILLER;
    buf[1] = (char)FILLER;
    buf[2] = (char)ASCII_CR;
    buf[3] = (char)ASCII_LF;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    blkcalc_add_blocks_for_data(&bi, buf, 4);

    munit_assert_int(bi.size_last_partial_block, ==, 80);
    munit_assert_int(bi.last_unterm_text_line_chars, ==, 0);
    return MUNIT_OK;
}

static MunitTest recs_fb_tests[] = {
    { "/exact_block",        test_fb_exact_block,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/partial_block",      test_fb_partial_block,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/long_line_wraps",    test_fb_long_line_wraps,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/line_exactly_lrecl", test_fb_line_exactly_lrecl, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/empty_lines",        test_fb_empty_lines_are_records, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/unterminated_tail",  test_fb_unterminated_tail,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/empty",              test_fb_empty,              NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/cr_is_data",         test_fb_cr_is_data,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /recs_vb -- variable format record counting                           */
/* ==================================================================== */

/* One short line: BDW(4) + RDW(4) + data. */
static MunitResult test_vb_one_line(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_V, 800, 84);
    add_line(&bi, 5);

    munit_assert_int(bi.size_last_partial_block, ==, 4 + 4 + 5);
    munit_assert_int(blkcalc_total_blocks(&bi), ==, 1);
    return MUNIT_OK;
}

/* An empty line is a zero length record: RDW only, 4 bytes. */
static MunitResult test_vb_empty_line_is_rdw_only(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_V, 800, 84);
    add_line(&bi, 0);
    add_line(&bi, 0);

    munit_assert_int(bi.size_last_partial_block, ==, 4 + 4 + 4);
    return MUNIT_OK;
}

/* V/VB wraps too (proven), at LRECL-4 bytes of data per record. */
static MunitResult test_vb_long_line_wraps(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    (void)params; (void)data;

    /* LRECL 84 -> 80 bytes of data per record.  A 100 char line becomes an
       80 byte record and a 20 byte one: BDW + (4+80) + (4+20). */
    blkcalc_info_init(&bi, RECFM_V, 800, 84);
    add_line(&bi, 100);

    munit_assert_int(bi.size_last_partial_block, ==, 4 + 84 + 24);
    return MUNIT_OK;
}

/* Records are NOT padded, so a block holds as many as physically fit and
   the count depends on their real lengths. */
static MunitResult test_vb_block_fills_by_bytes(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    int            i;
    (void)params; (void)data;

    /* BLKSIZE 104: BDW(4) + ten 10-byte records (4+6) = 104 exactly. */
    blkcalc_info_init(&bi, RECFM_V, 104, 84);
    for (i = 0; i < 10; i++)
        add_line(&bi, 6);

    munit_assert_int(bi.count_full_blocks, ==, 0);
    munit_assert_int(bi.size_last_partial_block, ==, 104);

    /* The eleventh cannot fit, so it opens a second block. */
    add_line(&bi, 6);
    munit_assert_int(bi.count_full_blocks, ==, 1);
    munit_assert_int(bi.size_last_partial_block, ==, 4 + 10);
    munit_assert_int(blkcalc_total_blocks(&bi), ==, 2);
    return MUNIT_OK;
}

static MunitTest recs_vb_tests[] = {
    { "/one_line",           test_vb_one_line,               NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/empty_line_rdw",     test_vb_empty_line_is_rdw_only, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/long_line_wraps",    test_vb_long_line_wraps,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/block_fills",        test_vb_block_fills_by_bytes,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /incremental -- chunk boundaries must not change the answer           */
/* ==================================================================== */

/* A line split across two calls is ONE line of the combined length, and a
   long one must wrap on that total rather than on either fragment. */
static MunitResult test_inc_line_split_across_calls(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t split;
    blkcalc_info_t whole;
    char           buf[128];
    int            n;
    (void)params; (void)data;

    n = make_line(buf, 100);      /* 100 filler chars + terminator = 101 */

    blkcalc_info_init(&split, RECFM_F, 800, 80);
    blkcalc_add_blocks_for_data(&split, buf, 45);
    blkcalc_add_blocks_for_data(&split, buf + 45, n - 45);

    blkcalc_info_init(&whole, RECFM_F, 800, 80);
    blkcalc_add_blocks_for_data(&whole, buf, n);

    munit_assert_int(split.count_full_blocks, ==, whole.count_full_blocks);
    munit_assert_int(split.size_last_partial_block, ==,
                     whole.size_last_partial_block);
    munit_assert_int(split.size_last_partial_block, ==, 160);
    return MUNIT_OK;
}

/* The same text fed one byte at a time must give the same totals. */
static MunitResult test_inc_byte_at_a_time(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t drip;
    blkcalc_info_t whole;
    char           buf[256];
    int            n = 0;
    int            i;
    (void)params; (void)data;

    n += make_line(buf + n, 10);      /* a short line              */
    n += make_line(buf + n, 12);      /* another                   */
    n += make_line(buf + n, 0);       /* an empty one              */
    n += make_line(buf + n, 95);      /* one that wraps            */
    for (i = 0; i < 4; i++)           /* an unterminated tail      */
        buf[n++] = (char)FILLER;

    blkcalc_info_init(&drip, RECFM_F, 800, 80);
    for (i = 0; i < n; i++)
        blkcalc_add_blocks_for_data(&drip, buf + i, 1);

    blkcalc_info_init(&whole, RECFM_F, 800, 80);
    blkcalc_add_blocks_for_data(&whole, buf, n);

    munit_assert_int(blkcalc_total_blocks(&drip), ==,
                     blkcalc_total_blocks(&whole));
    munit_assert_int(drip.last_unterm_text_line_chars, ==,
                     whole.last_unterm_text_line_chars);
    munit_assert_int(drip.last_unterm_text_line_chars, ==, 4);
    return MUNIT_OK;
}

/* A newline arriving in a later chunk closes the carried line. */
static MunitResult test_inc_newline_next_chunk(
    const MunitParameter params[], void *data)
{
    blkcalc_info_t bi;
    char           lf = (char)ASCII_LF;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    add_bytes(&bi, 3);
    munit_assert_int(bi.size_last_partial_block, ==, 0);

    blkcalc_add_blocks_for_data(&bi, &lf, 1);
    munit_assert_int(bi.size_last_partial_block, ==, 80);
    munit_assert_int(bi.last_unterm_text_line_chars, ==, 0);
    return MUNIT_OK;
}

static MunitTest incremental_tests[] = {
    { "/line_split",      test_inc_line_split_across_calls, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/byte_at_a_time",  test_inc_byte_at_a_time,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/newline_later",   test_inc_newline_next_chunk,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /fit -- blocks against the dataset geometry                           */
/* ==================================================================== */

static MunitResult test_fit_empty_dataset_fits(
    const MunitParameter params[], void *data)
{
    dataset_dscb_info_t ds;
    blkcalc_info_t      bi;
    uint32_t            ttr = 0;
    (void)params; (void)data;

    ds_3390(&ds, 800, 10, 0, 0);
    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    add_line(&bi, 5);

    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, 0, &ttr), ==, 0);
    return MUNIT_OK;
}

/* A dataset with nothing left cannot take another block. */
static MunitResult test_fit_full_dataset_refuses(
    const MunitParameter params[], void *data)
{
    dataset_dscb_info_t ds;
    blkcalc_info_t      bi;
    uint32_t            ttr;
    uint32_t            last;
    (void)params; (void)data;

    ds_3390(&ds, 800, 2, 1, 0);
    ds.trbal      = 0;            /* nothing left on the last track      */
    ds.sec_qty    = 0;
    ds.sec_tracks = 0;            /* and it cannot extend                */
    ds.lstar_r    = (uint8_t)ds.blocks_per_track;   /* track 1 is full   */

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    add_line(&bi, 5);

    last = ((uint32_t)ds.lstar_tt << 8) | ds.lstar_r;
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, last, &ttr), ==, -1);
    return MUNIT_OK;
}

/* Secondary space is credited, but only for the extents still unused. */
static MunitResult test_fit_secondary_counts(
    const MunitParameter params[], void *data)
{
    dataset_dscb_info_t ds;
    blkcalc_info_t      bi;
    uint32_t            ttr;
    uint32_t            last;
    int                 i;
    (void)params; (void)data;

    ds_3390(&ds, 800, 1, 0, 0);
    ds.trbal   = 0;
    ds.lstar_r = (uint8_t)ds.blocks_per_track;   /* the one track is full */

    /* 200 records = 20 blocks at 10 per block: more than one track. */
    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    for (i = 0; i < 200; i++)
        add_line(&bi, 5);

    last = ((uint32_t)ds.lstar_tt << 8) | ds.lstar_r;

    ds.nextents   = 16;           /* at the limit: no more extents        */
    ds.sec_tracks = 50;
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, last, &ttr), ==, -1);

    ds.nextents   = 1;            /* room to extend: now it fits          */
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, last, &ttr), ==, 0);
    return MUNIT_OK;
}

/* Chaining: feeding one member's predicted end into the next is what makes
   several pending members for one dataset add up rather than each being
   judged against the same starting point. */
static MunitResult test_fit_chained_ttr_advances(
    const MunitParameter params[], void *data)
{
    dataset_dscb_info_t ds;
    blkcalc_info_t      bi;
    uint32_t            ttr1 = 0;
    uint32_t            ttr2 = 0;
    int                 i;
    (void)params; (void)data;

    ds_3390(&ds, 800, 100, 0, 0);

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    for (i = 0; i < 100; i++)     /* 10 blocks */
        add_line(&bi, 5);

    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, 0, &ttr1), ==, 0);
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, ttr1, &ttr2), ==, 0);

    munit_assert_int((int)ttr1, >, 0);
    munit_assert_int((int)ttr2, >, (int)ttr1);
    return MUNIT_OK;
}

/* Chaining onto the DS1LSTAR track must not spend DS1TRBAL twice.  TRBAL
   describes the track as the VTOC found it and knows nothing about blocks we
   have already predicted onto it, so the room left has to be reduced by what
   we put there. */
static MunitResult test_fit_trbal_not_double_spent(
    const MunitParameter params[], void *data)
{
    dataset_dscb_info_t ds;
    blkcalc_info_t      bi;
    uint32_t            ttr1 = 0;
    uint32_t            ttr2 = 0;
    uint32_t            start;
    int                 i;
    (void)params; (void)data;

    /* One track, and TRBAL says exactly one 800-byte block still fits. */
    ds_3390(&ds, 800, 1, 0, 0);
    ds.sec_tracks = 0;
    ds.trbal      = (uint16_t)(ds.trklen / (uint32_t)ds.blocks_per_track);

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    for (i = 0; i < 10; i++)          /* exactly one block */
        add_line(&bi, 5);

    /* The first member takes that one block. */
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, 0, &ttr1), ==, 0);

    /* A second identical member cannot: the only free block is now used.
       Before the fix, TRBAL was re-read for the same track and this fitted. */
    start = ttr1;
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, start, &ttr2), ==, -1);
    return MUNIT_OK;
}

/* An EMPTY member still costs a block.  Stowing one writes an EOF marker, so
   a full dataset cannot take it -- and without this the fit test would answer
   "0 blocks needed, fits" for the empty member an NFS CREATE makes, which is
   exactly how an empty member reached the flush and abended (STC02334). */
static MunitResult test_fit_empty_member_costs_a_block(
    const MunitParameter params[], void *data)
{
    dataset_dscb_info_t ds;
    blkcalc_info_t      bi;
    uint32_t            ttr;
    (void)params; (void)data;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    munit_assert_int(blkcalc_total_blocks(&bi), ==, 0);   /* no content */

    /* Full dataset: one track, nothing free, no room to extend. */
    ds_3390(&ds, 800, 1, 0, 0);
    ds.trbal      = 0;
    ds.sec_tracks = 0;
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, 0, &ttr), ==, -1);

    /* With room, the same empty member is fine. */
    ds_3390(&ds, 800, 10, 0, 0);
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, 0, &ttr), ==, 0);
    return MUNIT_OK;
}

/* An unusable DSCB must answer "no", never "yes": a zero blocks-per-track
   would otherwise make everything look like it fits. */
static MunitResult test_fit_invalid_ds_refuses(
    const MunitParameter params[], void *data)
{
    dataset_dscb_info_t ds;
    blkcalc_info_t      bi;
    uint32_t            ttr;
    (void)params; (void)data;

    ds_3390(&ds, 800, 10, 0, 0);
    blkcalc_info_init(&bi, RECFM_F, 800, 80);

    ds.valid = 0;
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, 0, &ttr), ==, -1);

    ds.valid = 1;
    ds.blocks_per_track = 0;
    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, 0, &ttr), ==, -1);
    return MUNIT_OK;
}

/* DS1TRBAL and the record number both describe the current track; the
   smaller wins, because guessing high is what produces an abend. */
static MunitResult test_fit_trbal_limits_current_track(
    const MunitParameter params[], void *data)
{
    dataset_dscb_info_t ds;
    blkcalc_info_t      bi;
    uint32_t            ttr;
    int                 i;
    (void)params; (void)data;

    /* One track only, nothing written, but TRBAL says the track is full. */
    ds_3390(&ds, 800, 1, 0, 0);
    ds.trbal      = 0;
    ds.sec_tracks = 0;

    blkcalc_info_init(&bi, RECFM_F, 800, 80);
    for (i = 0; i < 10; i++)      /* one block */
        add_line(&bi, 5);

    munit_assert_int(blkcalc_will_member_fit(&bi, &ds, 0, &ttr), ==, -1);
    return MUNIT_OK;
}

static MunitTest fit_tests[] = {
    { "/empty_fits",       test_fit_empty_dataset_fits,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/full_refuses",     test_fit_full_dataset_refuses,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/secondary",        test_fit_secondary_counts,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/chained_ttr",      test_fit_chained_ttr_advances,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/trbal_once",       test_fit_trbal_not_double_spent,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/empty_costs_one",  test_fit_empty_member_costs_a_block, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/invalid_refuses",  test_fit_invalid_ds_refuses,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/trbal_limits",     test_fit_trbal_limits_current_track, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /dsinit -- decoding a raw VTOC entry                                  */
/* ==================================================================== */

static void raw_3390(mvs_dscb_info_t *r)
{
    memset(r, 0, sizeof(*r));
    r->status  = MVS_DSCB_ST_OK;
    memcpy(r->volser, "WORK04", 6);
    r->nextents = 1;
    r->tracks   = 30;
    r->dsorg[0] = MVS_DSCB_DSORG_PO;
    r->recfm    = (uint8_t)(MVS_DSCB_RECFM_F | MVS_DSCB_RECFM_BLK);
    r->blksize[0] = (uint8_t)(800 >> 8);
    r->blksize[1] = (uint8_t)(800 & 0xFF);
    r->lrecl[0]   = 0;
    r->lrecl[1]   = 80;
    r->devtype[3] = 0x0F;
    r->trklen[0]  = (uint8_t)(58786 >> 8);
    r->trklen[1]  = (uint8_t)(58786 & 0xFF);
    r->trkcyl[0]  = 0;
    r->trkcyl[1]  = 15;
    r->lstar[0]   = 0;
    r->lstar[1]   = 3;
    r->lstar[2]   = 5;
    r->trbal[0]   = 0x10;
    r->trbal[1]   = 0x00;
}

static MunitResult test_dsinit_decodes(
    const MunitParameter params[], void *data)
{
    mvs_dscb_info_t     raw;
    dataset_dscb_info_t ds;
    (void)params; (void)data;

    raw_3390(&raw);
    munit_assert_int(blkcalc_dataset_init(&ds, &raw), ==, 0);

    munit_assert_int((int)ds.valid,    ==, 1);
    munit_assert_int((int)ds.blksize,  ==, 800);
    munit_assert_int((int)ds.lrecl,    ==, 80);
    munit_assert_int((int)ds.tracks,   ==, 30);
    munit_assert_int((int)ds.lstar_tt, ==, 3);
    munit_assert_int((int)ds.lstar_r,  ==, 5);
    munit_assert_int((int)ds.trbal,    ==, 0x1000);
    munit_assert_int(ds.blocks_per_track, >, 0);
    munit_assert_string_equal(ds.volser, "WORK04");
    return MUNIT_OK;
}

/* A bad status must not produce a "valid" dataset -- that would read as
   "no limits" to the fit test. */
static MunitResult test_dsinit_rejects_bad_status(
    const MunitParameter params[], void *data)
{
    mvs_dscb_info_t     raw;
    dataset_dscb_info_t ds;
    (void)params; (void)data;

    raw_3390(&raw);
    raw.status = MVS_DSCB_ST_NOTFOUND;

    munit_assert_int(blkcalc_dataset_init(&ds, &raw), ==, -1);
    munit_assert_int((int)ds.valid, ==, 0);
    return MUNIT_OK;
}

/* Unreadable geometry likewise: blocks_per_track of 0 is not a usable
   answer and must not be dressed up as one. */
static MunitResult test_dsinit_rejects_no_geometry(
    const MunitParameter params[], void *data)
{
    mvs_dscb_info_t     raw;
    dataset_dscb_info_t ds;
    (void)params; (void)data;

    raw_3390(&raw);
    raw.blksize[0] = 0;
    raw.blksize[1] = 0;           /* nothing fits on a track */

    munit_assert_int(blkcalc_dataset_init(&ds, &raw), ==, -1);
    munit_assert_int((int)ds.valid, ==, 0);
    return MUNIT_OK;
}

/* Trailing blanks come off the volser, which the DSCB pads to 6. */
static MunitResult test_dsinit_volser_trimmed(
    const MunitParameter params[], void *data)
{
    mvs_dscb_info_t     raw;
    dataset_dscb_info_t ds;
    (void)params; (void)data;

    raw_3390(&raw);
    memcpy(raw.volser, "WRK1  ", 6);

    munit_assert_int(blkcalc_dataset_init(&ds, &raw), ==, 0);
    munit_assert_string_equal(ds.volser, "WRK1");
    return MUNIT_OK;
}

static MunitTest dsinit_tests[] = {
    { "/decodes",          test_dsinit_decodes,             NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/bad_status",       test_dsinit_rejects_bad_status,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/no_geometry",      test_dsinit_rejects_no_geometry, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/volser_trimmed",   test_dsinit_volser_trimmed,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/recs_fb",     recs_fb_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/recs_vb",     recs_vb_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/incremental", incremental_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/fit",         fit_tests,         NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/dsinit",      dsinit_tests,      NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvsblkc_suite = {
    "/mvsblkc",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
