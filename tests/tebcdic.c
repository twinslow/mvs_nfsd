/*
 * tests/tebcdic.c - Unit tests for ebcdic.c (CP037 EBCDIC <-> ASCII).
 *
 * Suite prefix: /ebcdic
 * Sub-suites  : /tables /buffer /member
 *
 * The translation tables and buffer translators are byte->byte maps that are
 * independent of the host's own character set, so every assertion here uses
 * NUMERIC byte values (0x41, not 'A').  That is essential: under JCC a char
 * literal is EBCDIC, so 'A' would be 0xC1 on MVS and 0x41 on the dev host --
 * using numbers keeps the tests identical and correct on both.
 *
 * Covered: the key CP037 mappings both ways, the unmapped-byte fallbacks
 * (EBCDIC->ASCII 0x1A, ASCII->EBCDIC 0x3F, 0xFF pass-through), the ASCII
 * round-trip over the letters/digits/space that MVS names use, the buffer
 * translators (incl. in-place and zero length), the single-char inline helpers,
 * and ebcdic_member_to_name.
 *
 * name_to_ebcdic_member: only its RETURN value (validation) is checked, not the
 * bytes it writes.  It runs the input through the EBCDIC-native ctype toupper()
 * and then the ASCII-indexed table, so the OUTPUT bytes diverge between the dev
 * host and MVS; the accept/reject decision, however, is the same on both.  (Both
 * member helpers are currently unused by the server -- pinned here for safety.)
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>

#include "munit.h"
#include "ebcdic.h"   /* tables, *_c helpers, buffer + member translators */

/* ==================================================================== */
/* /tables -- the CP037 translation tables and single-char helpers      */
/* ==================================================================== */

/* Representative EBCDIC -> ASCII mappings (CP037). */
static MunitResult test_ebc_to_asc_key(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int((int)ebcdic_to_ascii_tab[0x40], ==, 0x20);  /* space    */
    munit_assert_int((int)ebcdic_to_ascii_tab[0xC1], ==, 0x41);  /* A        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0xC9], ==, 0x49);  /* I        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0xD1], ==, 0x4A);  /* J        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0xE2], ==, 0x53);  /* S        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0xE9], ==, 0x5A);  /* Z        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0x81], ==, 0x61);  /* a        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0xF0], ==, 0x30);  /* 0        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0xF9], ==, 0x39);  /* 9        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0x4B], ==, 0x2E);  /* .        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0x7B], ==, 0x23);  /* #        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0x7C], ==, 0x40);  /* @        */
    munit_assert_int((int)ebcdic_to_ascii_tab[0x15], ==, 0x0A);  /* NL -> LF */
    munit_assert_int((int)ebcdic_to_ascii_tab[0x25], ==, 0x0A);  /* LF -> LF */
    munit_assert_int((int)ebcdic_to_ascii_tab[0x0D], ==, 0x0D);  /* CR       */
    munit_assert_int((int)ebcdic_to_ascii_tab[0x05], ==, 0x09);  /* HT -> TAB*/
    munit_assert_int((int)ebcdic_to_ascii_tab[0xFF], ==, 0xFF);  /* pass-thru*/
    return MUNIT_OK;
}

/* Representative ASCII -> EBCDIC mappings (CP037). */
static MunitResult test_asc_to_ebc_key(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int((int)ascii_to_ebcdic_tab[0x20], ==, 0x40);  /* space    */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x41], ==, 0xC1);  /* A        */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x5A], ==, 0xE9);  /* Z        */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x61], ==, 0x81);  /* a        */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x30], ==, 0xF0);  /* 0        */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x39], ==, 0xF9);  /* 9        */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x2E], ==, 0x4B);  /* .        */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x40], ==, 0x7C);  /* @        */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x0A], ==, 0x15);  /* LF -> NL */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x0D], ==, 0x0D);  /* CR       */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x09], ==, 0x05);  /* TAB -> HT*/
    munit_assert_int((int)ascii_to_ebcdic_tab[0xFF], ==, 0xFF);  /* pass-thru*/
    return MUNIT_OK;
}

