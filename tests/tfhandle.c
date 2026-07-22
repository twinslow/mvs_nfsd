/*
 * tests/tfhandle.c - Unit tests for fhandle.c: the NFS file-handle WIRE FORMAT
 * (fh_encode / fh_decode).
 *
 * Suite prefix: /fhandle
 * Sub-suites  : /encode /decode /roundtrip
 *
 * These two functions are pure byte packing/unpacking of the 60-byte handle
 * (magic + export_id big-endian, then a 44-byte dsname and 8-byte member, each
 * ASCII and blank-padded with FH_PAD_CHAR 0x20).  They carry no I/O and no
 * export-table dependency, so they run identically on the dev host and MVS.
 * The self-describing handle is what makes a handle survive a server restart,
 * and a garbled encode/decode was a suspect in the write-path corruption work,
 * so the round-trip and the exact byte layout are both pinned here.
 *
 * NOT tested here: fh_from_path() / fh_resolve().  They take a genuine ASCII
 * path / an ASCII-field handle and walk the export table, and driving them
 * correctly under JCC (where char literals are EBCDIC, but the handle fields
 * are ASCII) needs hand-built ASCII inputs -- deferred to a later integration
 * pass.  Linking fhandle.o still pulls their dependencies in (mvsfid.o and the
 * export-table stubs in tstubs.c), which is why those are in the build.
 *
 * Encoding note: for the STRING fields the tests use native char literals for
 * both the input and the expectation, so a byte written as (uint8_t)'A' is
 * compared against the same native 'A' -- correct on both the ASCII dev host
 * and EBCDIC MVS.  The magic/export_id bytes and the pad byte (FH_PAD_CHAR) are
 * fixed numeric values, so they are asserted numerically.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>

#include "munit.h"
#include "nfsd.h"     /* our_fhandle_t, OUR_FH*, FH_* constants + fh_* protos */

static int u32_eq(uint32_t a, uint32_t b) { return a == b ? 1 : 0; }

/* ==================================================================== */
/* /encode -- fh_encode                                                 */
/* ==================================================================== */

/* magic and export_id occupy bytes 0-7, big-endian. */
static MunitResult test_encode_magic_id(const MunitParameter params[], void *data)
{
    our_fhandle_t fh;
    uint8_t       b[OUR_FHSIZE];
    (void)params; (void)data;

    memset(&fh, 0, sizeof(fh));
    fh.magic     = OUR_FH_MAGIC;         /* 0x4E465333 == 'NFS3' */
    fh.export_id = 0x11223344u;

    fh_encode(&fh, b);
    munit_assert_int((int)b[0], ==, 0x4E);
    munit_assert_int((int)b[1], ==, 0x46);
    munit_assert_int((int)b[2], ==, 0x53);
    munit_assert_int((int)b[3], ==, 0x33);
    munit_assert_int((int)b[4], ==, 0x11);
    munit_assert_int((int)b[5], ==, 0x22);
    munit_assert_int((int)b[6], ==, 0x33);
    munit_assert_int((int)b[7], ==, 0x44);
    return MUNIT_OK;
}

/* Short dsname/member are placed at their field offsets and the remainder is
   FH_PAD_CHAR (0x20). */
static MunitResult test_encode_fields_padded(const MunitParameter params[], void *data)
{
    our_fhandle_t fh;
    uint8_t       b[OUR_FHSIZE];
    (void)params; (void)data;

    memset(&fh, 0, sizeof(fh));
    fh.magic = OUR_FH_MAGIC;
    strcpy(fh.dsname, "ABC");            /* 3 chars into the 44-byte field */
    strcpy(fh.member, "M");             /* 1 char  into the 8-byte  field */

    fh_encode(&fh, b);
    /* dsname field: bytes 8..51 */
    munit_assert_int((int)b[8],  ==, (int)(uint8_t)'A');
    munit_assert_int((int)b[9],  ==, (int)(uint8_t)'B');
    munit_assert_int((int)b[10], ==, (int)(uint8_t)'C');
    munit_assert_int((int)b[11], ==, FH_PAD_CHAR);       /* padded from here */
    munit_assert_int((int)b[51], ==, FH_PAD_CHAR);
    /* member field: bytes 52..59 */
    munit_assert_int((int)b[52], ==, (int)(uint8_t)'M');
    munit_assert_int((int)b[53], ==, FH_PAD_CHAR);
    munit_assert_int((int)b[59], ==, FH_PAD_CHAR);
    return MUNIT_OK;
}

