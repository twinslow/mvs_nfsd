/*
 * tstubs.h - Shared stub declarations for mvsio unit tests.
 *
 * Provides a lightweight in-process replacement for the exports table
 * normally managed by exports.c.  Each test fixture calls
 * stub_clear_exports() then stub_add_export() to set up the exact
 * exports state it needs.
 *
 * The actual implementations of exports_count() and exports_get() that
 * satisfy the linker are in tstubs.c.  Link tstubs.c in place of
 * exports.c for all test builds.
 */

#ifndef TSTUBS_H_INCLUDED
#define TSTUBS_H_INCLUDED

/* Reset the stub exports table to empty (call at the start of each fixture). */
void stub_clear_exports(void);

/* Append one entry to the stub exports table. */
void stub_add_export(const char *export_path,
                     const char *host_path,
                     const char *file_ext);

#endif /* TSTUBS_H_INCLUDED */
