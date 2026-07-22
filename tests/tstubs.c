/*
 * tstubs.c - Shared stub implementations for mvsio unit tests.
 *
 * Provides stub versions of exports_count() / exports_get() and the
 * dataset provider (export_dataset_count/get/find_by_dirname) that
 * satisfy the linker without requiring exports.c, a config file, or any
 * live MVS dataset.
 *
 * Build: include this file in the test link in place of exports.c.
 *   cc -std=c99 -Wall -I src -I tests \
 *      tests/runall.c tests/tstubs.c tests/tmvsio.c tests/tmvsio2.c \
 *      src/mvsio.c tests/munit.c \
 *      -o tests/runall
 *
 * Encoding note: the tested functions (mvs_path_type,
 * mvs_get_pds_dsn_and_member) only ever read the *_ebcdic and file_ext
 * fields, comparing them against literal test strings.  On the dev host
 * everything is ASCII; on MVS everything is EBCDIC.  Because the stub
 * derives dirname_ebcdic by lower-casing the (same-encoding) dsname
 * literal, the tests are correct on both targets without any explicit
 * conversion.  The *_ascii fields are populated but not relied upon.
 */

#include <string.h>
#include <ctype.h>

#include "nfsd.h"    /* export_t, pds_dataset_t, MAX_* */
#include "tstubs.h"
#include "asmutils.h" /* mvs_dynalloc / mvs_stow name aliases (for the stubs) */

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

/* Fill a dataset from a dsname + file extension, deriving the lower-case
 * directory name the same way exports.c does. */
static void fill_dataset(pds_dataset_t *ds,
                         const char *dsname, const char *file_ext)
{
    int i;

    memset(ds, 0, sizeof(*ds));

    strncpy(ds->dsname_ebcdic, dsname, MAX_DSNAME_LEN - 1);
    ds->dsname_ebcdic[MAX_DSNAME_LEN - 1] = '\0';
    strncpy(ds->dsname_ascii, dsname, MAX_DSNAME_LEN - 1);
    ds->dsname_ascii[MAX_DSNAME_LEN - 1] = '\0';

    for (i = 0; ds->dsname_ebcdic[i] != '\0' && i < MAX_DSNAME_LEN - 1; i++)
        ds->dirname_ebcdic[i] = (char)tolower((unsigned char)ds->dsname_ebcdic[i]);
    ds->dirname_ebcdic[i] = '\0';
    strncpy(ds->dirname_ascii, ds->dirname_ebcdic, MAX_DSNAME_LEN - 1);
    ds->dirname_ascii[MAX_DSNAME_LEN - 1] = '\0';

    strncpy(ds->file_ext, file_ext, MAX_FILE_EXT_LEN - 1);
    ds->file_ext[MAX_FILE_EXT_LEN - 1] = '\0';
}

/* -----------------------------------------------------------------------
 * stub_add_export: append one export with a single PDS dataset.
 * host_path is the PDS dsname; the directory name is derived from it.
 * ----------------------------------------------------------------------- */
void stub_add_export(const char *export_path,
                     const char *host_path,
                     const char *file_ext)
{
    export_t *e = &s_exports[s_nexports++];

    memset(e, 0, sizeof(*e));

    strncpy(e->export_path,        export_path, MAX_PATH - 1);
    e->export_path[MAX_PATH - 1]        = '\0';
    strncpy(e->export_path_ebcdic, export_path, MAX_PATH - 1);
    e->export_path_ebcdic[MAX_PATH - 1] = '\0';

    /* First dataset for this export. */
    fill_dataset(&e->datasets[0], host_path, file_ext);
    e->ndatasets = 1;

    /* Legacy single-dataset fields (mirrors exports.c). */
    strncpy(e->host_path,        host_path, MAX_PATH - 1);
    e->host_path[MAX_PATH - 1]        = '\0';
    strncpy(e->host_path_ebcdic, host_path, MAX_PATH - 1);
    e->host_path_ebcdic[MAX_PATH - 1] = '\0';
    strncpy(e->file_ext,         file_ext,  MAX_FILE_EXT_LEN - 1);
    e->file_ext[MAX_FILE_EXT_LEN - 1] = '\0';
}

