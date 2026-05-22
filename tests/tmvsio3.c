/*
 * tests/tmvsio3.c
 *
 * Unit tests for mvsio.c:
 *   mvs_extract_ispf_stats
 *   mvs_set_no_ispf_stats
 *   mvs_pds_member_entry_set
 *   mvs_skip_dir_entry
 *   mvs_process_dir_block
 *
 * None of these functions call exports_count() or exports_get(), so no
 * stub export table setup is required.
 *
 * JCC C89 compliance: all variable declarations precede executable
 * statements within each function body.  Only block comments are used.
 *
 * Endianness:
 *   PDS directory data originates on MVS (big-endian).  All multi-byte
 *   integer fields in the test buffers are therefore stored big-endian,
 *   which is what the production code reads natively on MVS.
 *
 *   On a little-endian development machine (Linux/x86) the production
 *   code performs the same native pointer casts and therefore reads the
 *   values in the opposite byte order.  To keep both build targets
 *   using the same expected values in assertions, every multi-byte
 *   integer sequence in the static test buffers is guarded with
 *   #ifdef __MVS__ so the bytes are laid out in native byte order for
 *   each platform.
 *
 *   Fields affected:
 *     - uint16_t: size, initSize, modCount halfwords in ISPF stats
 *     - uint16_t: TT (first_block_tt) in directory entries
 *     - uint16_t: 2-byte block-used count at the start of each dir block
 *     - int32_t:  extended-stats size, initSize, modCount
 */

#include "munit.h"
#include "mvsio.h"
#include <string.h>

#include "types.h"

/* -------------------------------------------------------------------- */
/* Forward declarations for internal mvsio.c functions not in mvsio.h  */
/* -------------------------------------------------------------------- */
void mvs_extract_ispf_stats(pds_member_entry_t *entry,
                             unsigned char *userdata, int user_data_len);
void mvs_set_no_ispf_stats(pds_member_entry_t *entry);
int  mvs_pds_member_entry_set(pds_member_entry_t *entry,
                               const uint8_t *start_blockptr);
int  mvs_skip_dir_entry(const uint8_t *start_blockptr);
int  mvs_process_dir_block(const uint8_t *block_data,
                            const char *start_member, int max_members,
                            pds_member_entry_t *member_entries,
                            int *num_members_returned, int *end_of_dir);

/* -------------------------------------------------------------------- */
/* Test data buffers                                                     */
/* -------------------------------------------------------------------- */

/*
 * 30-byte basic ISPF statistics block (no extended stats).
 *
 * Byte map (all offsets relative to start of buffer):
 *   [0]      ver  = 1
 *   [1]      mod  = 5
 *   [2]      flags = 0 (no extended stats)
 *   [3]      seconds (BCD 00 = 0)
 *   [4..7]   crdate: century=0x01 year=0x24 day-hi=0x00 day-lo=0x10
 *              => year 1900+100*1+24=2024, day 10*0+1=1
 *   [8..13]  chgdate: same date at [8..11], time 0x14 0x30 at [12..13]
 *   [14..15] size     = 100  (native-endian uint16)
 *   [16..17] initSize = 80   (native-endian uint16)
 *   [18..19] modCount = 10   (native-endian uint16)
 *   [20..27] user = "TONYW   " (3 trailing spaces trimmed to "TONYW")
 *   [28..29] reserved = 0
 *
 * Non-const: mvs_extract_ispf_stats takes unsigned char * (non-const).
 */
static unsigned char c_ispf_basic[30] = {
    0x01, 0x05, 0x00,              /* ver=1, mod=5, flags=0          */
    0x00,                           /* seconds (BCD 0)                */
    0x01, 0x24, 0x00, 0x10,        /* crdate: 2024-001               */
    0x01, 0x24, 0x00, 0x10,        /* chgdate date: 2024-001         */
    0x14, 0x30,                     /* chgdate time: 14:30            */
#ifdef __MVS__
    0x00, 0x64,                     /* size     = 100 (BE)            */
    0x00, 0x50,                     /* initSize = 80  (BE)            */
    0x00, 0x0A,                     /* modCount = 10  (BE)            */
#else
    0x64, 0x00,                     /* size     = 100 (LE)            */
    0x50, 0x00,                     /* initSize = 80  (LE)            */
    0x0A, 0x00,                     /* modCount = 10  (LE)            */
#endif
    'T',  'O',  'N',  'Y',  'W',
    ' ',  ' ',  ' ',               /* trailing spaces, trimmed        */
    0x00, 0x00                      /* reserved                       */
};

