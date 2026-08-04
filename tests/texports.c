/*
 * tests/texports.c - Unit tests for exports.c (the NFSDCONF parser + table).
 *
 * Suite prefix: /exports
 * Sub-suites  : /parse /multi /options /failclosed /lookup
 *
 * WHY THIS IS A STANDALONE PROGRAM (not part of runall)
 * -----------------------------------------------------
 * Every other test links tests/tstubs.c, which REPLACES exports.c -- it defines
 * exports_count / exports_get / export_dataset_* etc. so the modules under test
 * can resolve them without the real export table.  This suite tests the REAL
 * exports.c, so it must NOT link tstubs (the two collide on those symbols).  It
 * therefore has its own main() and its own build (see below), separate from
 * runall.  This is also the module that had NO coverage while a multi-export
 * regression scare played out -- test_multi/two_export_paths is the guard.
 *
 * HOW IT DRIVES THE PARSER
 * ------------------------
 * exports_load() takes a file path and fopen()s it, so each test writes a small
 * config to a temp file and then inspects the result through the public
 * accessors.  exports_load() resets the table (g_nexports = 0) on entry, so the
 * tests are independent.  Its MVS-only dependencies -- mvs_dscb(),
 * blkcalc_dataset_init() and the two DSCB field formatters -- are stubbed
 * below, so no real dataset is needed and the VTOC is never touched.  That
 * matters now that a dataset whose DSCB cannot be read FAILS its whole export
 * (see cfg_load_dscb_info): unstubbed, every test here would see an empty
 * table.
 *
 * Encoding: config text and the expected dsname/keyword strings both use native
 * char literals, so a byte written as 'A' is compared against the same native
 * 'A' -- correct on the ASCII dev host and on EBCDIC MVS alike.  Only the
 * numeric perm values and the derived file_ext are checked against literals,
 * both of which are charset-stable through the parser.
 *
 * Build (dev host):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/texports.c src/exports.c src/cfgopts.c src/ebcdic.c src/logger.c \
 *      tests/munit.c -o tests/runexports
 *   tests/runexports
 *
 * On MVS: tests-jcl/testexp.jcl compiles + links EXPORTS/CFGOPTS/EBCDIC/LOGGER
 * with TEXPORTS and MUNIT into a standalone module and runs it.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <stdio.h>
#include <string.h>

#include "munit.h"
#include "nfsd.h"      /* export_t / pds_dataset_t + exports_* accessors */

#ifdef __MVS__
#include "asmutils.h"  /* mvs_dscb_info_t, MVS_DSCB_* (for the stubs)      */
#include "mvsblkc.h"   /* blkcalc_dataset_init + its 8-char alias          */
#include "mvsutl.h"    /* mvs_dscb_dsorg_str / mvs_dscb_recfm_str, MVS_DEV_*/
#endif

/* ------------------------------------------------------------------ */
/* Link stubs -- MVS only                                              */
/*                                                                     */
/* Under __MVS__, exports_load() runs cfg_load_dscb_info(), which reads */
/* the VTOC for every exported dataset and FAILS any export it cannot   */
/* account for; cfg_drop_failed_exports() then removes it.  The configs */
/* these tests write name datasets that do not exist, so without these  */
/* stubs every test would find an empty export table.                   */
/*                                                                     */
/* Stubbing rather than linking the real thing is deliberate: the real  */
/* blkcalc_dataset_init() lives in MVSBLKC, which needs pww_slot_at()   */
/* and pww_read_range() and so drags MVSPWW -> MVSSPL / MVSPWFL /       */
/* MVSDALC / MVSENQ / MVSSTOW ... i.e. most of the server, into a job   */
/* whose whole purpose is the config parser.  The real decoder has its  */
/* own coverage in tests/tmvsblkc.c (/dsinit).                          */
/* ------------------------------------------------------------------ */
#ifdef __MVS__

/* Report a plausible PO / FB 80 / 8000 dataset on a 3390 for every name
   asked about, so every export survives validation. */
int mvs_dscb(uint8_t request_type, uint8_t options,
             const char **dsnlist, mvs_dscb_info_t *data)
{
    int i;

    (void)request_type;
    (void)options;
    if (dsnlist == NULL || data == NULL)
        return 8;

    for (i = 0; dsnlist[i] != NULL; i++) {
        mvs_dscb_info_t *e = &data[i];

        memset(e, 0, sizeof(*e));
        e->status     = MVS_DSCB_ST_OK;
        memcpy(e->volser, "WORK01", 6);
        e->nextents   = 1;
        e->tracks     = 100;
        e->dsorg[0]   = MVS_DSCB_DSORG_PO;
        e->recfm      = (uint8_t)(MVS_DSCB_RECFM_F | MVS_DSCB_RECFM_BLK);
        e->blksize[0] = (uint8_t)(8000 >> 8);
        e->blksize[1] = (uint8_t)(8000 & 0xFF);
        e->lrecl[1]   = 80;
        e->devtype[3] = MVS_DEV_3390;
        e->trklen[0]  = (uint8_t)(58786 >> 8);
        e->trklen[1]  = (uint8_t)(58786 & 0xFF);
        e->trkcyl[1]  = 15;
    }
    return 0;
}

