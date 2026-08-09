/*
 * tests/tcfgopts.c - Unit tests for cfgopts.c (export keyword options).
 *
 * Suite prefix: /cfgopts
 * Sub-suites  : /octal  /keywords  /resolve
 *
 * These are the pure config helpers behind the read-only / permission
 * keywords (design_export_options.md §12).  They have no I/O and no
 * dependency on the export table, so they link cleanly beside tstubs.
 *
 * The keyword functions only READ their token strings, so string literals
 * (char[] in C89) are safe to pass as the char* token array.
 *
 * Build (from project root):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c tests/tcfgopts.c \
 *      src/cfgopts.c src/logger.c tests/munit.c \
 *      -o tests/runall_cfgopts
 */

#include "munit.h"
#include "cfgopts.h"
#include <string.h>

/* ===================================================================== */
/* /octal -- cfg_parse_octal                                             */
/* ===================================================================== */

static MunitResult test_octal_valid(
    const MunitParameter params[], void *data)
{
    uint16_t v;
    (void)params; (void)data;

    v = 0xFFFF;
    munit_assert_int(cfg_parse_octal("0",    &v), ==, 0);
    munit_assert_int((int)v, ==, 0);

    munit_assert_int(cfg_parse_octal("755",  &v), ==, 0);
    munit_assert_int((int)v, ==, 0755);

    munit_assert_int(cfg_parse_octal("777",  &v), ==, 0);
    munit_assert_int((int)v, ==, 0777);

    munit_assert_int(cfg_parse_octal("644",  &v), ==, 0);
    munit_assert_int((int)v, ==, 0644);

    /* A leading zero is accepted (still base 8). */
    munit_assert_int(cfg_parse_octal("0755", &v), ==, 0);
    munit_assert_int((int)v, ==, 0755);
    return MUNIT_OK;
}

static MunitResult test_octal_rejects(
    const MunitParameter params[], void *data)
{
    uint16_t v;
    (void)params; (void)data;

    munit_assert_int(cfg_parse_octal("",     &v), ==, -1);  /* empty        */
    munit_assert_int(cfg_parse_octal("778",  &v), ==, -1);  /* 8 not octal  */
    munit_assert_int(cfg_parse_octal("999",  &v), ==, -1);  /* 9 not octal  */
    munit_assert_int(cfg_parse_octal("8",    &v), ==, -1);  /* 8 not octal  */
    munit_assert_int(cfg_parse_octal("7x",   &v), ==, -1);  /* trailing junk*/
    munit_assert_int(cfg_parse_octal("1777", &v), ==, -1);  /* > 0777       */
    munit_assert_int(cfg_parse_octal("4000", &v), ==, -1);  /* setuid bit   */
    return MUNIT_OK;
}

