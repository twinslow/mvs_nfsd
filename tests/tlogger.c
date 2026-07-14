/*
 * tests/tlogger.c - Unit tests for logger.c (log levels + per-proc table
 *                   + the "SET LOGLVL" MODIFY command parser).
 *
 * Suite prefix: /logger
 * Sub-suites  : /level  /global  /proc  /modify
 *
 * The parser (log_handle_modify) compares operator tokens against literal
 * strings with plain char literals, so it is encoding-correct on both the
 * ASCII dev host and the EBCDIC MVS target: the command strings below are
 * C string literals in the same encoding as the code that inspects them.
 *
 * These tests only assert on state that logger.c exposes through its API
 * (log_get_level / log_proc_get_level and the return codes).  The actual
 * log output goes to stderr / WTO and is not captured here.
 *
 * Build (from project root):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c \
 *      tests/tlogger.c \
 *      src/logger.c tests/munit.c \
 *      -o tests/runall_logger
 */

#include "munit.h"
#include "logger.h"
#include <string.h>

/* ==================================================================== */
/* /level -- severity ordinal ordering                                   */
/* ==================================================================== */

/*
 * TRACE must sit strictly between DEBUG and INFO, and the ladder must be
 * strictly increasing.  The emit filter and the WTO threshold both rely
 * on this ordering.
 */
static MunitResult test_level_ordering(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int((int)LOG_DEBUG, <, (int)LOG_TRACE);
    munit_assert_int((int)LOG_TRACE, <, (int)LOG_INFO);
    munit_assert_int((int)LOG_INFO,  <, (int)LOG_WARN);
    munit_assert_int((int)LOG_WARN,  <, (int)LOG_ERROR);
    munit_assert_int((int)LOG_ERROR, <, (int)LOG_FATAL);
    return MUNIT_OK;
}

/*
 * TRACE is below INFO, so it stays under the "INFO and above -> WTO"
 * operator-console threshold.  Guard that relationship explicitly.
 */
static MunitResult test_level_trace_below_info(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int((int)LOG_TRACE, ==, (int)LOG_DEBUG + 1);
    munit_assert_int((int)LOG_INFO,  ==, (int)LOG_TRACE + 1);
    return MUNIT_OK;
}