/* Leave a VALID dataset behind so the export is not dropped.  The figures
   only have to be self-consistent; nothing in exports.c does arithmetic
   with them beyond logging. */
int blkcalc_dataset_init(dataset_dscb_info_t *out, const mvs_dscb_info_t *raw)
{
    (void)raw;
    if (out == NULL)
        return -1;

    memset(out, 0, sizeof(*out));
    out->valid            = 1;
    out->dsorg            = MVS_DSCB_DSORG_PO;
    out->recfm            = (uint8_t)(MVS_DSCB_RECFM_F | MVS_DSCB_RECFM_BLK);
    out->blksize          = 8000;
    out->lrecl            = 80;
    out->tracks           = 100;
    out->nextents         = 1;
    out->blocks_per_track = 6;
    strcpy(out->volser, "WORK01");
    return 0;
}

char *mvs_dscb_dsorg_str(uint8_t dsorg, char *str)
{
    (void)dsorg;
    strcpy(str, "PO");
    return str;
}

char *mvs_dscb_recfm_str(uint8_t recfm, char *str)
{
    (void)recfm;
    strcpy(str, "FB");
    return str;
}

#endif /* __MVS__ */

/* ------------------------------------------------------------------ */
/* Temp config file: written with native-encoding text, then handed   */
/* to exports_load() by path.                                          */
/* ------------------------------------------------------------------ */
/* The temp config path is the SAME for read and write.  On MVS the DCB and
   allocation attributes go in the fopen MODE string, NOT appended to the
   filename (that was the first-cut bug -- fopen returned NULL), matching the
   proven scratch-dataset pattern in mvsspl.c. */
#ifdef __MVS__
#define TEXP_CFG    "//DSN:&&TEXPCFG"
#define TEXP_WMODE  "w,pri=1,sec=1,unit=sysda,dsorg=ps,recfm=fb," \
                    "lrecl=255,blksize=6120"
#else
#define TEXP_CFG    "texports_tmp.cfg"
#define TEXP_WMODE  "w"
#endif

/* Write 'content' to the temp config and load it.  Returns exports_load()'s
   count so a test can assert on it directly. */
static int load_config(const char *content)
{
    FILE *f = fopen(TEXP_CFG, TEXP_WMODE);
    munit_assert_ptr_not_null(f);
    fputs(content, f);
    fclose(f);
    return exports_load(TEXP_CFG);
}

/* Find a loaded export by its (native) NFS path via the EBCDIC field, so no
   ASCII conversion is needed.  Returns NULL if absent. */
static export_t *exp_by_path(const char *path_native)
{
    int i;
    int n = exports_count();
    for (i = 0; i < n; i++) {
        export_t *e = exports_get(i);
        if (e != NULL && strcmp(e->export_path_ebcdic, path_native) == 0)
            return e;
    }
    return NULL;
}

/* Find a dataset within an export by its (native) dsname.  NULL if absent. */
static pds_dataset_t *ds_by_name(export_t *e, const char *dsname_native)
{
    int i;
    if (e == NULL)
        return NULL;
    for (i = 0; i < e->ndatasets; i++) {
        if (strcmp(e->datasets[i].dsname_ebcdic, dsname_native) == 0)
            return &e->datasets[i];
    }
    return NULL;
}

/* ==================================================================== */
/* /parse -- basic parsing                                              */
/* ==================================================================== */

static MunitResult test_single(const MunitParameter params[], void *data)
{
    export_t *e;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports  TEST.DS.ONE\n"), ==, 1);

    munit_assert_int(exports_count(), ==, 1);
    e = exp_by_path("/exports");
    munit_assert_ptr_not_null(e);
    munit_assert_int(e->ndatasets, ==, 1);
    munit_assert_ptr_not_null(ds_by_name(e, "TEST.DS.ONE"));
    return MUNIT_OK;
}

/* Comments (#) and blank lines are skipped; the count is unaffected. */
static MunitResult test_comments_blanks(const MunitParameter params[], void *data)
{
    export_t *e;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "# a comment\n"
        "[Exports]\n"
        "\n"
        "# another comment\n"
        "/exports  TEST.DS.ONE\n"
        "   \n"), ==, 1);

    e = exp_by_path("/exports");
    munit_assert_ptr_not_null(e);
    munit_assert_int(e->ndatasets, ==, 1);
    return MUNIT_OK;
}

