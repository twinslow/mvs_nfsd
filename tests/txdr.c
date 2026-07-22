/*
 * tests/txdr.c - Unit tests for xdr.c (XDR encode/decode primitives).
 *
 * Suite prefix: /xdr
 * Sub-suites  : /init /u32 /u64 /raw /opaque /string /skip /pos /errors /fhandle
 *
 * XDR is the wire serialisation under every RPC/NFS message, so the two
 * properties that matter are checked directly:
 *   1. Byte layout -- scalars are big-endian, 4-byte aligned; uint64 is two
 *      words high-first; opaque/string are length + data + zero-pad to 4.
 *      These are verified at the BYTE level (each byte is 0..255, so the
 *      assertions are exact and endianness-independent).
 *   2. Bounds -- every read/write is capacity-checked, sets x->error on
 *      overflow, and the error LATCHES so later operations are no-ops.
 *
 * All operations run over plain in-memory buffers, so there is no platform
 * dependency: the suite behaves identically on the Linux dev host and on MVS.
 *
 * The prototypes (and the JCC 8-char external-name aliases) for xdr live in
 * nfsd.h, so this file includes it rather than a dedicated xdr.h.
 *
 * munit note: munit has no uint32/uint64 assertion and munit_assert_int
 * narrows to int, so full-width values (e.g. 0xFFFFFFFF) are compared in C via
 * the u32_eq/u64_eq helpers and the boolean is asserted.  Byte-level checks use
 * munit_assert_int directly (bytes fit in int).
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>

#include "munit.h"
#include "nfsd.h"     /* xdr_t, xdr_* prototypes + JCC aliases, our_fhandle_t */

/* xdr_write_fhandle() calls the real fh_encode() from fhandle.c, which is now
   linked into the test build (see tfhandle.c). */

/* ------------------------------------------------------------------ */
/* JCC-safe full-width equality (see munit note in the file header).    */
/* ------------------------------------------------------------------ */
static int u32_eq(uint32_t a, uint32_t b) { return a == b ? 1 : 0; }
static int u64_eq(uint64_t a, uint64_t b) { return a == b ? 1 : 0; }

/* ==================================================================== */
/* /init -- xdr_init_read / xdr_init_write                              */
/* ==================================================================== */

static MunitResult test_init_read(const MunitParameter params[], void *data)
{
    uint8_t buf[16];
    xdr_t   x;
    (void)params; (void)data;

    memset(&x, 0xFF, sizeof(x));         /* poison, so init must clear it */
    xdr_init_read(&x, buf, sizeof(buf));

    munit_assert_ptr_equal(x.base, buf);
    munit_assert_int((int)x.capacity, ==, (int)sizeof(buf));
    munit_assert_int((int)x.pos,      ==, 0);
    munit_assert_int(x.error,         ==, 0);
    munit_assert_int((int)xdr_get_pos(&x), ==, 0);
    return MUNIT_OK;
}

static MunitResult test_init_write(const MunitParameter params[], void *data)
{
    uint8_t buf[32];
    xdr_t   x;
    (void)params; (void)data;

    memset(&x, 0xFF, sizeof(x));
    xdr_init_write(&x, buf, sizeof(buf));

    munit_assert_ptr_equal(x.base, buf);
    munit_assert_int((int)x.capacity, ==, (int)sizeof(buf));
    munit_assert_int((int)x.pos,      ==, 0);
    munit_assert_int(x.error,         ==, 0);
    return MUNIT_OK;
}