/* Unmapped bytes take the documented fallbacks. */
static MunitResult test_unmapped(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* EBCDIC with no ASCII equivalent -> 0x1A (SUB). */
    munit_assert_int((int)ebcdic_to_ascii_tab[0x04], ==, 0x1A);
    munit_assert_int((int)ebcdic_to_ascii_tab[0x06], ==, 0x1A);
    munit_assert_int((int)ebcdic_to_ascii_tab[0x41], ==, 0x1A);

    /* ASCII 0x80..0xFE (no CP037 equivalent) -> 0x3F ('?'); 0xFF passes. */
    munit_assert_int((int)ascii_to_ebcdic_tab[0x80], ==, 0x3F);
    munit_assert_int((int)ascii_to_ebcdic_tab[0xA0], ==, 0x3F);
    munit_assert_int((int)ascii_to_ebcdic_tab[0xFE], ==, 0x3F);
    munit_assert_int((int)ascii_to_ebcdic_tab[0xFF], ==, 0xFF);
    return MUNIT_OK;
}

/* ASCII letters/digits/space survive ASCII -> EBCDIC -> ASCII unchanged. */
static MunitResult test_roundtrip_alnum(const MunitParameter params[], void *data)
{
    int a;
    (void)params; (void)data;

    for (a = 0x41; a <= 0x5A; a++)   /* A-Z */
        munit_assert_int((int)ebcdic_to_ascii_tab[ascii_to_ebcdic_tab[a]], ==, a);
    for (a = 0x61; a <= 0x7A; a++)   /* a-z */
        munit_assert_int((int)ebcdic_to_ascii_tab[ascii_to_ebcdic_tab[a]], ==, a);
    for (a = 0x30; a <= 0x39; a++)   /* 0-9 */
        munit_assert_int((int)ebcdic_to_ascii_tab[ascii_to_ebcdic_tab[a]], ==, a);
    munit_assert_int((int)ebcdic_to_ascii_tab[ascii_to_ebcdic_tab[0x20]], ==, 0x20);
    return MUNIT_OK;
}

/* The inline single-char helpers agree with the tables. */
static MunitResult test_char_helpers(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int((int)ascii_to_ebcdic_c(0x41), ==, 0xC1);  /* A  */
    munit_assert_int((int)ascii_to_ebcdic_c(0x2E), ==, 0x4B);  /* .  */
    munit_assert_int((int)ebcdic_to_ascii_c(0xC1), ==, 0x41);
    munit_assert_int((int)ebcdic_to_ascii_c(0x40), ==, 0x20);  /* sp */
    return MUNIT_OK;
}

