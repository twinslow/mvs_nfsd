/*
 * tmvsio.c - Unit tests for mvsio.c: mvs_path_type()
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
 * mvs_path_type() classifies an export-relative path into three levels:
 *   <export_path>                        -> ROOT
 *   <export_path>/<dirname>              -> DATASET      (a PDS directory)
 *   <export_path>/<dirname>/<member>     -> PDS_MEMBER
 * where <dirname> is the lower-case form of the PDS dataset name.
 */

#include <string.h>

#include "munit.h"
#include "nfsd.h"      /* export_t, MAX_EXPORTS, MAX_PATH */
#include "mvsio.h"     /* mvs_path_type, MVS_PATH_TYPE_* */
#include "tstubs.h"    /* stub_clear_exports, stub_add_export, stub_add_dataset */

/* ======================================================================
 * Fixtures
 * ====================================================================== */

/* One export "/mvsnfsd/src" with a single dataset TEMP.NFSD.C,
 * seen by clients as directory "temp.nfsd.c" -- LOWER case, because both
 * the server and stub_add_dataset() derive the directory name by
 * lower-casing the dsname, and the lookup is an exact strcmp. */
static void *setup_single(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/mvsnfsd/src", "TEMP.NFSD.C", "c");
    return NULL;
}

/* One export "/export" grouping three datasets (dirnames):
 *   [0] TEMP.TESTPROJ.JCLLIB  -> temp.testproj.jcllib
 *   [1] TEMP.TESTPROJ.CNTL    -> temp.testproj.cntl
 *   [2] TEMP.TESTPROJ.LOADLIB -> temp.testproj.loadlib
 */
static void *setup_multi_ds(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/export", "TEMP.TESTPROJ.JCLLIB", "jcllib");
    stub_add_dataset("TEMP.TESTPROJ.CNTL",    "cntl");
    stub_add_dataset("TEMP.TESTPROJ.LOADLIB", "loadlib");
    return NULL;
}

/* Three separate exports, one dataset each, for export-selection tests. */
static void *setup_multi_exports(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/mvsnfsd/src", "TEMP.NFSD.C",    "c");
    stub_add_export("/mvsnfsd/hdr", "TEMP.NFSD.H",    "h");
    stub_add_export("/mvsnfsd/jcl", "TEMP.NFSD.CNTL", "cntl");
    return NULL;
}

/* Empty exports table. */
static void *setup_empty(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    return NULL;
}

/* ======================================================================
 * ROOT: the export path itself is the (virtual) root directory
 * ====================================================================== */

static MunitResult test_export_path_is_root(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/src", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_ROOT);
    munit_assert_int(eidx,   ==, 0);
    munit_assert_int(didx,   ==, -1);   /* no dataset for the root */
    return MUNIT_OK;
}

static MunitResult test_second_export_path_is_root(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/hdr", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_ROOT);
    munit_assert_int(eidx,   ==, 1);
    return MUNIT_OK;
}

/* ======================================================================
 * DATASET: <export>/<dirname> resolves to a PDS directory
 * ====================================================================== */

static MunitResult test_dirname_is_dataset(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/src/temp.nfsd.c", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(eidx,   ==, 0);
    munit_assert_int(didx,   ==, 0);
    return MUNIT_OK;
}

/* Second dataset within one export resolves to dataset_idx 1. */
static MunitResult test_dataset_idx_selected_by_dirname(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/export/temp.testproj.cntl", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(eidx,   ==, 0);
    munit_assert_int(didx,   ==, 1);
    return MUNIT_OK;
}

/* Third dataset -> dataset_idx 2. */
static MunitResult test_dataset_idx_third(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/export/temp.testproj.loadlib", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(didx,   ==, 2);
    return MUNIT_OK;
}

/* An unknown directory name under a valid export is not exported. */
static MunitResult test_unknown_dirname_not_exported(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/export/temp.nosuch.pds", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    return MUNIT_OK;
}

/* Trailing slash after the dirname still resolves to the directory. */
static MunitResult test_dirname_trailing_slash_is_dataset(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/src/temp.nfsd.c/", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(didx,   ==, 0);
    return MUNIT_OK;
}

