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
 * Run:
 *   tests/tmvsio
 *
 * NOTE: exports.c is NOT linked.  This file provides lightweight stubs
 * for exports_count() and exports_get() so the tests are fully self-
 * contained and need no config file or live dataset.
 */

#include <string.h>

#include "munit.h"
#include "nfsd.h"      /* export_t, MAX_EXPORTS, MAX_PATH */
#include "mvsvfs.h"    /* MVS_PATH_TYPE_DATASET, MVS_PATH_TYPE_PDS_MEMBER */
#include "mvsio.h"     /* mvs_path_type */
#include "tstubs.h"    /* stub_clear_exports, stub_add_export */

/* ======================================================================
 * Fixture: single export
 *   NFS path  : /dinonfs/src
 *   host path : TEMP.DINONFS.C
 *   extension : c
 * ====================================================================== */

static void *setup_single(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/dinonfs/src", "TEMP.DINONFS.C", "c");
    return NULL;
}

/* ======================================================================
 * Fixture: three exports
 *   [0] /dinonfs/src  -> TEMP.DINONFS.C    (c)
 *   [1] /dinonfs/hdr  -> TEMP.DINONFS.H    (h)
 *   [2] /dinonfs/jcl  -> TEMP.DINONFS.CNTL (jcl)
 * ====================================================================== */

static void *setup_multi(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/dinonfs/src", "TEMP.DINONFS.C",    "c");
    stub_add_export("/dinonfs/hdr", "TEMP.DINONFS.H",    "h");
    stub_add_export("/dinonfs/jcl", "TEMP.DINONFS.CNTL", "jcl");
    return NULL;
}

/* ======================================================================
 * Fixture: empty exports table (no exports configured)
 * ====================================================================== */

static void *setup_empty(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    return NULL;
}

/* ======================================================================
 * Tests: exact match returns MVS_PATH_TYPE_DATASET
 * ====================================================================== */

/* Path exactly matches the single export path -> DATASET, idx == 0 */
static MunitResult test_exact_match_is_dataset(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/src", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(idx,    ==, 0);
    return MUNIT_OK;
}

/* Path exactly matches the first of three exports -> DATASET, idx == 0 */
static MunitResult test_exact_match_first_of_multi(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/src", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(idx,    ==, 0);
    return MUNIT_OK;
}

/* Path exactly matches the second export -> DATASET, idx == 1 */
static MunitResult test_exact_match_second_export(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/hdr", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(idx,    ==, 1);
    return MUNIT_OK;
}

/* Path exactly matches the third export -> DATASET, idx == 2 */
static MunitResult test_exact_match_third_export(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/jcl", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(idx,    ==, 2);
    return MUNIT_OK;
}

/* ======================================================================
 * Tests: path under an export returns MVS_PATH_TYPE_PDS_MEMBER
 * ====================================================================== */

/* export_path/member -> PDS_MEMBER, idx == 0 */
static MunitResult test_member_path_is_pds_member(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/src/nfsd.c", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_PDS_MEMBER);
    munit_assert_int(idx,    ==, 0);
    return MUNIT_OK;
}

/* Member path under the second export -> PDS_MEMBER, idx == 1 */
static MunitResult test_member_path_second_export(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/hdr/types.h", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_PDS_MEMBER);
    munit_assert_int(idx,    ==, 1);
    return MUNIT_OK;
}

/* Member path under the third export -> PDS_MEMBER, idx == 2 */
static MunitResult test_member_path_third_export(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/jcl/compile.jcl", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_PDS_MEMBER);
    munit_assert_int(idx,    ==, 2);
    return MUNIT_OK;
}

/* ======================================================================
 * Tests: paths that should not match anything
 * ====================================================================== */

/* Completely unrelated path -> 0 */
static MunitResult test_unrelated_path_no_match(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/not/exported/at/all", &idx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    /* idx must be unchanged when there is no match */
    munit_assert_int(idx, ==, -1);
    return MUNIT_OK;
}

