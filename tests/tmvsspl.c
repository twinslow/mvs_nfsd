/*
 * tests/tmvsspl.c - Unit tests for mvsspl.c (the write-spill store).
 *
 * These are the tests that matter most for the intermittent corruption seen on
 * MVS: they drive spill_open/write/read/close directly, in as many odd patterns
 * as possible, and verify every byte.
 *
 * Method: a REFERENCE MODEL.  A plain in-memory array g_ref[] is kept in lock
 * step with the spill file -- every do_write() applies the identical change to
 * both (data at the offset, holes zero-filled), then verify_all() reads the
 * whole spill back through spill_read() and compares it byte-for-byte to g_ref.
 * Any RMW slip, buffering loss, wrong offset, or hole mis-fill shows up as a
 * mismatch at the exact byte.
 *
 * These run the REAL spill code: tmpfile() on the dev host, a &&-temp dataset
 * on MVS -- so the MVS run exercises the actual JCC block I/O where the bug
 * lives.  The "random" tests use a small fixed-seed LCG (below), so every run
 * drives the exact same sequence -- a failure is deterministic and repeatable,
 * and depends on no optional munit features.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>

#include "munit.h"
#include "types.h"
#include "mvspww.h"    /* pending_member_t (spill_fp / spill_size) */
#include "mvsspl.h"

/* Big enough to exceed any plausible JCC stream buffer and span many blocks. */
#define REF_MAX   (256 * 1024)
#define SPL_BLK   4096

static pending_member_t g_pm;
static uint8_t          g_ref[REF_MAX];
static uint32_t         g_ref_size;

/* ==================================================================== */
/* Reference-model helpers                                              */
/* ==================================================================== */

/* A small self-contained LCG, so the tests are reproducible and depend on no
   optional munit features.  Reset per test (spl_begin) for determinism. */
static uint32_t g_rng;

static uint32_t trand(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return g_rng;
}

/* A value in [0, n).  n == 0 yields 0. */
static uint32_t trand_below(uint32_t n)
{
    return (n == 0) ? 0u : (trand() % n);
}

/* Fill 'len' bytes of 'buf' with reproducible pseudo-random data. */
static void fill(uint8_t *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        buf[i] = (uint8_t)(trand() >> 17);
}

/* Open a fresh scratch on 'slot' and reset the reference. */
static void spl_begin(int slot)
{
    memset(&g_pm, 0, sizeof(g_pm));
    strcpy(g_pm.dsname_ebcdic, "TEST.SPILL.DATA");
    strcpy(g_pm.member_name,   "SPLTEST");
    memset(g_ref, 0, sizeof(g_ref));
    g_ref_size = 0;
    g_rng      = 0x2545F491u;   /* fixed seed -> reproducible test data */

    munit_assert_int(spill_open(&g_pm, slot), ==, 0);
    munit_assert_ptr_not_null(g_pm.spill_fp);
    munit_assert_int((int)g_pm.spill_size, ==, 0);
}

static void spl_end(void)
{
    spill_close(&g_pm);
    munit_assert_ptr_null(g_pm.spill_fp);
}

/* Apply one write to BOTH the spill and the reference, and check the size
   invariant (spill_size must track the logical high-water). */
static void do_write(uint32_t off, const uint8_t *data, uint32_t len)
{
    munit_assert_int((int)((uint64_t)off + len <= REF_MAX), ==, 1);

    /* reference: a gap ahead of 'off' becomes zeros, then the data lands. */
    if (off > g_ref_size)
        memset(g_ref + g_ref_size, 0, (size_t)(off - g_ref_size));
    if (len > 0)
        memcpy(g_ref + off, data, (size_t)len);
    if (off + len > g_ref_size)
        g_ref_size = off + len;

    munit_assert_int(spill_write(&g_pm, off, data, len), ==, 0);
    munit_assert_int((int)g_pm.spill_size, ==, (int)g_ref_size);
}

/* Read [off,off+len) from the spill and compare to the reference. */
static void check_range(uint32_t off, uint32_t len)
{
    uint8_t buf[SPL_BLK];
    uint32_t done;
    uint32_t n;

    for (done = 0; done < len; done += n) {
        n = len - done;
        if (n > sizeof(buf))
            n = sizeof(buf);
        munit_assert_int(spill_read(&g_pm, off + done, buf, n), ==, 0);
        munit_assert_memory_equal(n, buf, g_ref + off + done);
    }
}

/* Read the entire spill and compare to the reference. */
static void verify_all(void)
{
    munit_assert_int((int)g_pm.spill_size, ==, (int)g_ref_size);
    if (g_ref_size > 0)
        check_range(0, g_ref_size);
}

/* ==================================================================== */
/* Tests                                                                */
/* ==================================================================== */

/* open sets the handle + size; close clears them. */
static MunitResult test_open_close(const MunitParameter p[], void *d)
{
    (void)p; (void)d;
    spl_begin(0);
    munit_assert_ptr_not_null(g_pm.spill_fp);
    spl_end();
    munit_assert_ptr_null(g_pm.spill_fp);
    munit_assert_int((int)g_pm.spill_size, ==, 0);
    return MUNIT_OK;
}

