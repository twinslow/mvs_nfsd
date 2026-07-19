/*
 * tmvsio2.c - Unit tests for mvsio.c: mvs_get_pds_dsn_and_member()
 *
 * Uses the munit framework.
 *
 * Build (from project root):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c tests/tstubs.c tests/tmvsio.c tests/tmvsio2.c \
 *      src/mvsio.c tests/munit.c \
 *      -o tests/runall
 *
 * NOTE: exports.c is NOT linked.  tests/tstubs.c provides the exports
 * table and dataset provider.
 *
 * Under the multi-PDS model a member path has three components:
 *   <export_path>/<dirname>/<member-file>
 * where <dirname> is the lower-case PDS dataset name.  This function:
 *   - resolves <dirname> to its dataset and returns the REAL dsname;
 *   - extracts the leaf file name, strips the extension, upper-cases and
 *     truncates the stem to 8 chars for the member name;
 *   - validates the extension against that dataset's file_ext;
 *   - returns an empty member name when the path is the PDS directory
 *     itself (no member component).
 */

#include <errno.h>
#include <string.h>

#include "munit.h"
#include "nfsd.h"    /* MAX_PATH, MAX_NAME */
#include "mvsio.h"   /* mvs_get_pds_dsn_and_member */
#include "tstubs.h"  /* stub_clear_exports, stub_add_export, stub_add_dataset */

/* ======================================================================
 * Fixtures
 * ====================================================================== */

/* Export "/dinonfs/src" -> dataset TEMP.DINONFS.C (dir temp.dinonfs.c, ext c) */
static void *setup_c_export(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/dinonfs/src", "TEMP.DINONFS.C", "c");
    return NULL;
}

/* Export "/dinonfs/data" -> dataset TEMP.DINONFS.DATA with an empty file_ext
   (the 'nofileext' case): members have no extension, so the whole leaf name is
   the member and a name containing '.' is not a valid member. */
static void *setup_no_ext(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/dinonfs/data", "TEMP.DINONFS.DATA", "");
    return NULL;
}

/* One export "/export" grouping two datasets (dir / ext):
 *   TEMP.TESTPROJ.JCLLIB  -> temp.testproj.jcllib  / jcllib
 *   TEMP.TESTPROJ.CNTL    -> temp.testproj.cntl    / cntl
 */
static void *setup_multi_ds(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/export", "TEMP.TESTPROJ.JCLLIB", "jcllib");
    stub_add_dataset("TEMP.TESTPROJ.CNTL", "cntl");
    return NULL;
}

/* ======================================================================
 * dsname is resolved from the directory name (the real PDS name)
 * ====================================================================== */

static MunitResult test_dsname_from_dataset(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c/nfsd.c", dsname, member, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(dsname, "TEMP.DINONFS.C");
    return MUNIT_OK;
}

/* The dsname comes from the matched dataset, not from export_idx alone:
 * two datasets in one export resolve to different real dsnames. */