/*
 * 40-byte extended ISPF stats buffer.
 *
 * Flag byte [2] has MVS_PDSDIR_ISPF_EXT_STATS (0x04) set.  The extended
 * code path reads 4-byte ints from offsets 22, 26, and 30 relative to
 * the original userdata pointer.  The buffer is padded to 34 bytes so
 * the modCount read at [30..33] stays within bounds.
 *
 * Basic halfword size/initSize/modCount at [14..19] are overridden by the
 * extended 32-bit values.  Only size and initSize are asserted in tests
 * because modCount at [30..33] is an acknowledged overread.
 *
 * Extended values (native-endian int32):
 *   size     = 1000  at [22..25]
 *   initSize = 400   at [26..29]
 *   modCount = 10    at [30..33]  (padding only -- value not asserted)
 */
static unsigned char c_ispf_ext[40] = {
    0x01, 0x03, 0x04,              /* ver=1, mod=3, flags=0x04       */
    0x00,                           /* seconds                        */
    0x01, 0x24, 0x00, 0x10,        /* crdate: 2024-001               */
    0x01, 0x24, 0x00, 0x10,        /* chgdate date                   */
    0x00, 0x00,                     /* chgdate time                   */
#ifdef __MVS__
    0x00, 0x00, 0x03, 0xE8,        /* ext size     = 1000 at [14..17] (BE) */
    0x00, 0x00, 0x01, 0x90,        /* ext initSize = 400  at [18..21] (BE) */
    0x00, 0x00, 0x00, 0x0A,        /* ext modCount padding at [22..25] (BE) */
#else
    0xE8, 0x03, 0x00, 0x00,        /* ext size     = 1000 at [14..17] (LE) */
    0x90, 0x01, 0x00, 0x00,        /* ext initSize = 400  at [18..21] (LE) */
    0x0A, 0x00, 0x00, 0x00,        /* ext modCount = 10   at [22..25] (LE) */
#endif
    'T',  'O',  'N',  'Y',  'W',
    ' ',  ' ',  ' ',               /* trailing spaces, trimmed        */
    0x00, 0x00, 0x00, 0x00,        /* reserved                        */
    0x00, 0x00                     /* reserved                        */
};

/*
 * 12-byte directory entry: "NFSD    ", TT=1, R=3, indicator=0x00.
 * No user data (0 halfwords).  mvs_pds_member_entry_set returns 12.
 * mvs_skip_dir_entry also returns 12.
 * TT is stored in native byte order so entry->first_block_tt == 1 on
 * both MVS and Linux.
 */
static const uint8_t c_entry_no_data[12] = {
    'N','F','S','D',' ',' ',' ',' ', /* member name [0..7]           */
#ifdef __MVS__
    0x00, 0x01,                       /* TT = 1 (BE) [8..9]          */
#else
    0x01, 0x00,                       /* TT = 1 (LE) [8..9]          */
#endif
    0x03,                             /* R = 3       [10]             */
    0x00                              /* indicator: no alias, 0 hw   */
};

/*
 * 12-byte alias entry: indicator=0x80 (alias flag set, 0 halfwords).
 */
static const uint8_t c_entry_alias[12] = {
    'N','F','S','D',' ',' ',' ',' ',
#ifdef __MVS__
    0x00, 0x01,                       /* TT = 1 (BE)                  */
#else
    0x01, 0x00,                       /* TT = 1 (LE)                  */
#endif
    0x03,
    0x80                              /* alias flag (bit 7)           */
};

/*
 * 42-byte directory entry with full ISPF stats (15 halfwords = 30 bytes).
 * indicator = 0x0F: bits 0-4 = 15 halfwords, bit 7 = 0 (not alias).
 * mvs_pds_member_entry_set returns 42.
 * mvs_skip_dir_entry also returns 42.
 */