/* A small write, read straight back. */
static MunitResult test_basic(const MunitParameter p[], void *d)
{
    uint8_t data[100];
    (void)p; (void)d;
    spl_begin(0);
    fill(data, sizeof(data));
    do_write(0, data, sizeof(data));
    verify_all();
    spl_end();
    return MUNIT_OK;
}

/* Sequential, block-aligned appends (the easy case: all new blocks). */
static MunitResult test_seq_aligned(const MunitParameter p[], void *d)
{
    uint8_t blk[SPL_BLK];
    int i;
    (void)p; (void)d;
    spl_begin(0);
    for (i = 0; i < 40; i++) {           /* 40 * 4 KB = 160 KB */
        fill(blk, sizeof(blk));
        do_write((uint32_t)i * SPL_BLK, blk, sizeof(blk));
    }
    verify_all();
    spl_end();
    return MUNIT_OK;
}

/* Sequential 81-byte "lines" (80 chars + LF): the FB/80 copy pattern that
   corrupted on MVS.  Every append after the first lands mid-block, so each one
   is a partial-block read/modify/write -- exactly the stressed path. */
static MunitResult test_fb80_lines(const MunitParameter p[], void *d)
{
    uint8_t line[81];
    int n;
    int i;
    (void)p; (void)d;
    spl_begin(0);
    for (n = 0; n < 2000; n++) {         /* 2000 * 81 = 162000 bytes */
        for (i = 0; i < 80; i++)
            line[i] = (uint8_t)('0' + ((n + i) % 10));
        line[80] = 0x0A;                 /* LF */
        do_write((uint32_t)n * 81, line, 81);
    }
    verify_all();
    /* Spot-check line 160 -- where the corruption first appeared. */
    check_range(160u * 81u, 81);
    spl_end();
    return MUNIT_OK;
}

/* One big write (spills all at once), then overwrite a few bytes inside a block
   well away from the ends -- the partial-block RMW that dropped writes. */
static MunitResult test_overwrite_mid(const MunitParameter p[], void *d)
{
    static uint8_t big[60000];
    uint8_t patch[10];
    (void)p; (void)d;
    spl_begin(0);
    fill(big, sizeof(big));
    do_write(0, big, sizeof(big));

    fill(patch, sizeof(patch));
    do_write(50, patch, sizeof(patch));          /* block 0            */
    fill(patch, sizeof(patch));
    do_write(12904, patch, sizeof(patch));       /* block 3, mid-block */
    fill(patch, sizeof(patch));
    do_write(40000, patch, sizeof(patch));       /* a later block      */
    verify_all();
    spl_end();
    return MUNIT_OK;
}

/* Overwrite spanning several blocks (RMW of the two partial end blocks plus
   full middle blocks). */
static MunitResult test_overwrite_span(const MunitParameter p[], void *d)
{
    static uint8_t big[60000];
    static uint8_t span[10000];
    (void)p; (void)d;
    spl_begin(0);
    fill(big, sizeof(big));
    do_write(0, big, sizeof(big));
    fill(span, sizeof(span));
    do_write(3000, span, sizeof(span));          /* 3000..13000 */
    verify_all();
    spl_end();
    return MUNIT_OK;
}

/* Out-of-order: a high block first (creating a hole), then fill lower blocks. */
static MunitResult test_out_of_order(const MunitParameter p[], void *d)
{
    uint8_t blk[SPL_BLK];
    (void)p; (void)d;
    spl_begin(0);
    fill(blk, sizeof(blk));
    do_write(8u * SPL_BLK, blk, sizeof(blk));     /* block 8 (hole 0..8)  */
    fill(blk, sizeof(blk));
    do_write(0, blk, sizeof(blk));                /* block 0              */
    fill(blk, sizeof(blk));
    do_write(4u * SPL_BLK, blk, sizeof(blk));     /* block 4 (mid-hole)   */
    verify_all();                                 /* holes must be zeros  */
    spl_end();
    return MUNIT_OK;
}

/* A hole left by a far write must read back as zeros. */
static MunitResult test_hole_zeros(const MunitParameter p[], void *d)
{
    uint8_t a[16];
    uint8_t b[16];
    (void)p; (void)d;
    spl_begin(0);
    fill(a, sizeof(a));
    do_write(0, a, sizeof(a));
    fill(b, sizeof(b));
    do_write(50000, b, sizeof(b));                /* huge hole 16..50000 */
    verify_all();
    spl_end();
    return MUNIT_OK;
}

/* len == 0 is a pure zero-extend to 'off'. */
static MunitResult test_zero_extend(const MunitParameter p[], void *d)
{
    uint8_t a[100];
    (void)p; (void)d;
    spl_begin(0);
    fill(a, sizeof(a));
    do_write(0, a, sizeof(a));
    do_write(20000, NULL, 0);                     /* extend to 20000, zeros */
    munit_assert_int((int)g_pm.spill_size, ==, 20000);
    verify_all();
    do_write(10000, NULL, 0);                     /* off < size: no-op      */
    munit_assert_int((int)g_pm.spill_size, ==, 20000);
    verify_all();
    spl_end();
    return MUNIT_OK;
}

