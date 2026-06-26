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
 *   export_path (NFS) : /dinonfs/src
 *   host_path (MVS)   : TEMP.DINONFS.C
 *   extension         : c
 *
 * mvs_path_type() matches on host_path_ebcdic (the host path field).
 * On Linux, host_path_ebcdic is identical to host_path (no EBCDIC
 * conversion), so tests pass the host path directly.
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
 *
 * Tests pass the host_path (e.g. "TEMP.DINONFS.C") to mvs_path_type()
 * since that is what host_path_ebcdic is set to on Linux.
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

/* Host path exactly matches host_path_ebcdic -> DATASET, idx == 0 */
static MunitResult test_exact_match_is_dataset(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("TEMP.DINONFS.C", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(idx,    ==, 0);
    return MUNIT_OK;
}

/* Host path matches first export's host_path_ebcdic -> DATASET, idx == 0 */
static MunitResult test_exact_match_first_of_multi(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("TEMP.DINONFS.C", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(idx,    ==, 0);
    return MUNIT_OK;
}

/* Host path matches second export's host_path_ebcdic -> DATASET, idx == 1 */
static MunitResult test_exact_match_second_export(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("TEMP.DINONFS.H", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(idx,    ==, 1);
    return MUNIT_OK;
}

/* Host path matches third export's host_path_ebcdic -> DATASET, idx == 2 */
static MunitResult test_exact_match_third_export(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("TEMP.DINONFS.CNTL", &idx);

    munit_assert_int(result, ==, MVS_PATH_TYPE_DATASET);
    munit_assert_int(idx,    ==, 2);
    return MUNIT_OK;
}

/* ======================================================================
 * Tests: path under an export returns MVS_PATH_TYPE_PDS_MEMBER
 * ====================================================================== */

/* host_path/member -> PDS_MEMBER, idx == 0 */
static MunitResult test_member_path_is_pds_member(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("TEMP.DINONFS.C/nfsd.c", &idx);

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

    result = mvs_path_type("TEMP.DINONFS.H/types.h", &idx);

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

    result = mvs_path_type("TEMP.DINONFS.CNTL/compile.jcl", &idx);

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

/* Path shares a prefix with host_path_ebcdic but has no '/' separator ->
 * e.g. "TEMP.DINONFS.Cextra" should NOT match "TEMP.DINONFS.C" */
static MunitResult test_prefix_without_slash_no_match(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("TEMP.DINONFS.Cextra", &idx);

    munit_assert_int(result, ==, MVS_PATH_NOT_EXPORTED);
    munit_assert_int(idx, ==, -1);
    return MUNIT_OK;
}

/* Path is a strict prefix (shorter) of host_path_ebcdic -> no match.
 * e.g. "TEMP.DINONFS" does not match "TEMP.DINONFS.C" */
static MunitResult test_shorter_than_export_no_match(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("TEMP.DINONFS", &idx);

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

/* Path that is the host_path with a trailing slash -> no exact match.
 * "TEMP.DINONFS.C/" is not a DATASET match but the length and slash
 * checks pass, so the implementation returns PDS_MEMBER. */
static MunitResult test_trailing_slash_no_match(const MunitParameter params[], void *data)
{
    int idx = -1;
    int result;
    (void)params; (void)data;

    result = mvs_path_type("TEMP.DINONFS.C/", &idx);

    /*
     * The implementation returns PDS_MEMBER for "TEMP.DINONFS.C/" because
     * path[host_path_len] == '/' and the length check passes.
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