static MunitTest octal_tests[] = {
    { "/valid",   test_octal_valid,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/rejects", test_octal_rejects, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ===================================================================== */
/* /keywords -- cfg_parse_keywords                                       */
/* ===================================================================== */

static MunitResult test_kw_ro_rw(
    const MunitParameter params[], void *data)
{
    cfg_opts_t o;
    char *ro[] = { "ro" };
    char *rw[] = { "rw" };
    (void)params; (void)data;

    munit_assert_int(cfg_parse_keywords(ro, 1, &o, CFG_LEVEL_DATASET, "x"), ==, 0);
    munit_assert_int(o.has_readonly, ==, 1);
    munit_assert_int(o.readonly,     ==, 1);

    munit_assert_int(cfg_parse_keywords(rw, 1, &o, CFG_LEVEL_DATASET, "x"), ==, 0);
    munit_assert_int(o.has_readonly, ==, 1);
    munit_assert_int(o.readonly,     ==, 0);
    return MUNIT_OK;
}

static MunitResult test_kw_perms(
    const MunitParameter params[], void *data)
{
    cfg_opts_t o;
    char *toks[] = { "dirperm=755", "memperm=644" };
    (void)params; (void)data;

    munit_assert_int(cfg_parse_keywords(toks, 2, &o, CFG_LEVEL_DATASET, "x"), ==, 0);
    munit_assert_int(o.has_dirperm, ==, 1);
    munit_assert_int((int)o.dirperm, ==, 0755);
    munit_assert_int(o.has_memperm, ==, 1);
    munit_assert_int((int)o.memperm, ==, 0644);
    munit_assert_int(o.has_readonly, ==, 0);   /* untouched */
    return MUNIT_OK;
}

/* Case-insensitive, order-independent, multiple keywords in one call. */
static MunitResult test_kw_case_and_order(
    const MunitParameter params[], void *data)
{
    cfg_opts_t o;
    char *toks[] = { "DirPerm=700", "RO", "MEMPERM=400" };
    (void)params; (void)data;

    munit_assert_int(cfg_parse_keywords(toks, 3, &o, CFG_LEVEL_DATASET, "x"), ==, 0);
    munit_assert_int((int)o.dirperm, ==, 0700);
    munit_assert_int(o.readonly,     ==, 1);
    munit_assert_int((int)o.memperm, ==, 0400);
    return MUNIT_OK;
}

/* rootperm is valid at export level, an error at dataset level. */
static MunitResult test_kw_rootperm_level(
    const MunitParameter params[], void *data)
{
    cfg_opts_t o;
    char *toks[] = { "rootperm=555" };
    (void)params; (void)data;

    munit_assert_int(cfg_parse_keywords(toks, 1, &o, CFG_LEVEL_EXPORT, "x"), ==, 0);
    munit_assert_int(o.has_rootperm, ==, 1);
    munit_assert_int((int)o.rootperm, ==, 0555);

    munit_assert_int(cfg_parse_keywords(toks, 1, &o, CFG_LEVEL_DATASET, "x"), ==, -1);
    return MUNIT_OK;
}

static MunitResult test_kw_unknown(
    const MunitParameter params[], void *data)
{
    cfg_opts_t o;
    char *toks[] = { "ro", "bogus" };
    (void)params; (void)data;

    munit_assert_int(cfg_parse_keywords(toks, 2, &o, CFG_LEVEL_DATASET, "x"), ==, -1);
    return MUNIT_OK;
}

static MunitResult test_kw_bad_value(
    const MunitParameter params[], void *data)
{
    cfg_opts_t o;
    char *toks[] = { "dirperm=778" };
    (void)params; (void)data;

    munit_assert_int(cfg_parse_keywords(toks, 1, &o, CFG_LEVEL_DATASET, "x"), ==, -1);
    return MUNIT_OK;
}

/* fileext=<ext>: stored lower-case, valid at either level; empty or
   over-long is rejected. */
static MunitResult test_kw_fileext(
    const MunitParameter params[], void *data)
{
    cfg_opts_t o;
    char *ok[]      = { "FileExt=JCL" };
    char *empty[]   = { "fileext=" };
    char *toolong[] = { "fileext=0123456789abcdef" };  /* 16 chars > 15 max */
    char *dotted[]  = { "fileext=tar.gz" };             /* '.' breaks parsing */
    (void)params; (void)data;

    munit_assert_int(cfg_parse_keywords(ok, 1, &o, CFG_LEVEL_DATASET, "x"), ==, 0);
    munit_assert_int(o.has_fileext, ==, 1);
    munit_assert_string_equal(o.fileext, "jcl");   /* lower-cased */

    /* Also valid at export level (unlike rootperm). */
    munit_assert_int(cfg_parse_keywords(ok, 1, &o, CFG_LEVEL_EXPORT, "x"), ==, 0);
    munit_assert_int(o.has_fileext, ==, 1);

    munit_assert_int(cfg_parse_keywords(empty, 1, &o, CFG_LEVEL_DATASET, "x"), ==, -1);
    munit_assert_int(cfg_parse_keywords(toolong, 1, &o, CFG_LEVEL_DATASET, "x"), ==, -1);
    munit_assert_int(cfg_parse_keywords(dotted, 1, &o, CFG_LEVEL_DATASET, "x"), ==, -1);
    return MUNIT_OK;
}

/* nofileext: bare boolean; mutually exclusive with fileext= on one line. */
static MunitResult test_kw_nofileext(
    const MunitParameter params[], void *data)
{
    cfg_opts_t o;
    char *ok[]   = { "NoFileExt" };
    char *both[] = { "fileext=jcl", "nofileext" };
    (void)params; (void)data;

    munit_assert_int(cfg_parse_keywords(ok, 1, &o, CFG_LEVEL_DATASET, "x"), ==, 0);
    munit_assert_int(o.has_nofileext, ==, 1);
    munit_assert_int(o.has_fileext,   ==, 0);

    /* fileext= and nofileext together is contradictory. */
    munit_assert_int(cfg_parse_keywords(both, 2, &o, CFG_LEVEL_DATASET, "x"), ==, -1);
    return MUNIT_OK;
}

/* No keywords -> success, nothing set. */
static MunitResult test_kw_empty(
    const MunitParameter params[], void *data)
{
    cfg_opts_t o;
    char *toks[1];
    (void)params; (void)data;

    munit_assert_int(cfg_parse_keywords(toks, 0, &o, CFG_LEVEL_DATASET, "x"), ==, 0);
    munit_assert_int(o.has_readonly,  ==, 0);
    munit_assert_int(o.has_dirperm,   ==, 0);
    munit_assert_int(o.has_memperm,   ==, 0);
    munit_assert_int(o.has_rootperm,  ==, 0);
    munit_assert_int(o.has_fileext,   ==, 0);
    munit_assert_int(o.has_nofileext, ==, 0);
    return MUNIT_OK;
}

static MunitTest keyword_tests[] = {
    { "/ro_rw",         test_kw_ro_rw,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/perms",         test_kw_perms,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/case_and_order", test_kw_case_and_order, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/rootperm_level", test_kw_rootperm_level, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/unknown",       test_kw_unknown,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/bad_value",     test_kw_bad_value,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/fileext",       test_kw_fileext,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nofileext",     test_kw_nofileext,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/empty",         test_kw_empty,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ===================================================================== */
/* /resolve -- cfg_resolve_opts                                          */
/* ===================================================================== */

/* Build an empty (all-absent) option set. */
static void opts_clear(cfg_opts_t *o) { memset(o, 0, sizeof(*o)); }

/* No keywords anywhere -> the documented defaults. */
static MunitResult test_resolve_defaults(
    const MunitParameter params[], void *data)
{
    cfg_opts_t ds;
    uint8_t  ro;
    uint16_t dir, mem;
    char     ext[CFG_FILEEXT_MAX];
    (void)params; (void)data;

    opts_clear(&ds);
    strcpy(ext, "cbl");   /* the dsname-derived default, passed in */
    munit_assert_int(cfg_resolve_opts(NULL, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_int((int)ro,  ==, 0);
    munit_assert_int((int)dir, ==, 0777);
    munit_assert_int((int)mem, ==, 0777);
    munit_assert_string_equal(ext, "cbl");   /* no keyword -> default kept */
    return MUNIT_OK;
}

/* Export-level perms are inherited; a dataset overrides them. */
static MunitResult test_resolve_inherit_and_override(
    const MunitParameter params[], void *data)
{
    cfg_opts_t exp, ds;
    uint8_t  ro;
    uint16_t dir, mem;
    char     ext[CFG_FILEEXT_MAX];
    (void)params; (void)data;

    opts_clear(&exp);
    exp.has_dirperm = 1; exp.dirperm = 0555;
    exp.has_memperm = 1; exp.memperm = 0444;

    /* Dataset inherits both. */
    opts_clear(&ds);
    ext[0] = '\0';
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_int((int)dir, ==, 0555);
    munit_assert_int((int)mem, ==, 0444);

    /* Dataset overrides dirperm, still inherits memperm. */
    opts_clear(&ds);
    ds.has_dirperm = 1; ds.dirperm = 0700;
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_int((int)dir, ==, 0700);
    munit_assert_int((int)mem, ==, 0444);
    return MUNIT_OK;
}

/* Export ro is a ceiling inherited by datasets. */
static MunitResult test_resolve_ro_ceiling(
    const MunitParameter params[], void *data)
{
    cfg_opts_t exp, ds;
    uint8_t  ro;
    uint16_t dir, mem;
    char     ext[CFG_FILEEXT_MAX];
    (void)params; (void)data;

    opts_clear(&exp);
    exp.has_readonly = 1; exp.readonly = 1;

    opts_clear(&ds);
    ext[0] = '\0';
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_int((int)ro, ==, 1);
    return MUNIT_OK;
}

/* A dataset may narrow to ro inside a read-write export. */
static MunitResult test_resolve_dataset_ro(
    const MunitParameter params[], void *data)
{
    cfg_opts_t ds;
    uint8_t  ro;
    uint16_t dir, mem;
    char     ext[CFG_FILEEXT_MAX];
    (void)params; (void)data;

    opts_clear(&ds);
    ds.has_readonly = 1; ds.readonly = 1;
    ext[0] = '\0';
    munit_assert_int(cfg_resolve_opts(NULL, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_int((int)ro, ==, 1);
    return MUNIT_OK;
}

/* 'rw' on a dataset inside an 'ro' export is an error; 'rw' inside a
   read-write export is fine. */
static MunitResult test_resolve_rw_in_ro_is_error(
    const MunitParameter params[], void *data)
{
    cfg_opts_t exp, ds;
    uint8_t  ro;
    uint16_t dir, mem;
    char     ext[CFG_FILEEXT_MAX];
    (void)params; (void)data;

    opts_clear(&exp);
    exp.has_readonly = 1; exp.readonly = 1;
    opts_clear(&ds);
    ds.has_readonly = 1; ds.readonly = 0;   /* rw */

    ext[0] = '\0';
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, -1);

    /* Same rw dataset, but inside a read-write export -> allowed. */
    opts_clear(&exp);
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_int((int)ro, ==, 0);
    return MUNIT_OK;
}

/* fileext: dsname-derived default kept when absent; export sets a default;
   dataset overrides the export default. */
static MunitResult test_resolve_fileext(
    const MunitParameter params[], void *data)
{
    cfg_opts_t exp, ds;
    uint8_t  ro;
    uint16_t dir, mem;
    char     ext[CFG_FILEEXT_MAX];
    (void)params; (void)data;

    /* Export-level fileext becomes the default for a dataset with none. */
    opts_clear(&exp);
    exp.has_fileext = 1; strcpy(exp.fileext, "jcl");
    opts_clear(&ds);
    strcpy(ext, "cntl");   /* derived default -- overridden by the export */
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_string_equal(ext, "jcl");

    /* Dataset-level fileext overrides the export default. */
    opts_clear(&ds);
    ds.has_fileext = 1; strcpy(ds.fileext, "asm");
    strcpy(ext, "cntl");
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_string_equal(ext, "asm");

    /* Neither set -> the derived default passed in is kept. */
    opts_clear(&exp);
    opts_clear(&ds);
    strcpy(ext, "cntl");
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_string_equal(ext, "cntl");
    return MUNIT_OK;
}

/* nofileext clears the extension; dataset level wins over export level, and
   overrides an export fileext= (and vice-versa). */
static MunitResult test_resolve_nofileext(
    const MunitParameter params[], void *data)
{
    cfg_opts_t exp, ds;
    uint8_t  ro;
    uint16_t dir, mem;
    char     ext[CFG_FILEEXT_MAX];
    (void)params; (void)data;

    /* Dataset nofileext clears the dsname-derived default. */
    opts_clear(&ds);
    ds.has_nofileext = 1;
    strcpy(ext, "samplib");
    munit_assert_int(cfg_resolve_opts(NULL, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_string_equal(ext, "");

    /* Export nofileext, dataset silent -> cleared. */
    opts_clear(&exp);
    exp.has_nofileext = 1;
    opts_clear(&ds);
    strcpy(ext, "samplib");
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_string_equal(ext, "");

    /* Export fileext=jcl, dataset nofileext -> dataset wins (cleared). */
    opts_clear(&exp);
    exp.has_fileext = 1; strcpy(exp.fileext, "jcl");
    opts_clear(&ds);
    ds.has_nofileext = 1;
    strcpy(ext, "samplib");
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_string_equal(ext, "");

    /* Export nofileext, dataset fileext=asm -> dataset wins (asm). */
    opts_clear(&exp);
    exp.has_nofileext = 1;
    opts_clear(&ds);
    ds.has_fileext = 1; strcpy(ds.fileext, "asm");
    strcpy(ext, "samplib");
    munit_assert_int(cfg_resolve_opts(&exp, &ds, &ro, &dir, &mem, ext, "x"), ==, 0);
    munit_assert_string_equal(ext, "asm");
    return MUNIT_OK;
}

static MunitTest resolve_tests[] = {
    { "/defaults",            test_resolve_defaults,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/inherit_and_override", test_resolve_inherit_and_override, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/ro_ceiling",          test_resolve_ro_ceiling,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/dataset_ro",          test_resolve_dataset_ro,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/rw_in_ro_is_error",   test_resolve_rw_in_ro_is_error,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/fileext",             test_resolve_fileext,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nofileext",           test_resolve_nofileext,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ===================================================================== */
/* Suite registration                                                    */
/* ===================================================================== */

static MunitSuite sub_suites[] = {
    { "/octal",    octal_tests,   NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/keywords", keyword_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/resolve",  resolve_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tcfgopts_suite = {
    "/cfgopts",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