/* ======================================================================
 * PDS_MEMBER: <export>/<dirname>/<member>
 * ====================================================================== */

static MunitResult test_member_path_is_pds_member(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/src/temp.nfsd.c/nfsd.c", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_PDS_MEMBER);
    munit_assert_int(eidx,   ==, 0);
    munit_assert_int(didx,   ==, 0);
    return MUNIT_OK;
}

static MunitResult test_member_path_selects_dataset(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/export/temp.testproj.loadlib/thing.loadlib",
                           &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_PDS_MEMBER);
    munit_assert_int(didx,   ==, 2);
    return MUNIT_OK;
}

/* Anything deeper than <dirname>/<member> is not supported. */
static MunitResult test_too_deep_not_exported(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/src/temp.nfsd.c/sub/nfsd.c",
                           &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    return MUNIT_OK;
}

/* ======================================================================
 * Non-matching paths
 * ====================================================================== */

/* Unrelated path -> NOT_EXPORTED, indices unchanged. */
static MunitResult test_unrelated_path_no_match(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/not/exported/at/all", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    munit_assert_int(eidx, ==, -1);
    munit_assert_int(didx, ==, -1);
    return MUNIT_OK;
}

/* Shared prefix without a '/' boundary must not match. */
static MunitResult test_prefix_without_slash_no_match(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/srcextra", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    munit_assert_int(eidx, ==, -1);
    return MUNIT_OK;
}

/* Empty path -> no match. */
static MunitResult test_empty_path_no_match(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    return MUNIT_OK;
}

/* Export path with a trailing slash (empty component) -> no match. */
static MunitResult test_export_trailing_slash_no_match(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/src/", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    return MUNIT_OK;
}

/* Any path against an empty exports table -> no match. */
static MunitResult test_no_exports_configured(const MunitParameter params[], void *data)
{
    int eidx = -1, didx = -1, result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/src", &eidx, &didx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    munit_assert_int(eidx, ==, -1);
    return MUNIT_OK;
}

/* NULL output pointers must be tolerated. */
static MunitResult test_null_out_pointers_ok(const MunitParameter params[], void *data)
{
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/mvsnfsd/src/temp.nfsd.c", NULL, NULL);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    return MUNIT_OK;
}

/* ======================================================================
 * Suite tables
 * ====================================================================== */

static MunitTest root_tests[] = {
    { "/export_path_is_root",        test_export_path_is_root,        setup_single,         NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/second_export_path_is_root", test_second_export_path_is_root, setup_multi_exports,  NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest dataset_tests[] = {
    { "/dirname_is_dataset",              test_dirname_is_dataset,              setup_single,   NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/dataset_idx_selected_by_dirname", test_dataset_idx_selected_by_dirname, setup_multi_ds, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/dataset_idx_third",               test_dataset_idx_third,               setup_multi_ds, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/unknown_dirname_not_exported",    test_unknown_dirname_not_exported,    setup_multi_ds, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/dirname_trailing_slash",          test_dirname_trailing_slash_is_dataset, setup_single, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest member_tests[] = {
    { "/member_path_is_pds_member",   test_member_path_is_pds_member,   setup_single,   NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_path_selects_dataset", test_member_path_selects_dataset, setup_multi_ds, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/too_deep_not_exported",       test_too_deep_not_exported,       setup_single,   NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest nomatch_tests[] = {
    { "/unrelated_path_no_match",       test_unrelated_path_no_match,       setup_single, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/prefix_without_slash_no_match", test_prefix_without_slash_no_match, setup_single, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/empty_path_no_match",           test_empty_path_no_match,           setup_single, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/export_trailing_slash_no_match",test_export_trailing_slash_no_match,setup_single, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/no_exports_configured",         test_no_exports_configured,         setup_empty,  NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/null_out_pointers_ok",          test_null_out_pointers_ok,          setup_single, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitSuite sub_suites[] = {
    { "/root",    root_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/dataset", dataset_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/member",  member_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/nomatch", nomatch_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL,       NULL,          NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

/* Exported -- referenced by tests/runall.c */
MunitSuite tmvsio_suite = {
    "/mvsio/mvs_path_type",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