static const uint8_t c_entry_full_stats[42] = {
    'N','F','S','D',' ',' ',' ',' ', /* name      [0..7]             */
#ifdef __MVS__
    0x00, 0x01,                       /* TT = 1 (BE) [8..9]          */
#else
    0x01, 0x00,                       /* TT = 1 (LE) [8..9]          */
#endif
    0x01,                             /* R         [10]               */
    0x0F,                             /* indicator [11]: 15 hw, no alias */
    /* ISPF stats [12..41] = 30 bytes */
    0x01, 0x05, 0x00,                 /* ver=1, mod=5, flags=0 [12..14] */
    0x00,                             /* seconds              [15]    */
    0x01, 0x24, 0x00, 0x10,          /* crdate               [16..19]*/
    0x01, 0x24, 0x00, 0x10,          /* chgdate date         [20..23]*/
    0x14, 0x30,                       /* chgdate time         [24..25]*/
#ifdef __MVS__
    0x00, 0x64,                       /* size=100             [26..27] (BE) */
    0x00, 0x50,                       /* initSize=80          [28..29] (BE) */
    0x00, 0x0A,                       /* modCount=10          [30..31] (BE) */
#else
    0x64, 0x00,                       /* size=100             [26..27] (LE) */
    0x50, 0x00,                       /* initSize=80          [28..29] (LE) */
    0x0A, 0x00,                       /* modCount=10          [30..31] (LE) */
#endif
    'T',  'O',  'N',  'Y',  'W',
    ' ',  ' ',  ' ',                  /* user                 [32..39]*/
    0x00, 0x00                        /* reserved             [40..41]*/
};

/*
 * 22-byte directory entry with 5 halfwords (10 bytes) of user data.
 * indicator = 0x05: bits 0-4 = 5 halfwords.
 * mvs_skip_dir_entry returns 22.
 */
static const uint8_t c_entry_5hw[22] = {
    'M','V','S','I','O',' ',' ',' ', /* name [0..7]                  */
#ifdef __MVS__
    0x00, 0x01,                       /* TT = 1 (BE) [8..9]          */
#else
    0x01, 0x00,                       /* TT = 1 (LE) [8..9]          */
#endif
    0x01,                             /* R    [10]                    */
    0x05,                             /* indicator [11]: 5 halfwords  */
    0x00, 0x00, 0x00, 0x00,          /* user data (10 bytes)         */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
};

/*
 * 10-byte directory block containing only the end-of-directory marker.
 * The 2-byte count field is native-endian: count=10 means
 * dir_block_end = block_start + 10; the loop processes bytes [2..9]
 * which are exactly the 8-byte end marker.
 */
static const uint8_t c_block_end_only[10] = {
#ifdef __MVS__
    0x00, 0x0A,                        /* count = 10 (BE)             */
#else
    0x0A, 0x00,                        /* count = 10 (LE)             */
#endif
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF  /* end marker            */
};

/*
 * 26-byte directory block: one member ("NFSD    ") followed by end marker.
 * count = 26 (2 + 12 entry + 8 endmark + 4 padding).
 */
static const uint8_t c_block_one_member[26] = {
#ifdef __MVS__
    0x00, 0x1A,                        /* count = 26 (BE)             */
    'N','F','S','D',' ',' ',' ',' ',  /* name [2..9]                 */
    0x00, 0x01,                        /* TT = 1 (BE) [10..11]       */
#else
    0x1A, 0x00,                        /* count = 26 (LE)             */
    'N','F','S','D',' ',' ',' ',' ',  /* name [2..9]                 */
    0x01, 0x00,                        /* TT = 1 (LE) [10..11]       */
#endif
    0x01, 0x00,                        /* R=1 [12], indicator=0 [13] */
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  /* end marker          */
    0x00, 0x00, 0x00, 0x00             /* padding                     */
};

/*
 * 26-byte directory block: one member ("MVSIO   ") that sorts before
 * "NFSD    ", followed by end marker.  Used to test that entries whose
 * name is less than start_member are skipped.
 */
