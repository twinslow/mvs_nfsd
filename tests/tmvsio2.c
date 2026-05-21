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
 * Run:
 *   tests/runall
 *
 * NOTE: exports.c is NOT linked.  Stub implementations of exports_count()
 * and exports_get() are provided by tests/tstubs.c.
 *
 * Function under test:
 *   int mvs_get_pds_dsn_and_member(
 *           const char *path,
 *           char       *pds_dsname,
 *           char       *pds_member_name,
 *           int         export_idx);
 *
 * Behaviour:
 *   - Copies exports_get(export_idx)->host_path into pds_dsname,
 *     truncated to 44 characters (MVS dataset name limit).
 *   - If strlen(path) <= strlen(export_path): sets pds_member_name to ""
 *     and returns 0  (path is the dataset root, not a member).
 *   - Otherwise extracts the filename from the last '/' in path, splits
 *     at the last '.' to separate name and extension, uppercases and
 *     truncates the name to 8 characters for pds_member_name.
 *   - If exports_get(export_idx)->file_ext is non-empty and does not
 *     match the extracted extension (case-insensitive): sets errno to
 *     ENOENT and returns -1.
 *   - Returns 0 on success.
 */

#include <errno.h>
#include <string.h>

#include "munit.h"
#include "nfsd.h"    /* MAX_PATH, MAX_NAME */
#include "mvsio.h"   /* mvs_get_pds_dsn_and_member */
#include "tstubs.h"  /* stub_clear_exports, stub_add_export */

/* ======================================================================
 * Fixtures
 * ====================================================================== */

/*
 * setup_c_export: single export mapping a C-source PDS.
 *   export_path : /dinonfs/src
 *   host_path   : TEMP.DINONFS.C
 *   file_ext    : c
 */
static void *setup_c_export(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/dinonfs/src", "TEMP.DINONFS.C", "c");
    return NULL;
}

/*
 * setup_long_dsname: export whose host_path exceeds the 44-character MVS
 * dataset name limit (49 chars), allowing truncation to be tested.
 *   host_path : TONYW.DINONFS.VERY.LONG.DSN.EXCEEDS.MAXIMUM.CHARS
 *               (49 characters)
 */
static void *setup_long_dsname(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/dinonfs/src",
                    "TONYW.DINONFS.VERY.LONG.DSN.EXCEEDS.MAXIMUM.CHARS",
                    "c");
    return NULL;
}

/*
 * setup_no_ext: export with no file-extension restriction (file_ext == "").
 * Any file extension (or none) should be accepted.
 */
static void *setup_no_ext(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/dinonfs/data", "TEMP.DINONFS.DATA", "");
    return NULL;
}

/*
 * setup_multi: two exports for index-selection tests.
 *   [0] /dinonfs/src -> TEMP.DINONFS.C   (c)
 *   [1] /dinonfs/hdr -> TEMP.DINONFS.H   (h)
 */
static void *setup_multi(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/dinonfs/src", "TEMP.DINONFS.C", "c");
    stub_add_export("/dinonfs/hdr", "TEMP.DINONFS.H", "h");
    return NULL;
}

/* ======================================================================
 * Tests: pds_dsname is taken from the export host_path
 * ====================================================================== */

/* pds_dsname should equal the export host_path exactly */
static MunitResult test_dsname_equals_host_path(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/nfsd.c", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(pds_dsname, "TEMP.DINONFS.C");
    return MUNIT_OK;
}

/* pds_dsname should be truncated to 44 characters when host_path is longer */
static MunitResult test_dsname_truncated_to_44(const MunitParameter params[], void *data)
{
    char   pds_dsname[MAX_PATH];
    char   pds_member_name[9];
    int    result;
    size_t dsname_len;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/nfsd.c", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    dsname_len = strlen(pds_dsname);
    munit_assert_size(dsname_len, ==, 44);
    return MUNIT_OK;
}

/* ======================================================================
 * Tests: path is the dataset root (no member component)
 * ====================================================================== */

/* When path equals the export_path exactly, pds_member_name is empty */
static MunitResult test_path_is_dataset_empty_member(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(pds_member_name, "");
    return MUNIT_OK;
}

/* ======================================================================
 * Tests: member name extraction and transformation
 * ====================================================================== */

/* Lowercase filename stem is uppercased in pds_member_name */
static MunitResult test_member_name_uppercased(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/nfsd.c", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(pds_member_name, "NFSD");
    return MUNIT_OK;
}

/* Member name is truncated to 8 characters when the stem is longer */
static MunitResult test_member_name_truncated_at_8(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    /* "verylongname" is 12 characters; truncated to 8 -> "VERYLONG" */
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/verylongname.c", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(pds_member_name, "VERYLONG");
    return MUNIT_OK;
}