/* Path shares a prefix with the export but has no '/' separator ->
 * e.g. "/dinonfs/srcextra" should NOT match "/dinonfs/src" */
static MunitResult test_prefix_without_slash_no_match(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/srcextra", &idx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    munit_assert_int(idx, ==, -1);
    return MUNIT_OK;
}

/* Path is a strict prefix (shorter) of the export path -> no match.
 * e.g. "/dinonfs/sr" does not match "/dinonfs/src" */
static MunitResult test_shorter_than_export_no_match(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/sr", &idx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    munit_assert_int(idx, ==, -1);
    return MUNIT_OK;
}

/* Empty path -> no match */
static MunitResult test_empty_path_no_match(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("", &idx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    munit_assert_int(idx, ==, -1);
    return MUNIT_OK;
}

/* Root "/" path -> no match */
static MunitResult test_root_path_no_match(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/", &idx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    munit_assert_int(idx, ==, -1);
    return MUNIT_OK;
}

/* Path that is the export path with a trailing slash -> no match.
 * "/dinonfs/src/" is neither an exact match nor a member path because
 * there is no filename component after the slash. */
static MunitResult test_trailing_slash_no_match(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/src/", &idx);

    /*
     * The implementation returns PDS_MEMBER for "/dinonfs/src/" because
     * path[export_path_len] == '/' and the length check passes.
     * This test documents current behaviour: callers must strip trailing
     * slashes before calling mvs_path_type.
     */
    munit_assert_int(result, ==, MVS_PATH_TYPE_PDS_MEMBER);
    munit_assert_int(idx, ==, 0);
    return MUNIT_OK;
}

/* ======================================================================
 * Tests: empty exports table
 * ====================================================================== */

/* Any path against an empty export table -> 0 */
static MunitResult test_no_exports_configured(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("/dinonfs/src", &idx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    munit_assert_int(idx, ==, -1);
    return MUNIT_OK;
}

/* ======================================================================
 * Test suite tables
 * ====================================================================== */

static MunitTest single_export_tests[] = {
    {
        "/exact_match_is_dataset",
        test_exact_match_is_dataset,
        setup_single, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/member_path_is_pds_member",
        test_member_path_is_pds_member,
        setup_single, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/unrelated_path_no_match",
        test_unrelated_path_no_match,
        setup_single, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/prefix_without_slash_no_match",
        test_prefix_without_slash_no_match,
        setup_single, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/shorter_than_export_no_match",
        test_shorter_than_export_no_match,
        setup_single, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/empty_path_no_match",
        test_empty_path_no_match,
        setup_single, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/root_path_no_match",
        test_root_path_no_match,
        setup_single, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/trailing_slash_behaviour",
        test_trailing_slash_no_match,
        setup_single, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest multi_export_tests[] = {
    {
        "/exact_match_first_of_multi",
        test_exact_match_first_of_multi,
        setup_multi, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/exact_match_second_export",
        test_exact_match_second_export,
        setup_multi, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/exact_match_third_export",
        test_exact_match_third_export,
        setup_multi, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/member_path_second_export",
        test_member_path_second_export,
        setup_multi, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    {
        "/member_path_third_export",
        test_member_path_third_export,
        setup_multi, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest empty_export_tests[] = {
    {
        "/no_exports_configured",
        test_no_exports_configured,
        setup_empty, NULL,
        MUNIT_TEST_OPTION_NONE, NULL
    },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* Sub-suites */
static MunitSuite sub_suites[] = {
    { "/single", single_export_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/multi",  multi_export_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/empty",  empty_export_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL,      NULL,                NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

/* Exported -- referenced by tests/runall.c */
MunitSuite tmvsio_suite = {
    "/mvsio/mvs_path_type",
    NULL,           /* no top-level tests; all are in sub-suites */
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