/* A dsname/member that exactly fills its field leaves no pad and does not spill
   into the neighbouring field. */
static MunitResult test_encode_full_fields(const MunitParameter params[], void *data)
{
    our_fhandle_t fh;
    uint8_t       b[OUR_FHSIZE];
    int           i;
    (void)params; (void)data;

    memset(&fh, 0, sizeof(fh));
    fh.magic = OUR_FH_MAGIC;
    for (i = 0; i < FH_DSNAME_LEN; i++) fh.dsname[i] = (char)'X';
    fh.dsname[FH_DSNAME_LEN] = '\0';
    for (i = 0; i < FH_MEMBER_LEN; i++) fh.member[i] = (char)'Y';
    fh.member[FH_MEMBER_LEN] = '\0';

    fh_encode(&fh, b);
    for (i = 0; i < FH_DSNAME_LEN; i++)
        munit_assert_int((int)b[8 + i], ==, (int)(uint8_t)'X');
    for (i = 0; i < FH_MEMBER_LEN; i++)
        munit_assert_int((int)b[8 + FH_DSNAME_LEN + i], ==, (int)(uint8_t)'Y');
    return MUNIT_OK;
}

static MunitTest encode_tests[] = {
    { "/magic_id",     test_encode_magic_id,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/fields_padded", test_encode_fields_padded, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/full_fields",  test_encode_full_fields,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /decode -- fh_decode                                                 */
/* ==================================================================== */

/* Trailing FH_PAD_CHAR is stripped, so the fields come back as C strings. */
static MunitResult test_decode_trims_pad(const MunitParameter params[], void *data)
{
    uint8_t       b[OUR_FHSIZE];
    our_fhandle_t fh;
    (void)params; (void)data;

    memset(b, FH_PAD_CHAR, sizeof(b));   /* every field pre-filled with pad */
    b[0] = 0x4E; b[1] = 0x46; b[2] = 0x53; b[3] = 0x33;   /* magic     */
    b[4] = 0; b[5] = 0; b[6] = 0; b[7] = 0x2A;            /* export_id 42 */
    b[8]  = (uint8_t)'A'; b[9] = (uint8_t)'B';            /* dsname "AB"  */
    b[52] = (uint8_t)'M';                                 /* member "M"   */

    munit_assert_int(fh_decode(b, (uint32_t)OUR_FHSIZE, &fh), ==, 0);
    munit_assert_int(u32_eq(fh.magic, OUR_FH_MAGIC), ==, 1);
    munit_assert_int(u32_eq(fh.export_id, 42u), ==, 1);
    munit_assert_string_equal(fh.dsname, "AB");
    munit_assert_string_equal(fh.member, "M");
    return MUNIT_OK;
}

/* A wrong magic is rejected. */
static MunitResult test_decode_bad_magic(const MunitParameter params[], void *data)
{
    uint8_t       b[OUR_FHSIZE];
    our_fhandle_t fh;
    (void)params; (void)data;

    memset(b, FH_PAD_CHAR, sizeof(b));
    b[0] = 0xDE; b[1] = 0xAD; b[2] = 0xBE; b[3] = 0xEF;   /* not 'NFS3' */
    munit_assert_int(fh_decode(b, (uint32_t)OUR_FHSIZE, &fh), ==, -1);
    return MUNIT_OK;
}

/* A wrong length is rejected before anything else is read. */
static MunitResult test_decode_bad_length(const MunitParameter params[], void *data)
{
    uint8_t       b[OUR_FHSIZE];
    our_fhandle_t fh;
    (void)params; (void)data;

    memset(b, FH_PAD_CHAR, sizeof(b));
    b[0] = 0x4E; b[1] = 0x46; b[2] = 0x53; b[3] = 0x33;   /* valid magic */
    munit_assert_int(fh_decode(b, (uint32_t)(OUR_FHSIZE - 1), &fh), ==, -1);
    munit_assert_int(fh_decode(b, (uint32_t)(OUR_FHSIZE + 1), &fh), ==, -1);
    munit_assert_int(fh_decode(b, 0u, &fh), ==, -1);
    return MUNIT_OK;
}

static MunitTest decode_tests[] = {
    { "/trims_pad",  test_decode_trims_pad,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/bad_magic",  test_decode_bad_magic,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/bad_length", test_decode_bad_length, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /roundtrip -- fh_encode then fh_decode reproduces the handle          */
/* ==================================================================== */

/* Helper: encode fh then decode into out; assert both steps succeed. */
static void roundtrip(const our_fhandle_t *fh, our_fhandle_t *out)
{
    uint8_t b[OUR_FHSIZE];
    fh_encode(fh, b);
    munit_assert_int(fh_decode(b, (uint32_t)OUR_FHSIZE, out), ==, 0);
}

/* A full PDS member handle round-trips exactly. */
static MunitResult test_roundtrip_member(const MunitParameter params[], void *data)
{
    our_fhandle_t fh, out;
    (void)params; (void)data;

    memset(&fh, 0, sizeof(fh));
    fh.magic     = OUR_FH_MAGIC;
    fh.export_id = 0xABCDEF01u;
    strcpy(fh.dsname, "TEMP.TESTPROJ.CNTL");
    strcpy(fh.member, "HRTPLOAD");         /* exactly 8 chars */

    roundtrip(&fh, &out);
    munit_assert_int(u32_eq(out.magic, fh.magic), ==, 1);
    munit_assert_int(u32_eq(out.export_id, fh.export_id), ==, 1);
    munit_assert_string_equal(out.dsname, "TEMP.TESTPROJ.CNTL");
    munit_assert_string_equal(out.member, "HRTPLOAD");
    return MUNIT_OK;
}

/* A PDS directory handle: dsname set, member empty. */
static MunitResult test_roundtrip_dir(const MunitParameter params[], void *data)
{
    our_fhandle_t fh, out;
    (void)params; (void)data;

    memset(&fh, 0, sizeof(fh));
    fh.magic     = OUR_FH_MAGIC;
    fh.export_id = 7u;
    strcpy(fh.dsname, "SYS1.MACLIB");
    /* member left "" */

    roundtrip(&fh, &out);
    munit_assert_string_equal(out.dsname, "SYS1.MACLIB");
    munit_assert_string_equal(out.member, "");
    return MUNIT_OK;
}

/* The export-root handle: both name fields empty. */
static MunitResult test_roundtrip_root(const MunitParameter params[], void *data)
{
    our_fhandle_t fh, out;
    (void)params; (void)data;

    memset(&fh, 0, sizeof(fh));
    fh.magic     = OUR_FH_MAGIC;
    fh.export_id = 0xFFFFFFFFu;
    /* dsname and member both "" */

    roundtrip(&fh, &out);
    munit_assert_int(u32_eq(out.export_id, 0xFFFFFFFFu), ==, 1);
    munit_assert_string_equal(out.dsname, "");
    munit_assert_string_equal(out.member, "");
    return MUNIT_OK;
}

static MunitTest roundtrip_tests[] = {
    { "/member", test_roundtrip_member, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/dir",    test_roundtrip_dir,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/root",   test_roundtrip_root,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/encode",    encode_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/decode",    decode_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/roundtrip", roundtrip_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tfhandle_suite = {
    "/fhandle",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