static MunitResult test_dsname_multi_first(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    result = mvs_get_pds_dsn_and_member(
                 "/export/temp.testproj.jcllib/aaa.jcllib", dsname, member, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(dsname, "TEMP.TESTPROJ.JCLLIB");
    munit_assert_string_equal(member, "AAA");
    return MUNIT_OK;
}

static MunitResult test_dsname_multi_second(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    result = mvs_get_pds_dsn_and_member(
                 "/export/temp.testproj.cntl/runjob.cntl", dsname, member, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(dsname, "TEMP.TESTPROJ.CNTL");
    munit_assert_string_equal(member, "RUNJOB");
    return MUNIT_OK;
}

/* ======================================================================
 * PDS directory itself (no member component) -> empty member name
 * ====================================================================== */

static MunitResult test_dataset_path_empty_member(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c", dsname, member, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(dsname, "TEMP.DINONFS.C");
    munit_assert_string_equal(member, "");
    return MUNIT_OK;
}

/* ======================================================================
 * Member name extraction
 * ====================================================================== */

static MunitResult test_member_uppercased(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c/nfsd.c", dsname, member, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(member, "NFSD");
    return MUNIT_OK;
}

/* An over-length base name is rejected (NOT silently truncated), so that
   "report01a" and "report01b" cannot collapse to the same 8-char member. */
static MunitResult test_member_too_long_rejected(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    errno = 0;
    /* "verylongname" is 12 chars -> too long for a member */
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c/verylongname.c", dsname, member, 0);

    munit_assert_int(result, ==, -1);
    munit_assert_int(errno,  ==, ENAMETOOLONG);
    return MUNIT_OK;
}

/* An exactly-8-char base name is accepted (boundary case). */
static MunitResult test_member_exactly_8_ok(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c/rep01234.c", dsname, member, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(member, "REP01234");
    return MUNIT_OK;
}

/* A base name that starts with a digit is rejected (member names cannot
   begin with a digit).  This is the case that slipped through on MVS. */
static MunitResult test_member_leading_digit_rejected(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    errno = 0;
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c/1test.c", dsname, member, 0);

    munit_assert_int(result, ==, -1);
    munit_assert_int(errno,  ==, ENAMETOOLONG);   /* coarsened for the client */
    return MUNIT_OK;
}

/* A base name with an invalid member character (a hyphen) is rejected. */
static MunitResult test_member_invalid_char_rejected(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    errno = 0;
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c/my-file.c", dsname, member, 0);

    munit_assert_int(result, ==, -1);
    /* The validator flags this as EINVAL internally, but the path helper
       coarsens every "not a valid member name" fault to ENAMETOOLONG so the
       Windows client shows the clear "filename or extension is too long"
       message rather than the confusing NFS3ERR_INVAL one. */
    munit_assert_int(errno,  ==, ENAMETOOLONG);
    return MUNIT_OK;
}

/* A member file with no extension, on an export with no ext restriction. */
static MunitResult test_member_no_extension(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/data/temp.dinonfs.data/mvsio", dsname, member, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(member, "MVSIO");
    return MUNIT_OK;
}

/* ======================================================================
 * Extension validation against the DATASET's file_ext
 * ====================================================================== */

static MunitResult test_matching_extension_ok(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c/nfsd.c", dsname, member, 0);

    munit_assert_int(result, ==, 0);
    return MUNIT_OK;
}

/* A filename whose extension does not match the dataset's is rejected, so the
   filename<->member mapping stays 1:1 (prevents silent overwrites of the same
   member via different extensions). */
static MunitResult test_wrong_extension_error(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    errno = 0;
    /* dataset expects ".c" but the file is ".h" */
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c/types.h", dsname, member, 0);

    munit_assert_int(result, ==, -1);
    munit_assert_int(errno,  ==, ENOENT);
    return MUNIT_OK;
}

static MunitResult test_extension_case_insensitive(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    /* ".C" must match dataset file_ext "c" */
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/temp.dinonfs.c/nfsd.C", dsname, member, 0);

    munit_assert_int(result, ==, 0);
    return MUNIT_OK;
}

/* A no-extension (nofileext, or single-qualifier) dataset takes the WHOLE leaf
   as the member name -- so a filename containing '.' is rejected, NOT silently
   truncated at the dot (which would map both "report.txt" and "report.dat" to
   member REPORT).  Member names cannot contain a dot. */
static MunitResult test_no_ext_dotted_name_rejected(const MunitParameter params[], void *data)
{
    char dsname[MAX_PATH];
    char member[9];
    int  result;
    (void)params; (void)data;

    dsname[0] = '\0'; member[0] = '\0';
    errno = 0;
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/data/temp.dinonfs.data/report.txt", dsname, member, 0);

    munit_assert_int(result, ==, -1);
    munit_assert_int(errno,  ==, ENAMETOOLONG);   /* coarsened for the client */
    return MUNIT_OK;
}

/* ======================================================================
 * mvs_member_name_valid() -- direct, dependency-free tests of the
 * PDS member-name rules (1-8 chars; first char A-Z or @ # $, not a
 * digit; remaining A-Z / 0-9 / @ # $; no lowercase or other specials).
 * ====================================================================== */

/* Valid member names return 0. */
static MunitResult test_mnv_valid(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    munit_assert_int(mvs_member_name_valid("A"),        ==, 0);  /* min length */
    munit_assert_int(mvs_member_name_valid("ABC"),      ==, 0);
    munit_assert_int(mvs_member_name_valid("ABCDEFGH"), ==, 0);  /* max length 8 */
    munit_assert_int(mvs_member_name_valid("A1B2C3D4"), ==, 0);  /* digits after first */
    munit_assert_int(mvs_member_name_valid("@ABC"),     ==, 0);  /* national first */
    munit_assert_int(mvs_member_name_valid("#JOB"),     ==, 0);
    munit_assert_int(mvs_member_name_valid("$SYS1"),    ==, 0);
    munit_assert_int(mvs_member_name_valid("AB@#$"),    ==, 0);  /* national in rest */
    return MUNIT_OK;
}

/* Over-length names return ENAMETOOLONG. */
static MunitResult test_mnv_too_long(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    munit_assert_int(mvs_member_name_valid("ABCDEFGHI"),    ==, ENAMETOOLONG); /* 9 */
    munit_assert_int(mvs_member_name_valid("ABCDEFGHIJKL"), ==, ENAMETOOLONG); /* 12 */
    return MUNIT_OK;
}

/* Empty / leading-digit / lowercase / bad-character names return EINVAL. */
static MunitResult test_mnv_invalid(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    munit_assert_int(mvs_member_name_valid(""),     ==, EINVAL);  /* empty */
    munit_assert_int(mvs_member_name_valid("1ABC"), ==, EINVAL);  /* leading digit */
    munit_assert_int(mvs_member_name_valid("9"),    ==, EINVAL);  /* single digit */
    munit_assert_int(mvs_member_name_valid("abc"),  ==, EINVAL);  /* lowercase */
    munit_assert_int(mvs_member_name_valid("Abc"),  ==, EINVAL);  /* mixed case */
    munit_assert_int(mvs_member_name_valid("A-BC"), ==, EINVAL);  /* hyphen */
    munit_assert_int(mvs_member_name_valid("A.BC"), ==, EINVAL);  /* dot */
    munit_assert_int(mvs_member_name_valid("A BC"), ==, EINVAL);  /* space */
    munit_assert_int(mvs_member_name_valid("A/B"),  ==, EINVAL);  /* slash */
    return MUNIT_OK;
}

/* ======================================================================
 * Suite tables
 * ====================================================================== */

static MunitTest member_name_valid_tests[] = {
    { "/valid",     test_mnv_valid,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/too_long",  test_mnv_too_long, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/invalid",   test_mnv_invalid,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest dsname_tests[] = {
    { "/dsname_from_dataset", test_dsname_from_dataset, setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/dsname_multi_first",  test_dsname_multi_first,  setup_multi_ds, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/dsname_multi_second", test_dsname_multi_second, setup_multi_ds, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest dataset_path_tests[] = {
    { "/dataset_path_empty_member", test_dataset_path_empty_member, setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest member_tests[] = {
    { "/member_uppercased",        test_member_uppercased,           setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_too_long_rejected", test_member_too_long_rejected,    setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_exactly_8_ok",      test_member_exactly_8_ok,         setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_leading_digit",     test_member_leading_digit_rejected, setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_invalid_char",      test_member_invalid_char_rejected, setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_no_extension",      test_member_no_extension,         setup_no_ext,   NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest extension_tests[] = {
    { "/matching_extension_ok",       test_matching_extension_ok,       setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/wrong_extension_error",       test_wrong_extension_error,       setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/extension_case_insensitive",  test_extension_case_insensitive,  setup_c_export, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/no_ext_dotted_name_rejected", test_no_ext_dotted_name_rejected, setup_no_ext, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitSuite sub_suites[] = {
    { "/member_name_valid", member_name_valid_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/dsname",       dsname_tests,       NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/dataset_path", dataset_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/member",       member_tests,       NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/extension",    extension_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL,            NULL,               NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

/* Exported -- referenced by tests/runall.c */
MunitSuite tmvsio2_suite = {
    "/mvsio/mvs_get_pds_dsn_and_member",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
