/*
 * tstubs.c - Shared stub implementations for mvsio unit tests.
 *
 * Provides stub versions of exports_count() and exports_get() that
 * satisfy the linker without requiring exports.c, a config file, or
 * any live MVS dataset.
 *
 * Build: include this file in the test link in place of exports.c.
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c tests/tstubs.c tests/tmvsio.c tests/tmvsio2.c \
 *      src/mvsio.c tests/munit.c \
 *      -o tests/runall
 */

#include <string.h>

#include "nfsd.h"    /* export_t, MAX_EXPORTS, MAX_PATH, MAX_FILE_EXT_LEN */
#include "tstubs.h"

static export_t s_exports[MAX_EXPORTS];
static int      s_nexports = 0;

/* -----------------------------------------------------------------------
 * stub_clear_exports: reset the table to empty.
 * Call at the start of each fixture setup function.
 * ----------------------------------------------------------------------- */
void stub_clear_exports(void)
{
    s_nexports = 0;
}

/* -----------------------------------------------------------------------
 * stub_add_export: append one export entry to the stub table.
 * ----------------------------------------------------------------------- */
void stub_add_export(const char *export_path,
                     const char *host_path,
                     const char *file_ext)
{
    export_t *e = &s_exports[s_nexports++];
    strncpy(e->export_path, export_path, MAX_PATH - 1);
    e->export_path[MAX_PATH - 1] = '\0';
    strncpy(e->host_path,   host_path,   MAX_PATH - 1);
    e->host_path[MAX_PATH - 1] = '\0';
    strncpy(e->file_ext,    file_ext,    MAX_FILE_EXT_LEN - 1);
    e->file_ext[MAX_FILE_EXT_LEN - 1] = '\0';
}

/* -----------------------------------------------------------------------
 * exports_count / exports_get: satisfy the linker in place of exports.c.
 * ----------------------------------------------------------------------- */
int exports_count(void)
{
    return s_nexports;
}

export_t *exports_get(int idx)
{
    if (idx < 0 || idx >= s_nexports) return NULL;
    return &s_exports[idx];
}