static MunitTest level_tests[] = {
    { "/ordering",         test_level_ordering,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/trace_below_info", test_level_trace_below_info, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /global -- log_set_level / log_get_level                              */
/* ==================================================================== */

static MunitResult test_global_set_get_roundtrip(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    log_set_level(LOG_WARN);
    munit_assert_int((int)log_get_level(), ==, (int)LOG_WARN);

    log_set_level(LOG_DEBUG);
    munit_assert_int((int)log_get_level(), ==, (int)LOG_DEBUG);
    return MUNIT_OK;
}

static MunitTest global_tests[] = {
    { "/set_get_roundtrip", test_global_set_get_roundtrip, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /wto -- console (WTO) threshold                                       */
/* ==================================================================== */

static MunitResult test_wto_set_get_roundtrip(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    log_set_wto_level(LOG_DEBUG);
    munit_assert_int((int)log_get_wto_level(), ==, (int)LOG_DEBUG);

    log_set_wto_level(LOG_ERROR);
    munit_assert_int((int)log_get_wto_level(), ==, (int)LOG_ERROR);
    return MUNIT_OK;
}

/*
 * The two thresholds are stored separately (settable without disturbing
 * each other).  Emission still caps the console at the stream level --
 * effective console floor is max(stream, WTO) -- but that behaviour lives
 * in vlog_msg's WTO path, which writes to the console and cannot be
 * observed from a unit test; here we only assert the stored values.
 */
static MunitResult test_wto_stored_separately(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    log_set_level(LOG_WARN);
    log_set_wto_level(LOG_DEBUG);

    munit_assert_int((int)log_get_level(),     ==, (int)LOG_WARN);
    munit_assert_int((int)log_get_wto_level(), ==, (int)LOG_DEBUG);
    return MUNIT_OK;
}

static MunitTest wto_tests[] = {
    { "/set_get_roundtrip",  test_wto_set_get_roundtrip,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/stored_separately",  test_wto_stored_separately,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /proc -- per-procedure level table                                    */
/* ==================================================================== */

/* After init every slot inherits, so it reports the global level. */
static MunitResult test_proc_init_inherits_global(
    const MunitParameter params[], void *data)
{
    int i;
    (void)params; (void)data;

    log_proc_init();
    log_set_level(LOG_INFO);

    for (i = 0; i < LOG_PROC_COUNT; i++)
        munit_assert_int((int)log_proc_get_level(i), ==, (int)LOG_INFO);
    return MUNIT_OK;
}

/* A pinned slot reports its own level; unpinned slots still inherit. */
static MunitResult test_proc_pin_is_isolated(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    log_proc_init();
    log_set_level(LOG_INFO);

    rc = log_proc_set_level(LOG_PROC_WRITE, LOG_TRACE);
    munit_assert_int(rc, ==, 0);

    munit_assert_int((int)log_proc_get_level(LOG_PROC_WRITE), ==, (int)LOG_TRACE);
    munit_assert_int((int)log_proc_get_level(LOG_PROC_READ),  ==, (int)LOG_INFO);
    return MUNIT_OK;
}

/* Changing the global moves inheriting slots but not a pinned one. */
static MunitResult test_proc_global_change_respects_pin(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    log_proc_init();
    log_set_level(LOG_INFO);
    log_proc_set_level(LOG_PROC_WRITE, LOG_TRACE);

    log_set_level(LOG_ERROR);

    /* READ inherits -> follows the new global. */
    munit_assert_int((int)log_proc_get_level(LOG_PROC_READ),  ==, (int)LOG_ERROR);
    /* WRITE is pinned -> unchanged. */
    munit_assert_int((int)log_proc_get_level(LOG_PROC_WRITE), ==, (int)LOG_TRACE);
    return MUNIT_OK;
}

/* Clearing a pin (INHERIT) makes the slot follow the global again. */
static MunitResult test_proc_clear_pin_restores_inherit(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    log_proc_init();
    log_set_level(LOG_ERROR);
    log_proc_set_level(LOG_PROC_WRITE, LOG_TRACE);
    munit_assert_int((int)log_proc_get_level(LOG_PROC_WRITE), ==, (int)LOG_TRACE);

    rc = log_proc_set_level(LOG_PROC_WRITE, LOG_LEVEL_INHERIT);
    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)log_proc_get_level(LOG_PROC_WRITE), ==, (int)LOG_ERROR);
    return MUNIT_OK;
}

/* Out-of-range proc / level values are rejected. */
static MunitResult test_proc_set_level_rejects_bad_args(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    log_proc_init();

    munit_assert_int(log_proc_set_level(-1,             LOG_INFO), ==, -1);
    munit_assert_int(log_proc_set_level(LOG_PROC_COUNT, LOG_INFO), ==, -1);
    /* Out-of-range level (not a real level and not INHERIT). */
    munit_assert_int(log_proc_set_level(LOG_PROC_WRITE, (int)LOG_FATAL + 1), ==, -1);
    munit_assert_int(log_proc_set_level(LOG_PROC_WRITE, -5), ==, -1);
    /* INHERIT (-1) is a legal level value, though. */
    munit_assert_int(log_proc_set_level(LOG_PROC_WRITE, LOG_LEVEL_INHERIT), ==, 0);
    return MUNIT_OK;
}

/* get_level on an out-of-range slot falls back to the global. */
static MunitResult test_proc_get_level_bad_slot_is_global(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    log_proc_init();
    log_set_level(LOG_WARN);

    munit_assert_int((int)log_proc_get_level(-1),             ==, (int)LOG_WARN);
    munit_assert_int((int)log_proc_get_level(LOG_PROC_COUNT), ==, (int)LOG_WARN);
    return MUNIT_OK;
}

static MunitTest proc_tests[] = {
    { "/init_inherits_global",       test_proc_init_inherits_global,       NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/pin_is_isolated",            test_proc_pin_is_isolated,            NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/global_change_respects_pin", test_proc_global_change_respects_pin, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/clear_pin_restores_inherit", test_proc_clear_pin_restores_inherit, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/set_level_rejects_bad_args", test_proc_set_level_rejects_bad_args, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/get_level_bad_slot_is_global", test_proc_get_level_bad_slot_is_global, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /modify -- log_handle_modify command parser                           */
/* ==================================================================== */

/* Each test starts from a known baseline. */
static void modify_reset(void)
{
    log_proc_init();
    log_set_level(LOG_DEBUG);
    log_set_wto_level(LOG_INFO);
}

/* "SET LOGLVL <level>" changes the global level. */
static MunitResult test_modify_sets_global(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();
    rc = log_handle_modify("SET LOGLVL INFO");

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)log_get_level(), ==, (int)LOG_INFO);
    return MUNIT_OK;
}

/* "SET LOGLVL <level> PROC=<name>" pins one proc, leaving the global. */
static MunitResult test_modify_pins_proc(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();
    log_set_level(LOG_INFO);

    rc = log_handle_modify("SET LOGLVL TRACE PROC=WRITE");

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)log_proc_get_level(LOG_PROC_WRITE), ==, (int)LOG_TRACE);
    /* Global untouched by a per-proc command. */
    munit_assert_int((int)log_get_level(), ==, (int)LOG_INFO);
    /* Another proc still inherits the global. */
    munit_assert_int((int)log_proc_get_level(LOG_PROC_READ), ==, (int)LOG_INFO);
    return MUNIT_OK;
}

/* Parsing is case-insensitive. */
static MunitResult test_modify_case_insensitive(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();
    rc = log_handle_modify("set loglvl warn proc=read");

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)log_proc_get_level(LOG_PROC_READ), ==, (int)LOG_WARN);
    return MUNIT_OK;
}