static MunitTest init_tests[] = {
    { "/read",  test_init_read,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/write", test_init_write, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /u32 -- xdr_write_uint32 / xdr_read_uint32                           */
/* ==================================================================== */

/* Big-endian byte layout on write. */
static MunitResult test_u32_write_layout(const MunitParameter params[], void *data)
{
    uint8_t buf[8];
    xdr_t   x;
    (void)params; (void)data;

    memset(buf, 0xAA, sizeof(buf));
    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_uint32(&x, 0x01020304u);

    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 4);
    munit_assert_int((int)buf[0], ==, 0x01);
    munit_assert_int((int)buf[1], ==, 0x02);
    munit_assert_int((int)buf[2], ==, 0x03);
    munit_assert_int((int)buf[3], ==, 0x04);
    return MUNIT_OK;
}

/* Big-endian assembly on read (decomposed to bytes to stay int-safe). */
static MunitResult test_u32_read_layout(const MunitParameter params[], void *data)
{
    uint8_t  buf[4];
    xdr_t    x;
    uint32_t v;
    (void)params; (void)data;

    buf[0] = 0xDE; buf[1] = 0xAD; buf[2] = 0xBE; buf[3] = 0xEF;
    xdr_init_read(&x, buf, sizeof(buf));
    v = xdr_read_uint32(&x);

    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 4);
    munit_assert_int((int)((v >> 24) & 0xFFu), ==, 0xDE);
    munit_assert_int((int)((v >> 16) & 0xFFu), ==, 0xAD);
    munit_assert_int((int)((v >>  8) & 0xFFu), ==, 0xBE);
    munit_assert_int((int)( v        & 0xFFu), ==, 0xEF);
    return MUNIT_OK;
}

/* Round-trip boundary values through write -> read. */
static MunitResult test_u32_roundtrip(const MunitParameter params[], void *data)
{
    static const uint32_t vals[5] = {
        0x00000000u, 0x00000001u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu
    };
    uint8_t buf[64];
    xdr_t   xw, xr;
    int     i;
    (void)params; (void)data;

    xdr_init_write(&xw, buf, sizeof(buf));
    for (i = 0; i < 5; i++)
        xdr_write_uint32(&xw, vals[i]);
    munit_assert_int(xw.error,    ==, 0);
    munit_assert_int((int)xw.pos, ==, 20);

    xdr_init_read(&xr, buf, sizeof(buf));
    for (i = 0; i < 5; i++)
        munit_assert_int(u32_eq(xdr_read_uint32(&xr), vals[i]), ==, 1);
    munit_assert_int(xr.error,    ==, 0);
    munit_assert_int((int)xr.pos, ==, 20);
    return MUNIT_OK;
}