static MunitResult test_empty(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    munit_assert_int(load_config("[Exports]\n"), ==, 0);
    munit_assert_int(exports_count(), ==, 0);
    return MUNIT_OK;
}

static MunitTest parse_tests[] = {
    { "/single",          test_single,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/comments_blanks", test_comments_blanks, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/empty",           test_empty,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /multi -- multiple datasets and multiple export paths               */
/* ==================================================================== */

/* Several datasets under ONE export path accumulate into one export. */
static MunitResult test_datasets_accumulate(const MunitParameter params[], void *data)
{
    export_t *e;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports  TEST.DS.ONE\n"
        "/exports  TEST.DS.TWO\n"
        "/exports  TEST.DS.THREE\n"), ==, 1);

    e = exp_by_path("/exports");
    munit_assert_ptr_not_null(e);
    munit_assert_int(e->ndatasets, ==, 3);
    munit_assert_ptr_not_null(ds_by_name(e, "TEST.DS.ONE"));
    munit_assert_ptr_not_null(ds_by_name(e, "TEST.DS.TWO"));
    munit_assert_ptr_not_null(ds_by_name(e, "TEST.DS.THREE"));
    return MUNIT_OK;
}

/* TWO distinct export paths produce TWO exports, each with its own datasets.
   This is the case that broke (as a stale build) and had no coverage. */
static MunitResult test_two_export_paths(const MunitParameter params[], void *data)
{
    export_t *a;
    export_t *b;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports   TEST.DS.ONE\n"
        "/exports   TEST.DS.TWO\n"
        "/iexports  ITEST.DS.ONE\n"), ==, 2);

    munit_assert_int(exports_count(), ==, 2);

    a = exp_by_path("/exports");
    b = exp_by_path("/iexports");
    munit_assert_ptr_not_null(a);
    munit_assert_ptr_not_null(b);
    munit_assert_int(a != b, ==, 1);               /* distinct slots */

    munit_assert_int(a->ndatasets, ==, 2);
    munit_assert_int(b->ndatasets, ==, 1);
    munit_assert_ptr_not_null(ds_by_name(a, "TEST.DS.ONE"));
    munit_assert_ptr_not_null(ds_by_name(a, "TEST.DS.TWO"));
    munit_assert_ptr_not_null(ds_by_name(b, "ITEST.DS.ONE"));
    /* the second path's dataset did NOT leak into the first */
    munit_assert_ptr_null(ds_by_name(a, "ITEST.DS.ONE"));
    return MUNIT_OK;
}

static MunitTest multi_tests[] = {
    { "/datasets_accumulate", test_datasets_accumulate, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/two_export_paths",    test_two_export_paths,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /options -- ro / perms / fileext / nofileext                        */
/* ==================================================================== */

static MunitResult test_ro_and_perms(const MunitParameter params[], void *data)
{
    pds_dataset_t *ro;
    pds_dataset_t *rw;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports  TEST.RO   ro\n"
        "/exports  TEST.RW   dirperm=751 memperm=640\n"), ==, 1);

    ro = ds_by_name(exp_by_path("/exports"), "TEST.RO");
    rw = ds_by_name(exp_by_path("/exports"), "TEST.RW");
    munit_assert_ptr_not_null(ro);
    munit_assert_ptr_not_null(rw);

    munit_assert_int(ro->readonly, ==, 1);
    munit_assert_int(rw->readonly, ==, 0);
    munit_assert_int((int)rw->dirperm, ==, 0751);
    munit_assert_int((int)rw->memperm, ==, 0640);
    return MUNIT_OK;
}

static MunitResult test_fileext(const MunitParameter params[], void *data)
{
    pds_dataset_t *ds;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports  TEST.WITH.JCL   fileext=jcl\n"), ==, 1);

    ds = ds_by_name(exp_by_path("/exports"), "TEST.WITH.JCL");
    munit_assert_ptr_not_null(ds);
    munit_assert_string_equal(ds->file_ext, "jcl");
    return MUNIT_OK;
}

/* No keyword: the extension defaults to the lower-cased last qualifier. */
static MunitResult test_fileext_default(const MunitParameter params[], void *data)
{
    pds_dataset_t *ds;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports  TEST.DATA.CNTL\n"), ==, 1);

    ds = ds_by_name(exp_by_path("/exports"), "TEST.DATA.CNTL");
    munit_assert_ptr_not_null(ds);
    munit_assert_string_equal(ds->file_ext, "cntl");
    return MUNIT_OK;
}