/* -----------------------------------------------------------------------
 * stub_add_dataset: append another PDS dataset to the most recently
 * added export (models several datasets grouped under one export path).
 * ----------------------------------------------------------------------- */
void stub_add_dataset(const char *host_path, const char *file_ext)
{
    export_t *e;

    if (s_nexports == 0) return;
    e = &s_exports[s_nexports - 1];
    if (e->ndatasets >= MAX_PDS_PER_EXPORT) return;

    fill_dataset(&e->datasets[e->ndatasets], host_path, file_ext);
    e->ndatasets++;
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

/* exports_get_id: index of 'exp' in the stub table, or -1 (mirrors exports.c).
   Referenced by fhandle.c's fh_resolve, so it must resolve when fhandle.o is
   linked -- even though the fh encode/decode tests never call it. */
int exports_get_id(const export_t *exp)
{
    int i;
    for (i = 0; i < s_nexports; i++) {
        if (&s_exports[i] == exp) return i;
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Dataset provider stubs (in place of exports.c).
 * ----------------------------------------------------------------------- */
int export_dataset_count(int export_idx)
{
    if (export_idx < 0 || export_idx >= s_nexports) return 0;
    return s_exports[export_idx].ndatasets;
}

pds_dataset_t *export_dataset_get(int export_idx, int dataset_idx)
{
    export_t *e;
    if (export_idx < 0 || export_idx >= s_nexports) return NULL;
    e = &s_exports[export_idx];
    if (dataset_idx < 0 || dataset_idx >= e->ndatasets) return NULL;
    return &e->datasets[dataset_idx];
}

int export_dataset_find_by_dirname(int export_idx, const char *dirname_ebcdic)
{
    export_t *e;
    int       i;
    if (export_idx < 0 || export_idx >= s_nexports) return -1;
    e = &s_exports[export_idx];
    for (i = 0; i < e->ndatasets; i++) {
        if (strcmp(e->datasets[i].dirname_ebcdic, dirname_ebcdic) == 0)
            return i;
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Stubs for the flush/STOW dependencies of mvspww.c.  The pending-write
 * pool references these, but the unit tests never trigger a flush, so
 * empty stubs satisfy the linker without pulling in exports.c or the
 * assembler helpers.
 * ----------------------------------------------------------------------- */
void export_dataset_touch(int export_idx, int dataset_idx)
{
    (void)export_idx; (void)dataset_idx;
}

#ifdef __MVS__
/*
 * Under __MVS__ mvspww.c calls these assembler helpers.  As of the write-path
 * change that takes the SPFEDIT enqueue + DISP=SHR allocation at CREATE / first
 * WRITE (pww_lock), they are on the create/write/truncate paths the buffer
 * tests exercise -- not just the flush path the tests avoid.  So they are
 * stubbed to SUCCEED (cheaply, with no real ENQ or allocation), keeping
 * tmvspww hermetic: no real I/O, no console WTOs, and no cross-test enqueue
 * leak (the tests reuse one member and pww_init() does not DEQ).
 *
 * NOTE: because mvs_enq is stubbed here, the RUNALL link must NOT also pull in
 * the real MVSENQ (INCLUDE SYSLIB(MVSENQ)) -- that would multiply-define it.
 */
int mvs_enq(uint8_t request_type, uint8_t options,
            const char *qname, const char *rname)
{
    (void)request_type; (void)options; (void)qname; (void)rname;
    return 0;
}

int mvs_dynalloc(uint8_t request_type, uint8_t options,
                 const char *dsname, const char *member, char *ddname)
{
    (void)options; (void)dsname; (void)member;
    if (request_type == MVS_DYNALLOC_REQ_ALLOC && ddname != NULL)
        strcpy(ddname, "SYS00001");   /* a plausible dynalloc ddname */
    return 0;
}

int mvs_stow(const char *ddname, const char *member,
             void *userdata, int user_data_length_bytes)
{
    (void)ddname; (void)member; (void)userdata; (void)user_data_length_bytes;
    return 0;
}
#endif