/* The RDIRPLUS alias maps to the READDIRPLUS slot. */
static MunitResult test_modify_rdirplus_alias(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();
    log_set_level(LOG_INFO);
    rc = log_handle_modify("SET LOGLVL DEBUG PROC=RDIRPLUS");

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)log_proc_get_level(LOG_PROC_READDIRPLUS), ==, (int)LOG_DEBUG);
    return MUNIT_OK;
}

/* Leading / interior extra blanks are tolerated. */
static MunitResult test_modify_extra_blanks(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();
    rc = log_handle_modify("   SET   LOGLVL   ERROR  ");

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)log_get_level(), ==, (int)LOG_ERROR);
    return MUNIT_OK;
}

/* An unknown level name is a malformed logger command (-1), global kept. */
static MunitResult test_modify_unknown_level(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();               /* global == LOG_DEBUG */
    rc = log_handle_modify("SET LOGLVL BOGUS");

    munit_assert_int(rc, ==, -1);
    munit_assert_int((int)log_get_level(), ==, (int)LOG_DEBUG);
    return MUNIT_OK;
}

/* An unknown proc name is malformed (-1). */
static MunitResult test_modify_unknown_proc(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();
    rc = log_handle_modify("SET LOGLVL INFO PROC=BOGUS");

    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

/* Missing level, missing '=', and missing proc name are all malformed. */
static MunitResult test_modify_malformed(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    modify_reset();
    munit_assert_int(log_handle_modify("SET LOGLVL"),           ==, -1);
    munit_assert_int(log_handle_modify("SET LOGLVL INFO PROC"), ==, -1);
    munit_assert_int(log_handle_modify("SET LOGLVL INFO PROC="),==, -1);
    return MUNIT_OK;
}

/* Commands the logger does not own return 1 (caller reports unhandled). */
static MunitResult test_modify_not_a_logger_command(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    modify_reset();
    munit_assert_int(log_handle_modify("DISPLAY STATUS"), ==, 1);
    munit_assert_int(log_handle_modify("SET FOO BAR"),    ==, 1);
    munit_assert_int(log_handle_modify(""),               ==, 1);
    munit_assert_int(log_handle_modify(NULL),             ==, 1);
    return MUNIT_OK;
}

/* "SET WTOLVL <level>" changes the console threshold, not the stream. */
static MunitResult test_modify_sets_wto(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();               /* stream DEBUG, WTO INFO */
    rc = log_handle_modify("SET WTOLVL DEBUG");

    munit_assert_int(rc, ==, 0);
    munit_assert_int((int)log_get_wto_level(), ==, (int)LOG_DEBUG);
    /* Stream level untouched. */
    munit_assert_int((int)log_get_level(), ==, (int)LOG_DEBUG);
    return MUNIT_OK;
}

/* An unknown WTOLVL level is malformed (-1), threshold kept. */
static MunitResult test_modify_wto_unknown_level(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();               /* WTO == LOG_INFO */
    rc = log_handle_modify("SET WTOLVL BOGUS");

    munit_assert_int(rc, ==, -1);
    munit_assert_int((int)log_get_wto_level(), ==, (int)LOG_INFO);
    return MUNIT_OK;
}

/* WTOLVL takes no PROC operand; a trailing operand is malformed. */
static MunitResult test_modify_wto_rejects_extra_operand(
    const MunitParameter params[], void *data)
{
    int rc;
    (void)params; (void)data;

    modify_reset();
    rc = log_handle_modify("SET WTOLVL INFO PROC=WRITE");

    munit_assert_int(rc, ==, -1);
    return MUNIT_OK;
}

/* A global SET must not disturb a proc that was previously pinned. */
static MunitResult test_modify_global_keeps_pin(
    const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    modify_reset();
    log_handle_modify("SET LOGLVL TRACE PROC=WRITE");
    log_handle_modify("SET LOGLVL ERROR");

    munit_assert_int((int)log_get_level(), ==, (int)LOG_ERROR);
    munit_assert_int((int)log_proc_get_level(LOG_PROC_WRITE), ==, (int)LOG_TRACE);
    return MUNIT_OK;
}

static MunitTest modify_tests[] = {
    { "/sets_global",           test_modify_sets_global,           NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/pins_proc",             test_modify_pins_proc,             NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/case_insensitive",      test_modify_case_insensitive,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/rdirplus_alias",        test_modify_rdirplus_alias,        NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/extra_blanks",          test_modify_extra_blanks,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/unknown_level",         test_modify_unknown_level,         NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/unknown_proc",          test_modify_unknown_proc,          NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/malformed",             test_modify_malformed,             NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/not_a_logger_command",  test_modify_not_a_logger_command,  NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/global_keeps_pin",      test_modify_global_keeps_pin,      NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/sets_wto",              test_modify_sets_wto,              NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/wto_unknown_level",     test_modify_wto_unknown_level,     NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/wto_rejects_extra_operand", test_modify_wto_rejects_extra_operand, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/level",  level_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/global", global_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/wto",    wto_tests,    NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/proc",   proc_tests,   NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/modify", modify_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tlogger_suite = {
    "/logger",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
