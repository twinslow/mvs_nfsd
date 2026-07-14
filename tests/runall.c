/*
 * runall.c - Top-level test runner: aggregates all test suites.
 *
 * Build (from project root):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c tests/tstubs.c \
 *      tests/tmvsio.c tests/tmvsio2.c tests/tmvspdir.c tests/tmvsprw.c \
 *      tests/tmvsdol.c tests/tmvsfsz.c tests/tmvsprf.c tests/tmvspww.c \
 *      tests/tlogger.c \
 *      src/mvsio.c src/mvsdol.c src/mvsfsz.c src/mvspdir.c src/mvsprw.c \
 *      src/mvspww.c src/ebcdic.c src/mvsprf.c src/logger.c tests/munit.c \
 *      -o tests/runall
 *
 * Run:
 *   tests/runall
 *
 * Adding a new test suite:
 *   1. Create tests/t<module>.c that defines a MunitSuite <module>_suite.
 *   2. Add an extern declaration for it below.
 *   3. Add the suite to the all_suites[] array in main().
 *   4. Add the new source file to the build command above.
 */

#include <string.h>
#include "munit.h"

/* -----------------------------------------------------------------------
 * One extern declaration per test module.
 * The suite structs are defined in their respective t<module>.c files.
 * ----------------------------------------------------------------------- */
extern MunitSuite tmvsio_suite;
extern MunitSuite tmvsio2_suite;
extern MunitSuite tmvspdir_suite;
extern MunitSuite tmvsprw_suite;
extern MunitSuite tmvsdol_suite;
extern MunitSuite tmvsfsz_suite;
extern MunitSuite tmvsprf_suite;
extern MunitSuite tmvspww_suite;
extern MunitSuite tlogger_suite;

/*
 * NUM_SUITES: count of module suites (excluding the NULL terminator).
 * Increment this by one each time a new extern suite is added.
 */
#define NUM_SUITES 9

/* -----------------------------------------------------------------------
 * main: build the root suite from all module suites and run it.
 *
 * All declarations are at the top of the function (C89 requirement).
 * Assignments follow after, using the extern suite values which are
 * copied by value at runtime.  munit reads the suite synchronously
 * inside munit_suite_main() so the local lifetime is sufficient.
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    MunitSuite all_suites[NUM_SUITES + 1]; /* +1 for NULL terminator */
    MunitSuite root;

    /* Populate module suites -- add one line per suite */
    all_suites[0] = tmvsio_suite;
    all_suites[1] = tmvsio2_suite;
    all_suites[2] = tmvspdir_suite;
    all_suites[3] = tmvsprw_suite;
    all_suites[4] = tmvsdol_suite;
    all_suites[5] = tmvsfsz_suite;
    all_suites[6] = tmvsprf_suite;
    all_suites[7] = tmvspww_suite;
    all_suites[8] = tlogger_suite;

    /* NULL terminator entry */
    memset(&all_suites[NUM_SUITES], 0, sizeof(MunitSuite));

    /* Root suite wraps all module suites */
    root.prefix     = "";    /* no prefix at the root level */
    root.tests      = NULL;  /* no tests directly at root   */
    root.suites     = all_suites;
    root.iterations = 1;
    root.options    = MUNIT_SUITE_OPTION_NONE;

    return munit_suite_main(&root, NULL, argc, (char * const *)argv);
}