static const uint8_t c_block_before_start[26] = {
#ifdef __MVS__
    0x00, 0x1A,                        /* count = 26 (BE)             */
    'M','V','S','I','O',' ',' ',' ',  /* name [2..9]                 */
    0x00, 0x01,                        /* TT = 1 (BE) [10..11]       */
#else
    0x1A, 0x00,                        /* count = 26 (LE)             */
    'M','V','S','I','O',' ',' ',' ',  /* name [2..9]                 */
    0x01, 0x00,                        /* TT = 1 (LE) [10..11]       */
#endif
    0x01, 0x00,                        /* R=1 [12], indicator=0 [13] */
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0x00, 0x00, 0x00, 0x00
};

/*
 * 38-byte directory block: two members ("MVSIO   " then "NFSD    ")
 * followed by end marker.  count = 2 + 12 + 12 + 8 + 4 = 38.
 * Used to test the max_members limit.
 */
static const uint8_t c_block_two_members[38] = {
#ifdef __MVS__
    0x00, 0x26,                        /* count = 38 (BE)             */
    'M','V','S','I','O',' ',' ',' ', 0x00,0x01, 0x01, 0x00,
    'N','F','S','D',' ',' ',' ',' ', 0x00,0x02, 0x01, 0x00,
#else
    0x26, 0x00,                        /* count = 38 (LE)             */
    'M','V','S','I','O',' ',' ',' ', 0x01,0x00, 0x01, 0x00,
    'N','F','S','D',' ',' ',' ',' ', 0x02,0x00, 0x01, 0x00,
#endif
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0x00, 0x00, 0x00, 0x00
};


/* ==================================================================== */
/* Suite 1: mvs_extract_ispf_stats                                      */
/* ==================================================================== */

static MunitResult test_ispf_stats_ver_mod_flags(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_extract_ispf_stats(&entry, c_ispf_basic, 30);

    munit_assert_int(entry.ver,        ==, 1);
    munit_assert_int(entry.mod,        ==, 5);
    munit_assert_int(entry.ispf_flags, ==, 0);
    return MUNIT_OK;
}

static MunitResult test_ispf_stats_size_fields(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_extract_ispf_stats(&entry, c_ispf_basic, 30);

    munit_assert_int(entry.size,     ==, 100);
    munit_assert_int(entry.initSize, ==, 80);
    return MUNIT_OK;
}

static MunitResult test_ispf_stats_user_field(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_extract_ispf_stats(&entry, c_ispf_basic, 30);

    /* Three trailing spaces in the 8-byte user field are trimmed */
    munit_assert_string_equal(entry.user, "TONYW");
    return MUNIT_OK;
}

static MunitResult test_ispf_stats_dates_nonzero(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_extract_ispf_stats(&entry, c_ispf_basic, 30);

    /* convert_ispf_datetime should produce a non-zero time_t for 2024-001 */
    munit_assert(entry.crdate  != 0);
    munit_assert(entry.chgdate != 0);
    return MUNIT_OK;
}

static MunitResult test_ispf_stats_extended(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));

    mvs_extract_ispf_stats(&entry, c_ispf_ext, 40);

    munit_assert_int(entry.ver,      ==, 1);
    munit_assert_int(entry.mod,      ==, 3);
    munit_assert_int(entry.size,     ==, 1000);
    munit_assert_int(entry.initSize, ==, 400);
    munit_assert_int(entry.modCount, ==, 10);
    return MUNIT_OK;
}

static MunitTest ispf_stats_tests[] = {
    { "/ver_mod_flags",  test_ispf_stats_ver_mod_flags,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/size_fields",    test_ispf_stats_size_fields,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/user_field",     test_ispf_stats_user_field,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/dates_nonzero",  test_ispf_stats_dates_nonzero,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/extended",       test_ispf_stats_extended,       NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};


/* ==================================================================== */
/* Suite 2: mvs_set_no_ispf_stats                                       */
/* ==================================================================== */

static MunitResult test_no_stats_ver_mod(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0xFF, sizeof(entry)); /* pre-fill with non-zero */
    mvs_set_no_ispf_stats(&entry);

    munit_assert_int(entry.ver,        ==, 0);
    munit_assert_int(entry.mod,        ==, 0);
    munit_assert_int(entry.ispf_flags, ==, 0);
    munit_assert_int(entry.modCount,   ==, 0);
    return MUNIT_OK;
}