/* File with no dot: entire filename becomes the member name */
static MunitResult test_member_name_no_extension(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    /* No extension restriction on this export, so any ext (or none) is fine */
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/data/MVSIO", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(pds_member_name, "MVSIO");
    return MUNIT_OK;
}

/* ======================================================================
 * Tests: file extension matching
 * ====================================================================== */

/* Correct extension returns 0 */
static MunitResult test_matching_extension_returns_ok(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/nfsd.c", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    return MUNIT_OK;
}

/* Wrong extension returns -1 and sets errno to ENOENT */
static MunitResult test_wrong_extension_returns_error(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';
    errno = 0;

    /* Export expects ".c" but path has ".h" */
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/types.h", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, -1);
    munit_assert_int(errno,  ==, ENOENT);
    return MUNIT_OK;
}

/* Extension matching is case-insensitive: ".C" matches an export with "c" */
static MunitResult test_extension_match_case_insensitive(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    /* Uppercase extension ".C" should still match export file_ext "c" */
    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/nfsd.C", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    return MUNIT_OK;
}

/* ======================================================================
 * Tests: no file-extension restriction on the export
 * ====================================================================== */

/* When export file_ext is empty, any extension is accepted */
static MunitResult test_no_ext_restriction_accepts_any_ext(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/data/report.txt", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(pds_member_name, "REPORT");
    return MUNIT_OK;
}

/* When export file_ext is empty, a file with no extension is also accepted */
static MunitResult test_no_ext_restriction_no_dot_in_name(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/data/MVSIO", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(pds_member_name, "MVSIO");
    return MUNIT_OK;
}

/* ======================================================================
 * Tests: correct export is selected by export_idx
 * ====================================================================== */

/* export_idx 0 -> first export's host_path */
static MunitResult test_dsname_first_export(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/src/nfsd.c", pds_dsname, pds_member_name, 0);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(pds_dsname, "TEMP.DINONFS.C");
    return MUNIT_OK;
}

/* export_idx 1 -> second export's host_path */
static MunitResult test_dsname_second_export(const MunitParameter params[], void *data)
{
    char pds_dsname[MAX_PATH];
    char pds_member_name[9];
    int  result;
    (void)params; (void)data;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';

    result = mvs_get_pds_dsn_and_member(
                 "/dinonfs/hdr/types.h", pds_dsname, pds_member_name, 1);

    munit_assert_int(result, ==, 0);
    munit_assert_string_equal(pds_dsname, "TEMP.DINONFS.H");
    return MUNIT_OK;
}

/* ======================================================================
 * Test suite tables
 * ====================================================================== */

static MunitTest dsname_tests[] = {
    {
        "/dsname_equals_host_path",
        test_dsname_equals_host_path,
        setup_c_export, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest dsname_truncation_tests[] = {
    {
        "/dsname_truncated_to_44",
        test_dsname_truncated_to_44,
        setup_long_dsname, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest dataset_path_tests[] = {
    {
        "/path_is_dataset_empty_member",
        test_path_is_dataset_empty_member,
        setup_c_export, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest member_extraction_tests[] = {
    {
        "/member_name_uppercased",
        test_member_name_uppercased,
        setup_c_export, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/member_name_truncated_at_8",
        test_member_name_truncated_at_8,
        setup_c_export, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/member_name_no_extension",
        test_member_name_no_extension,
        setup_no_ext, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest extension_tests[] = {
    {
        "/matching_extension_returns_ok",
        test_matching_extension_returns_ok,
        setup_c_export, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/wrong_extension_returns_error",
        test_wrong_extension_returns_error,
        setup_c_export, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/extension_match_case_insensitive",
        test_extension_match_case_insensitive,
        setup_c_export, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest no_ext_restriction_tests[] = {
    {
        "/accepts_any_extension",
        test_no_ext_restriction_accepts_any_ext,
        setup_no_ext, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/accepts_no_extension",
        test_no_ext_restriction_no_dot_in_name,
        setup_no_ext, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest export_idx_tests[] = {
    {
        "/dsname_first_export",
        test_dsname_first_export,
        setup_multi, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/dsname_second_export",
        test_dsname_second_export,
        setup_multi, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* Sub-suites */
static MunitSuite sub_suites[] = {
    { "/dsname",       dsname_tests,           NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/dsname_trunc", dsname_truncation_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/dataset_path", dataset_path_tests,      NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/member",       member_extraction_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/extension",    extension_tests,         NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/no_ext",       no_ext_restriction_tests,NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/export_idx",   export_idx_tests,        NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL,            NULL,                    NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

/* Exported -- referenced by tests/runall.c */
MunitSuite tmvsio2_suite = {
    "/mvsio/mvs_get_pds_dsn_and_member",
    NULL,       /* no top-level tests; all are in sub-suites */
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