/* Block-boundary edges: writes that end exactly on, start exactly on, and
   straddle 4 KB boundaries, plus single bytes at the seams. */
static MunitResult test_boundaries(const MunitParameter p[], void *d)
{
    static uint8_t buf[SPL_BLK * 2];
    uint8_t one[1];
    (void)p; (void)d;
    spl_begin(0);
    fill(buf, SPL_BLK);
    do_write(0, buf, SPL_BLK);                    /* [0, 4096)  ends on edge */
    fill(buf, SPL_BLK);
    do_write(SPL_BLK, buf, SPL_BLK);              /* [4096, 8192) starts edge */
    fill(buf, 2);
    do_write(SPL_BLK - 1, buf, 2);                /* straddle 4095..4097     */
    fill(one, 1);
    do_write(SPL_BLK, one, 1);                    /* single byte on the edge */
    fill(one, 1);
    do_write(8192 - 1, one, 1);                   /* last byte of block 1    */
    verify_all();
    spl_end();
    return MUNIT_OK;
}

/* Interleave writes and reads with backward seeks: write block i, read it, then
   re-read block 0 -- the write->read->read-elsewhere pattern JCC mishandles. */
static MunitResult test_interleave(const MunitParameter p[], void *d)
{
    static uint8_t blk[SPL_BLK];
    int i;
    (void)p; (void)d;
    spl_begin(0);
    for (i = 0; i < 40; i++) {
        fill(blk, sizeof(blk));
        do_write((uint32_t)i * SPL_BLK, blk, sizeof(blk));
        check_range((uint32_t)i * SPL_BLK, SPL_BLK);  /* just-written block  */
        check_range(0, SPL_BLK);                       /* block 0 (backward)  */
    }
    verify_all();
    spl_end();
    return MUNIT_OK;
}

/* Read a written file back in a scrambled order (many backward/forward seeks). */
static MunitResult test_random_reads(const MunitParameter p[], void *d)
{
    static uint8_t big[150000];
    int i;
    (void)p; (void)d;
    spl_begin(0);
    fill(big, sizeof(big));
    do_write(0, big, sizeof(big));
    for (i = 0; i < 400; i++) {
        uint32_t len = 1u + trand_below(5000);              /* 1..5000        */
        uint32_t off = trand_below((uint32_t)sizeof(big) - len + 1);
        check_range(off, len);
    }
    spl_end();
    return MUNIT_OK;
}

/* Reuse: a slot's scratch is reopened for a new, SMALLER member -- the "w+b"
   reopen must truncate so no stale bytes from the big one leak through. */
static MunitResult test_reuse_truncates(const MunitParameter p[], void *d)
{
    static uint8_t big[120000];
    uint8_t small[3000];
    (void)p; (void)d;

    spl_begin(0);
    fill(big, sizeof(big));
    do_write(0, big, sizeof(big));
    verify_all();
    spl_end();                                   /* close, leaving the scratch */

    /* Reopen the SAME slot for a smaller member. */
    spl_begin(0);                                /* resets g_ref to empty      */
    fill(small, sizeof(small));
    do_write(0, small, sizeof(small));
    munit_assert_int((int)g_pm.spill_size, ==, (int)sizeof(small));
    verify_all();                                /* must be ONLY the small data */
    spl_end();
    return MUNIT_OK;
}

/* The big one: many random writes (overwrites, holes, extends, len 0), then a
   full byte-for-byte verify.  Reproducible via --seed. */
static MunitResult test_fuzz(const MunitParameter p[], void *d)
{
    static uint8_t data[8192];
    int iter;
    (void)p; (void)d;
    /* Each overwrite reaching below the highest block reopens the scratch on
       MVS, so keep the count modest -- still a thorough mix of overwrites,
       holes, extends and len-0 zero-extends. */
    spl_begin(0);
    for (iter = 0; iter < 400; iter++) {
        uint32_t len = trand_below(8193);                   /* 0..8192        */
        uint32_t off = trand_below(REF_MAX - 8192 + 1);     /* 0..REF_MAX-8192 */
        fill(data, len);
        do_write(off, data, len);
    }
    verify_all();
    spl_end();
    return MUNIT_OK;
}

/* ==================================================================== */
/* Suite registration                                                   */
/* ==================================================================== */

static MunitTest spill_tests[] = {
    { "/open_close",      test_open_close,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/basic",           test_basic,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/seq_aligned",     test_seq_aligned,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/fb80_lines",      test_fb80_lines,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/overwrite_mid",   test_overwrite_mid,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/overwrite_span",  test_overwrite_span,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/out_of_order",    test_out_of_order,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/hole_zeros",      test_hole_zeros,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/zero_extend",     test_zero_extend,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/boundaries",      test_boundaries,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/interleave",      test_interleave,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/random_reads",    test_random_reads,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/reuse_truncates", test_reuse_truncates, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/fuzz",            test_fuzz,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite tmvsspl_suite = {
    "/mvsspl",
    spill_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};