static MunitResult test_no_stats_size(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_set_no_ispf_stats(&entry);

    munit_assert_int(entry.size,     ==, 999);
    munit_assert_int(entry.initSize, ==, 999);
    return MUNIT_OK;
}

static MunitResult test_no_stats_user_zeroed(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0xFF, sizeof(entry));
    mvs_set_no_ispf_stats(&entry);

    /* user is memset to 0; check first byte is NUL */
    munit_assert_int(entry.user[0], ==, 0);
    return MUNIT_OK;
}

static MunitResult test_no_stats_dates_set(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_set_no_ispf_stats(&entry);

    /* gettimeofday overwrites the initial zeroes with current epoch time */
    munit_assert(entry.crdate  != 0);
    munit_assert(entry.chgdate != 0);
    return MUNIT_OK;
}

static MunitTest no_stats_tests[] = {
    { "/ver_mod",      test_no_stats_ver_mod,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/size",         test_no_stats_size,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/user_zeroed",  test_no_stats_user_zeroed,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/dates_set",    test_no_stats_dates_set,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};


/* ==================================================================== */
/* Suite 3: mvs_pds_member_entry_set                                    */
/* ==================================================================== */

static MunitResult test_entry_set_len_no_data(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    int len;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    len = mvs_pds_member_entry_set(&entry, c_entry_no_data);

    /* 8-byte name + 2 TT + 1 R + 1 indicator + 0 user-data bytes = 12 */
    munit_assert_int(len, ==, 12);
    return MUNIT_OK;
}

static MunitResult test_entry_set_len_full_stats(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    int len;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    len = mvs_pds_member_entry_set(&entry, c_entry_full_stats);

    /* 12-byte header + 30 bytes ISPF stats (15 halfwords) = 42 */
    munit_assert_int(len, ==, 42);
    return MUNIT_OK;
}

static MunitResult test_entry_set_ttr(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_pds_member_entry_set(&entry, c_entry_no_data);

    /* TT bytes are native-endian so first_block_tt == 1 on both platforms */
    munit_assert_int(entry.first_block_tt,  ==, 1);
    munit_assert_int(entry.first_block_rec, ==, 3);
    return MUNIT_OK;
}

static MunitResult test_entry_set_alias_flag(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_pds_member_entry_set(&entry, c_entry_alias);

    munit_assert_int(entry.entry_type, ==, 0x80);
    return MUNIT_OK;
}

static MunitResult test_entry_set_non_alias(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_pds_member_entry_set(&entry, c_entry_no_data);

    munit_assert_int(entry.entry_type, ==, 0);
    return MUNIT_OK;
}

static MunitResult test_entry_set_name(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t entry;
    (void)params; (void)data;

    memset(&entry, 0, sizeof(entry));
    mvs_pds_member_entry_set(&entry, c_entry_no_data);

    munit_assert_string_equal(entry.name, "NFSD");     /* BUG: never written */
    return MUNIT_OK;
}

static MunitTest entry_set_tests[] = {
    { "/len_no_data",    test_entry_set_len_no_data,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/len_full_stats", test_entry_set_len_full_stats,   NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/ttr",            test_entry_set_ttr,              NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/alias_flag",     test_entry_set_alias_flag,       NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/non_alias",      test_entry_set_non_alias,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/name",   test_entry_set_name, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};


/* ==================================================================== */
/* Suite 4: mvs_skip_dir_entry                                          */
/* ==================================================================== */

static MunitResult test_skip_no_data(
    const MunitParameter params[], void *data)
{
    int len;
    (void)params; (void)data;

    /* indicator = 0x00: 0 halfwords of user data */
    len = mvs_skip_dir_entry(c_entry_no_data);
    munit_assert_int(len, ==, 12);
    return MUNIT_OK;
}

static MunitResult test_skip_full_stats(
    const MunitParameter params[], void *data)
{
    int len;
    (void)params; (void)data;

    /* indicator = 0x0F: 15 halfwords = 30 bytes of user data */
    len = mvs_skip_dir_entry(c_entry_full_stats);
    munit_assert_int(len, ==, 42);
    return MUNIT_OK;
}

static MunitResult test_skip_partial(
    const MunitParameter params[], void *data)
{
    int len;
    (void)params; (void)data;

    /* indicator = 0x05: 5 halfwords = 10 bytes of user data */
    len = mvs_skip_dir_entry(c_entry_5hw);
    munit_assert_int(len, ==, 22);
    return MUNIT_OK;
}

static MunitTest skip_tests[] = {
    { "/no_data",    test_skip_no_data,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/full_stats", test_skip_full_stats, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/partial",    test_skip_partial,    NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};


/* ==================================================================== */
/* Suite 5: mvs_process_dir_block                                       */
/* ==================================================================== */

static MunitResult test_dir_block_end_only(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t members[4];
    int num;
    int end;
    int rc;
    (void)params; (void)data;

    num = 0;
    end = 0;
    memset(members, 0, sizeof(members));
    rc = mvs_process_dir_block(c_block_end_only, "        ", 4,
                               members, &num, &end);

    munit_assert_int(rc,  ==, 0);
    munit_assert_int(num, ==, 0);
    munit_assert_int(end, ==, 1);
    return MUNIT_OK;
}

static MunitResult test_dir_block_one_member(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t members[4];
    int num;
    int end;
    (void)params; (void)data;

    num = 0;
    end = 0;
    memset(members, 0, sizeof(members));
    /*
     * start_member = "NFSD    " (exactly 8 bytes, space-padded).
     * The block contains "NFSD    " which compares >= start, so it is
     * extracted.  The end marker follows immediately.
     *
     */
    mvs_process_dir_block(c_block_one_member, "NFSD    ", 4,
                          members, &num, &end);

    munit_assert_int(num, ==, 1);
    munit_assert_int(end, ==, 1);
    munit_assert_string_equal(members[0].name, "NFSD");
    return MUNIT_OK;
}

static MunitResult test_dir_block_skip_before_start(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t members[4];
    int num;
    int end;
    (void)params; (void)data;

    num = 0;
    end = 0;
    memset(members, 0, sizeof(members));
    /*
     * Block contains "MVSIO   " which sorts before "NFSD    " ('M' < 'N').
     * The entry is skipped; the end marker then sets end_of_dir=1.
     */
    mvs_process_dir_block(c_block_before_start, "NFSD    ", 4,
                          members, &num, &end);

    munit_assert_int(num, ==, 0);
    munit_assert_int(end, ==, 1);
    return MUNIT_OK;
}

static MunitResult test_dir_block_max_members(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t members[4];
    int num;
    int end;
    (void)params; (void)data;

    num = 0;
    end = 0;
    memset(members, 0, sizeof(members));
    /*
     * Block contains "MVSIO   " and "NFSD    " followed by end marker.
     * With max_members=1: only the first entry is extracted, then the
     * loop breaks.  The end marker is never reached, so end_of_dir=0.
     * start_member="        " (8 spaces) is less than any member name.
     */
    mvs_process_dir_block(c_block_two_members, "        ", 1,
                          members, &num, &end);

    munit_assert_int(num, ==, 1);
    munit_assert_int(end, ==, 0);
    return MUNIT_OK;
}

static MunitResult test_dir_block_returns_zero(
    const MunitParameter params[], void *data)
{
    pds_member_entry_t members[4];
    int num;
    int end;
    int rc;
    (void)params; (void)data;

    num = 0;
    end = 0;
    memset(members, 0, sizeof(members));
    rc = mvs_process_dir_block(c_block_end_only, "        ", 4,
                               members, &num, &end);

    munit_assert_int(rc, ==, 0);
    return MUNIT_OK;
}

static MunitTest dir_block_tests[] = {
    { "/end_only",          test_dir_block_end_only,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/one_member",        test_dir_block_one_member,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/skip_before_start", test_dir_block_skip_before_start, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/max_members",       test_dir_block_max_members,       NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/returns_zero",      test_dir_block_returns_zero,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};


/* ==================================================================== */
/* Suite registration                                                   */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/ispf_stats", ispf_stats_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/no_stats",   no_stats_tests,   NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/entry_set",  entry_set_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/skip",       skip_tests,       NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/dir_block",  dir_block_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvsio3_suite = {
    "/mvsio",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
