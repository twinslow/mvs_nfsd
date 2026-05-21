/*
 * runall.c - Top-level test runner: aggregates all test suites.
 *
 * Build (from project root):
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c tests/tmvsio.c src/mvsio.c tests/munit.c \
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

/*
 * NUM_SUITES: count of module suites (excluding the NULL terminator).
 * Increment this by one each time a new extern suite is added.
 */
#define NUM_SUITES 1

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