static MunitTest table_tests[] = {
    { "/ebc_to_asc_key", test_ebc_to_asc_key, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/asc_to_ebc_key", test_asc_to_ebc_key, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/unmapped",       test_unmapped,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/roundtrip_alnum", test_roundtrip_alnum, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/char_helpers",   test_char_helpers,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /buffer -- ascii_to_ebcdic / ebcdic_to_ascii                        */
/* ==================================================================== */

static MunitResult test_ascii_to_ebcdic_buf(const MunitParameter params[], void *data)
{
    static const uint8_t src[5] = { 0x41, 0x42, 0x43, 0x20, 0x31 };  /* "ABC 1" */
    static const uint8_t exp[5] = { 0xC1, 0xC2, 0xC3, 0x40, 0xF1 };
    uint8_t dst[5];
    (void)params; (void)data;

    ascii_to_ebcdic(dst, src, 5);
    munit_assert_memory_equal(5, dst, exp);
    return MUNIT_OK;
}

static MunitResult test_ebcdic_to_ascii_buf(const MunitParameter params[], void *data)
{
    static const uint8_t src[5] = { 0xC1, 0xC2, 0xC3, 0x40, 0xF1 };
    static const uint8_t exp[5] = { 0x41, 0x42, 0x43, 0x20, 0x31 };
    uint8_t dst[5];
    (void)params; (void)data;

    ebcdic_to_ascii(dst, src, 5);
    munit_assert_memory_equal(5, dst, exp);
    return MUNIT_OK;
}

/* In-place translation is documented safe (dst == src). */
static MunitResult test_translate_in_place(const MunitParameter params[], void *data)
{
    static const uint8_t exp[3] = { 0x41, 0x42, 0x43 };
    uint8_t buf[3];
    (void)params; (void)data;

    buf[0] = 0xC1; buf[1] = 0xC2; buf[2] = 0xC3;
    ebcdic_to_ascii(buf, buf, 3);
    munit_assert_memory_equal(3, buf, exp);
    return MUNIT_OK;
}

/* Zero length leaves the destination untouched. */
static MunitResult test_translate_zero_len(const MunitParameter params[], void *data)
{
    uint8_t dst[4];
    (void)params; (void)data;

    memset(dst, 0x5A, sizeof(dst));
    ascii_to_ebcdic(dst, (const uint8_t *)"XXXX", 0);
    munit_assert_int((int)dst[0], ==, 0x5A);       /* sentinel intact */
    return MUNIT_OK;
}

static MunitTest buffer_tests[] = {
    { "/ascii_to_ebcdic", test_ascii_to_ebcdic_buf, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/ebcdic_to_ascii", test_ebcdic_to_ascii_buf, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/in_place",        test_translate_in_place,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/zero_len",        test_translate_zero_len,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /member -- ebcdic_member_to_name / name_to_ebcdic_member            */
/* ==================================================================== */

/* Trailing EBCDIC spaces (0x40) are stripped; the rest is translated. */
static MunitResult test_member_to_name_padded(const MunitParameter params[], void *data)
{
    uint8_t member8[8];
    char    name[9];
    int     n;
    (void)params; (void)data;

    member8[0] = 0xC1; member8[1] = 0xC2; member8[2] = 0xC3;   /* ABC */
    member8[3] = 0x40; member8[4] = 0x40; member8[5] = 0x40;
    member8[6] = 0x40; member8[7] = 0x40;

    n = ebcdic_member_to_name(name, member8);
    munit_assert_int(n, ==, 3);
    munit_assert_int((int)(uint8_t)name[0], ==, 0x41);
    munit_assert_int((int)(uint8_t)name[1], ==, 0x42);
    munit_assert_int((int)(uint8_t)name[2], ==, 0x43);
    munit_assert_int((int)(uint8_t)name[3], ==, 0x00);         /* NUL terminated */
    return MUNIT_OK;
}

/* A full 8-character member: no stripping. */
static MunitResult test_member_to_name_full(const MunitParameter params[], void *data)
{
    static const uint8_t member8[8] =
        { 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8 };    /* ABCDEFGH */
    char name[9];
    int  n;
    (void)params; (void)data;

    n = ebcdic_member_to_name(name, member8);
    munit_assert_int(n, ==, 8);
    munit_assert_int((int)(uint8_t)name[7], ==, 0x48);         /* H */
    munit_assert_int((int)(uint8_t)name[8], ==, 0x00);
    return MUNIT_OK;
}

/* An all-spaces member is the empty name. */
static MunitResult test_member_to_name_empty(const MunitParameter params[], void *data)
{
    uint8_t member8[8];
    char    name[9];
    int     n;
    (void)params; (void)data;

    memset(member8, 0x40, sizeof(member8));
    n = ebcdic_member_to_name(name, member8);
    munit_assert_int(n, ==, 0);
    munit_assert_int((int)(uint8_t)name[0], ==, 0x00);
    return MUNIT_OK;
}

/* name_to_ebcdic_member: accept/reject decision (return value only -- see the
   file header for why the output bytes are not checked). */
static MunitResult test_name_to_member_valid(const MunitParameter params[], void *data)
{
    uint8_t member8[8];
    (void)params; (void)data;

    munit_assert_int(name_to_ebcdic_member(member8, "ABC"),      ==, 0);
    munit_assert_int(name_to_ebcdic_member(member8, "A1@"),      ==, 0);
    munit_assert_int(name_to_ebcdic_member(member8, "ABCDEFGH"), ==, 0);  /* 8 = max */
    return MUNIT_OK;
}

static MunitResult test_name_to_member_rejects(const MunitParameter params[], void *data)
{
    uint8_t member8[8];
    (void)params; (void)data;

    munit_assert_int(name_to_ebcdic_member(member8, ""),          ==, -1); /* empty     */
    munit_assert_int(name_to_ebcdic_member(member8, "ABCDEFGHI"), ==, -1); /* 9 > 8     */
    munit_assert_int(name_to_ebcdic_member(member8, "A-B"),       ==, -1); /* bad char  */
    return MUNIT_OK;
}

static MunitTest member_tests[] = {
    { "/to_name_padded", test_member_to_name_padded, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/to_name_full",   test_member_to_name_full,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/to_name_empty",  test_member_to_name_empty,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/name_valid",     test_name_to_member_valid,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/name_rejects",   test_name_to_member_rejects, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/tables", table_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/buffer", buffer_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/member", member_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tebcdic_suite = {
    "/ebcdic",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