static MunitTest u32_tests[] = {
    { "/write_layout", test_u32_write_layout, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/read_layout",  test_u32_read_layout,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/roundtrip",    test_u32_roundtrip,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /u64 -- xdr_write_uint64 / xdr_read_uint64 (high word first)         */
/* ==================================================================== */

static MunitResult test_u64_write_layout(const MunitParameter params[], void *data)
{
    uint8_t buf[8];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_uint64(&x, 0x0102030405060708ull);

    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 8);
    munit_assert_int((int)buf[0], ==, 0x01);   /* high word first */
    munit_assert_int((int)buf[1], ==, 0x02);
    munit_assert_int((int)buf[2], ==, 0x03);
    munit_assert_int((int)buf[3], ==, 0x04);
    munit_assert_int((int)buf[4], ==, 0x05);
    munit_assert_int((int)buf[5], ==, 0x06);
    munit_assert_int((int)buf[6], ==, 0x07);
    munit_assert_int((int)buf[7], ==, 0x08);
    return MUNIT_OK;
}

/* The two 32-bit words must be read high-first: a value with a distinct hi
   and lo word catches any word swap. */
static MunitResult test_u64_word_order(const MunitParameter params[], void *data)
{
    uint8_t  buf[8];
    xdr_t    x;
    uint64_t v;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_uint64(&x, 0x11223344AABBCCDDull);

    xdr_init_read(&x, buf, sizeof(buf));
    v = xdr_read_uint64(&x);
    munit_assert_int(u32_eq((uint32_t)(v >> 32),         0x11223344u), ==, 1);
    munit_assert_int(u32_eq((uint32_t)(v & 0xFFFFFFFFu), 0xAABBCCDDu), ==, 1);
    return MUNIT_OK;
}

static MunitResult test_u64_roundtrip(const MunitParameter params[], void *data)
{
    static const uint64_t vals[4] = {
        0x0000000000000000ull, 0x0000000100000000ull,
        0x00000000FFFFFFFFull, 0xFFFFFFFFFFFFFFFFull
    };
    uint8_t buf[64];
    xdr_t   xw, xr;
    int     i;
    (void)params; (void)data;

    xdr_init_write(&xw, buf, sizeof(buf));
    for (i = 0; i < 4; i++)
        xdr_write_uint64(&xw, vals[i]);
    munit_assert_int((int)xw.pos, ==, 32);

    xdr_init_read(&xr, buf, sizeof(buf));
    for (i = 0; i < 4; i++)
        munit_assert_int(u64_eq(xdr_read_uint64(&xr), vals[i]), ==, 1);
    munit_assert_int(xr.error, ==, 0);
    return MUNIT_OK;
}

static MunitTest u64_tests[] = {
    { "/write_layout", test_u64_write_layout, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/word_order",   test_u64_word_order,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/roundtrip",    test_u64_roundtrip,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /raw -- xdr_write_raw / xdr_read_raw (no length prefix, no padding)  */
/* ==================================================================== */

static MunitResult test_raw_roundtrip(const MunitParameter params[], void *data)
{
    static const uint8_t src[5] = { 0x10, 0x20, 0x30, 0x40, 0x50 };
    uint8_t buf[8];
    uint8_t dst[5];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_raw(&x, src, 5);
    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 5);          /* exactly 5, NO padding */

    xdr_init_read(&x, buf, sizeof(buf));
    memset(dst, 0, sizeof(dst));
    xdr_read_raw(&x, dst, 5);
    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 5);
    munit_assert_memory_equal(5, dst, src);
    return MUNIT_OK;
}

/* raw does not pad, so a following write is byte-adjacent. */
static MunitResult test_raw_no_padding(const MunitParameter params[], void *data)
{
    static const uint8_t src[3] = { 0xAB, 0xCD, 0xEF };
    uint8_t buf[8];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_raw(&x, src, 3);
    munit_assert_int((int)x.pos, ==, 3);          /* not rounded up to 4 */
    xdr_write_raw(&x, src, 1);
    munit_assert_int((int)x.pos, ==, 4);
    munit_assert_int((int)buf[3], ==, 0xAB);
    return MUNIT_OK;
}

static MunitTest raw_tests[] = {
    { "/roundtrip",  test_raw_roundtrip,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/no_padding", test_raw_no_padding, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /opaque -- xdr_write_opaque / xdr_read_opaque (len + data + pad)     */
/* ==================================================================== */

/* len 5 -> [00 00 00 05][5 data][3 zero pad], total 12. */
static MunitResult test_opaque_write_pad(const MunitParameter params[], void *data)
{
    static const uint8_t src[5] = { 'h', 'e', 'l', 'l', 'o' };
    uint8_t buf[16];
    xdr_t   x;
    (void)params; (void)data;

    memset(buf, 0xFF, sizeof(buf));
    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_opaque(&x, src, 5);

    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 12);         /* 4 + 5 + 3 pad */
    munit_assert_int((int)buf[3], ==, 5);         /* length word   */
    munit_assert_memory_equal(5, buf + 4, src);
    munit_assert_int((int)buf[9],  ==, 0);        /* pad bytes zero */
    munit_assert_int((int)buf[10], ==, 0);
    munit_assert_int((int)buf[11], ==, 0);
    return MUNIT_OK;
}

/* len 4 is already aligned -> no padding, total 8. */
static MunitResult test_opaque_write_aligned(const MunitParameter params[], void *data)
{
    static const uint8_t src[4] = { 1, 2, 3, 4 };
    uint8_t buf[16];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_opaque(&x, src, 4);
    munit_assert_int((int)x.pos, ==, 8);
    return MUNIT_OK;
}

/* Read consumes the padding: dstlen is the true length, pos lands past pad. */
static MunitResult test_opaque_read(const MunitParameter params[], void *data)
{
    static const uint8_t src[5] = { 'w', 'o', 'r', 'l', 'd' };
    uint8_t  buf[16];
    uint8_t  dst[8];
    uint32_t dstlen;
    xdr_t    x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_opaque(&x, src, 5);

    xdr_init_read(&x, buf, sizeof(buf));
    dstlen = 0xFFFFFFFFu;
    memset(dst, 0, sizeof(dst));
    xdr_read_opaque(&x, dst, &dstlen, sizeof(dst));
    munit_assert_int(x.error,     ==, 0);
    munit_assert_int((int)dstlen, ==, 5);
    munit_assert_memory_equal(5, dst, src);
    munit_assert_int((int)x.pos,  ==, 12);        /* padding skipped */
    return MUNIT_OK;
}

/* A length exceeding maxlen is rejected (error set), no overrun. */
static MunitResult test_opaque_read_too_long(const MunitParameter params[], void *data)
{
    uint8_t  buf[16];
    uint8_t  dst[4];
    uint32_t dstlen;
    xdr_t    x;
    (void)params; (void)data;

    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 10;   /* claims len 10 */
    xdr_init_read(&x, buf, sizeof(buf));
    dstlen = 123;
    xdr_read_opaque(&x, dst, &dstlen, sizeof(dst));    /* maxlen 4 < 10 */
    munit_assert_int(x.error, ==, 1);
    return MUNIT_OK;
}

/* Zero-length opaque: just the length word, no data, no pad. */
static MunitResult test_opaque_zero_len(const MunitParameter params[], void *data)
{
    uint8_t  buf[8];
    uint8_t  dst[4];
    uint32_t dstlen;
    xdr_t    x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_opaque(&x, dst, 0);
    munit_assert_int((int)x.pos, ==, 4);

    xdr_init_read(&x, buf, sizeof(buf));
    dstlen = 99;
    xdr_read_opaque(&x, dst, &dstlen, sizeof(dst));
    munit_assert_int(x.error,     ==, 0);
    munit_assert_int((int)dstlen, ==, 0);
    munit_assert_int((int)x.pos,  ==, 4);
    return MUNIT_OK;
}

static MunitTest opaque_tests[] = {
    { "/write_pad",     test_opaque_write_pad,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/write_aligned", test_opaque_write_aligned, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/read",          test_opaque_read,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/read_too_long", test_opaque_read_too_long, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/zero_len",      test_opaque_zero_len,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /string -- xdr_write_string / xdr_read_string                       */
/* ==================================================================== */

static MunitResult test_string_roundtrip(const MunitParameter params[], void *data)
{
    uint8_t buf[16];
    char    dst[8];
    xdr_t   x;
    int     n;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_string(&x, "abc", 3);
    munit_assert_int((int)x.pos, ==, 8);          /* 4 + 3 + 1 pad */

    xdr_init_read(&x, buf, sizeof(buf));
    memset(dst, 0x7F, sizeof(dst));
    n = xdr_read_string(&x, dst, sizeof(dst));
    munit_assert_int(n, ==, 3);
    munit_assert_string_equal(dst, "abc");        /* NUL-terminated */
    munit_assert_int((int)x.pos, ==, 8);          /* padding skipped */
    return MUNIT_OK;
}

/* A string whose length needs >= maxlen (incl. the NUL) is rejected. */
static MunitResult test_string_read_too_long(const MunitParameter params[], void *data)
{
    uint8_t buf[16];
    char    dst[4];               /* holds at most 3 chars + NUL */
    xdr_t   x;
    int     n;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_string(&x, "toolong", 7);

    xdr_init_read(&x, buf, sizeof(buf));
    n = xdr_read_string(&x, dst, sizeof(dst));
    munit_assert_int(n,       ==, -1);
    munit_assert_int(x.error, ==, 1);
    return MUNIT_OK;
}

static MunitResult test_string_empty(const MunitParameter params[], void *data)
{
    uint8_t buf[8];
    char    dst[4];
    xdr_t   x;
    int     n;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_string(&x, "", 0);
    munit_assert_int((int)x.pos, ==, 4);

    xdr_init_read(&x, buf, sizeof(buf));
    memset(dst, 0x7F, sizeof(dst));
    n = xdr_read_string(&x, dst, sizeof(dst));
    munit_assert_int(n, ==, 0);
    munit_assert_string_equal(dst, "");
    return MUNIT_OK;
}

static MunitTest string_tests[] = {
    { "/roundtrip",    test_string_roundtrip,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/read_too_long", test_string_read_too_long, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/empty",        test_string_empty,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /skip -- xdr_skip                                                    */
/* ==================================================================== */

static MunitResult test_skip_advances(const MunitParameter params[], void *data)
{
    uint8_t  buf[16];
    xdr_t    x;
    (void)params; (void)data;

    memset(buf, 0, sizeof(buf));
    buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0x2A;  /* 42 at offset 8 */
    xdr_init_read(&x, buf, sizeof(buf));
    xdr_skip(&x, 8);
    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 8);
    munit_assert_int(u32_eq(xdr_read_uint32(&x), 42u), ==, 1);
    return MUNIT_OK;
}

static MunitResult test_skip_overflow(const MunitParameter params[], void *data)
{
    uint8_t buf[4];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_read(&x, buf, sizeof(buf));
    xdr_skip(&x, 5);                              /* past capacity */
    munit_assert_int(x.error, ==, 1);
    return MUNIT_OK;
}

static MunitTest skip_tests[] = {
    { "/advances", test_skip_advances, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/overflow", test_skip_overflow, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /pos -- xdr_get_pos / xdr_set_pos (READDIR backtracking)            */
/* ==================================================================== */

/* Write, remember the position, write more, then rewind and overwrite --
   the pattern READDIRPLUS uses to drop a half-built entry that overflows. */
static MunitResult test_pos_backtrack(const MunitParameter params[], void *data)
{
    uint8_t  buf[32];
    xdr_t    x;
    uint32_t mark;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_uint32(&x, 0xAAAAAAAAu);
    mark = xdr_get_pos(&x);                        /* == 4 */
    munit_assert_int((int)mark, ==, 4);
    xdr_write_uint32(&x, 0xBBBBBBBBu);             /* tentative entry */

    xdr_set_pos(&x, mark);                         /* roll it back */
    munit_assert_int((int)x.pos, ==, 4);
    xdr_write_uint32(&x, 0xCCCCCCCCu);             /* overwrite it */

    xdr_init_read(&x, buf, sizeof(buf));
    munit_assert_int(u32_eq(xdr_read_uint32(&x), 0xAAAAAAAAu), ==, 1);
    munit_assert_int(u32_eq(xdr_read_uint32(&x), 0xCCCCCCCCu), ==, 1);
    return MUNIT_OK;
}

/* set_pos to exactly capacity is allowed; beyond it sets error and does not
   move pos. */
static MunitResult test_pos_bounds(const MunitParameter params[], void *data)
{
    uint8_t buf[8];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_set_pos(&x, 8);                            /* == capacity: ok */
    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 8);

    xdr_set_pos(&x, 9);                            /* > capacity: rejected */
    munit_assert_int(x.error,    ==, 1);
    munit_assert_int((int)x.pos, ==, 8);           /* unchanged */
    return MUNIT_OK;
}

static MunitTest pos_tests[] = {
    { "/backtrack", test_pos_backtrack, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/bounds",    test_pos_bounds,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /errors -- capacity checks and error latching                        */
/* ==================================================================== */

/* Reading past the end sets error and returns 0; pos does not advance. */
static MunitResult test_read_overflow(const MunitParameter params[], void *data)
{
    uint8_t  buf[4];
    xdr_t    x;
    uint32_t v;
    (void)params; (void)data;

    xdr_init_read(&x, buf, sizeof(buf));
    (void)xdr_read_uint32(&x);                     /* consumes all 4 bytes */
    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 4);

    v = xdr_read_uint32(&x);                        /* nothing left */
    munit_assert_int(x.error,    ==, 1);
    munit_assert_int(u32_eq(v, 0u), ==, 1);
    munit_assert_int((int)x.pos, ==, 4);           /* not advanced */
    return MUNIT_OK;
}

/* Writing past the end sets error and leaves pos where it was. */
static MunitResult test_write_overflow(const MunitParameter params[], void *data)
{
    uint8_t buf[4];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_uint32(&x, 0x11223344u);             /* fills the buffer */
    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 4);

    xdr_write_uint32(&x, 0x55667788u);             /* no room */
    munit_assert_int(x.error,    ==, 1);
    munit_assert_int((int)x.pos, ==, 4);           /* unchanged */
    return MUNIT_OK;
}

/* Once error is set every later op is a no-op (a "sticky" failure flag), so a
   caller can check error once at the end instead of after each field. */
static MunitResult test_error_latches(const MunitParameter params[], void *data)
{
    uint8_t buf[16];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    x.error = 1;                                   /* force the failed state */

    xdr_write_uint32(&x, 0x12345678u);
    munit_assert_int((int)x.pos, ==, 0);           /* did nothing */
    xdr_write_uint64(&x, 0x1122334455667788ull);
    munit_assert_int((int)x.pos, ==, 0);

    /* Reads in the failed state return 0 without advancing. */
    munit_assert_int(u32_eq(xdr_read_uint32(&x), 0u), ==, 1);
    munit_assert_int(u64_eq(xdr_read_uint64(&x), 0u), ==, 1);
    munit_assert_int((int)x.pos, ==, 0);
    return MUNIT_OK;
}

static MunitTest error_tests[] = {
    { "/read_overflow",  test_read_overflow,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/write_overflow", test_write_overflow, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/latches",        test_error_latches,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /fhandle -- xdr_write_fhandle (framing over the real fh_encode)      */
/* ==================================================================== */

/* OUR_FHSIZE (60) is 4-byte aligned, so the wire form is [len=60][60 bytes]
   with no padding.  The 60 payload bytes must match fh_encode()'s output. */
static MunitResult test_fhandle_framing(const MunitParameter params[], void *data)
{
    our_fhandle_t fh;
    uint8_t       buf[128];
    uint8_t       expect[OUR_FHSIZE];
    xdr_t         x;
    (void)params; (void)data;

    memset(&fh, 0, sizeof(fh));
    fh.magic     = OUR_FH_MAGIC;
    fh.export_id = 0x01020304u;
    strcpy(fh.dsname, "TEMP.FOO");
    strcpy(fh.member, "BAR");
    fh_encode(&fh, expect);                        /* real fh_encode (fhandle.c) */

    xdr_init_write(&x, buf, sizeof(buf));
    xdr_write_fhandle(&x, &fh);

    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 4 + OUR_FHSIZE);
    munit_assert_int((int)buf[0], ==, 0);          /* length word == 60 */
    munit_assert_int((int)buf[1], ==, 0);
    munit_assert_int((int)buf[2], ==, 0);
    munit_assert_int((int)buf[3], ==, OUR_FHSIZE);
    munit_assert_memory_equal(OUR_FHSIZE, buf + 4, expect);
    return MUNIT_OK;
}

static MunitTest fhandle_tests[] = {
    { "/framing", test_fhandle_framing, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/init",    init_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/u32",     u32_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/u64",     u64_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/raw",     raw_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/opaque",  opaque_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/string",  string_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/skip",    skip_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/pos",     pos_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/errors",  error_tests,   NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/fhandle", fhandle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite txdr_suite = {
    "/xdr",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