static MunitResult test_nofileext(const MunitParameter params[], void *data)
{
    pds_dataset_t *ds;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports  TEST.NOEXT   nofileext\n"), ==, 1);

    ds = ds_by_name(exp_by_path("/exports"), "TEST.NOEXT");
    munit_assert_ptr_not_null(ds);
    munit_assert_string_equal(ds->file_ext, "");
    return MUNIT_OK;
}

static MunitTest option_tests[] = {
    { "/ro_and_perms",   test_ro_and_perms,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/fileext",        test_fileext,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/fileext_default", test_fileext_default, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nofileext",      test_nofileext,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /failclosed -- a bad line drops its WHOLE export, others survive     */
/* ==================================================================== */

/* An unknown keyword fails the affected export only. */
static MunitResult test_bad_keyword(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/bad   TEST.BAD   boguskeyword\n"
        "/good  TEST.GOOD  ro\n"), ==, 1);

    munit_assert_ptr_null(exp_by_path("/bad"));        /* dropped whole */
    munit_assert_ptr_not_null(exp_by_path("/good"));   /* survived      */
    return MUNIT_OK;
}

/* A bad option VALUE also fails the export (fail-closed, not fail-open). */
static MunitResult test_bad_value(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/bad   TEST.BAD   memperm=999\n"     /* 9 is not an octal digit */
        "/good  TEST.GOOD\n"), ==, 1);

    munit_assert_ptr_null(exp_by_path("/bad"));
    munit_assert_ptr_not_null(exp_by_path("/good"));
    return MUNIT_OK;
}

/* One bad dataset drops the ENTIRE export, including its good datasets. */
static MunitResult test_bad_drops_whole_export(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports  TEST.GOOD.ONE\n"
        "/exports  TEST.BAD      nope\n"
        "/exports  TEST.GOOD.TWO\n"), ==, 0);

    munit_assert_int(exports_count(), ==, 0);          /* the whole export gone */
    return MUNIT_OK;
}

static MunitTest failclosed_tests[] = {
    { "/bad_keyword",            test_bad_keyword,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/bad_value",              test_bad_value,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/bad_drops_whole_export", test_bad_drops_whole_export, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /lookup -- exports_get_id / exports_find_by_nfs_path                 */
/* ==================================================================== */

static MunitResult test_get_id(const MunitParameter params[], void *data)
{
    int i;
    int n;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports   TEST.DS.ONE\n"
        "/iexports  ITEST.DS.ONE\n"), ==, 2);

    n = exports_count();
    for (i = 0; i < n; i++)                            /* round-trip index<->ptr */
        munit_assert_int(exports_get_id(exports_get(i)), ==, i);
    return MUNIT_OK;
}

/* exports_find_by_nfs_path resolves each export's own (ASCII) path back to
   itself and distinguishes the two -- pass the stored path so no literal
   ASCII/EBCDIC conversion is needed. */
static MunitResult test_find_by_path(const MunitParameter params[], void *data)
{
    export_t *a;
    export_t *b;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports   TEST.DS.ONE\n"
        "/iexports  ITEST.DS.ONE\n"), ==, 2);

    a = exp_by_path("/exports");
    b = exp_by_path("/iexports");
    munit_assert_ptr_not_null(a);
    munit_assert_ptr_not_null(b);

    munit_assert_ptr_equal(exports_find_by_nfs_path(a->export_path), a);
    munit_assert_ptr_equal(exports_find_by_nfs_path(b->export_path), b);
    munit_assert_int(exports_find_by_nfs_path(a->export_path) != b, ==, 1);
    return MUNIT_OK;
}

/* The dataset provider agrees with the export it belongs to. */
static MunitResult test_dataset_provider(const MunitParameter params[], void *data)
{
    export_t *a;
    int       id;
    (void)params; (void)data;

    munit_assert_int(load_config(
        "[Exports]\n"
        "/exports  TEST.DS.ONE\n"
        "/exports  TEST.DS.TWO\n"), ==, 1);

    a  = exp_by_path("/exports");
    id = exports_get_id(a);
    munit_assert_int(export_dataset_count(id), ==, 2);
    munit_assert_ptr_equal(export_dataset_get(id, 0), &a->datasets[0]);
    munit_assert_ptr_null(export_dataset_get(id, 5));     /* out of range */
    return MUNIT_OK;
}

static MunitTest lookup_tests[] = {
    { "/get_id",           test_get_id,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/find_by_path",     test_find_by_path,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/dataset_provider", test_dataset_provider, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite + standalone main                                              */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/parse",      parse_tests,      NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/multi",      multi_tests,      NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/options",    option_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/failclosed", failclosed_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/lookup",     lookup_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite texports_suite = {
    "/exports",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[])
{
    return munit_suite_main(&texports_suite, NULL, argc, (char * const *)argv);
}
