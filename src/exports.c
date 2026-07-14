/*
 * exports.c - Load and query the NFS export configuration.
 *
 * Config file format:
 *   # comment lines start with #
 *   <nfs-export-path>  <local-host-path>
 *
 * Example:
 *   /export/src    /home/user/src
 *   /export/data   /home/user/data
 *
 * Blank lines and lines beginning with '#' are ignored.
 * Up to MAX_EXPORTS entries are supported.
 *
 * For the MVS port: host_path entries will map to PDS dataset names
 * (e.g. HERC01.SYS1.SOURCE) rather than POSIX paths.
 */
 
#include <stdio.h>    /* fopen, fclose, fgets */
#include <string.h>   /* strncpy, strlen, strcmp, strchr */
#include <ctype.h>    /* isspace */
#include <time.h>     /* time */
 
#ifndef __MVS__
#include <sys/stat.h> /* stat */
#endif
 
#include "nfsd.h"
#include "ebcdic.h"
#include "logger.h"
#include "mvsio.h"

static export_t  g_exports[MAX_EXPORTS];
static int       g_nexports = 0;
 
/* ------------------------------------------------------------------ */
/* dataset_init: populate a pds_dataset_t from a dsname token.        */
/*                                                                    */
/* dsname_ebcdic is the raw config token (EBCDIC on MVS).  Derives    */
/* the ASCII form, the lower-case directory name (EBCDIC + ASCII),    */
/* the file extension (lower-case last qualifier), and the DCB info.  */
/* ------------------------------------------------------------------ */
static void dataset_init(pds_dataset_t *ds, const char *dsname_ebcdic)
{
    char *dot;
    int   i;

    memset(ds, 0, sizeof(*ds));

    /* Real dsname, EBCDIC and ASCII. */
    strncpy(ds->dsname_ebcdic, dsname_ebcdic, MAX_DSNAME_LEN - 1);
    ds->dsname_ebcdic[MAX_DSNAME_LEN - 1] = '\0';
    ebcdic_to_ascii(ds->dsname_ascii, ds->dsname_ebcdic, MAX_DSNAME_LEN - 1);

    /* Directory name = lower-case dsname, EBCDIC and ASCII. */
    for (i = 0; ds->dsname_ebcdic[i] != '\0' && i < MAX_DSNAME_LEN - 1; i++)
        ds->dirname_ebcdic[i] = (char)tolower((unsigned char)ds->dsname_ebcdic[i]);
    ds->dirname_ebcdic[i] = '\0';
    ebcdic_to_ascii(ds->dirname_ascii, ds->dirname_ebcdic, MAX_DSNAME_LEN - 1);

    /* File extension = lower-case last qualifier of the dsname. */
    dot = strrchr(ds->dsname_ebcdic, '.');
    if (dot) {
        strncpy(ds->file_ext, dot + 1, MAX_FILE_EXT_LEN - 1);
        ds->file_ext[MAX_FILE_EXT_LEN - 1] = '\0';
        for (i = 0; ds->file_ext[i] != '\0'; i++)
            ds->file_ext[i] = (char)tolower((unsigned char)ds->file_ext[i]);
    } else {
        ds->file_ext[0] = '\0';
    }

    /* DCB info (record format / lengths) for member size estimation. */
    if (mvs_get_dcb_info_dsn(ds->dsname_ebcdic, &ds->dcbinfo) < 0)
        log_error("exports_load: get dcb failed for %s", ds->dsname_ebcdic);
    else
        log_info("exports_load: %s DSORG=0x%02X RECFM=0x%02X LRECL=%d BLKSIZE=%d",
            ds->dsname_ebcdic, ds->dcbinfo.dsorg, ds->dcbinfo.recfm,
            ds->dcbinfo.lrecl, ds->dcbinfo.blksize);
}

/* ------------------------------------------------------------------ */
/* find_or_create_export: return the index of the export with the     */
/* given (EBCDIC) NFS path, creating a new one if none exists yet.    */
/* Returns -1 if the export table is full.                            */
/* ------------------------------------------------------------------ */
static int find_or_create_export(const char *export_path_ebcdic)
{
    int idx;

    for (idx = 0; idx < g_nexports; idx++) {
        if (strcmp(g_exports[idx].export_path_ebcdic, export_path_ebcdic) == 0)
            return idx;
    }

    if (g_nexports >= MAX_EXPORTS)
        return -1;

    idx = g_nexports++;
    memset(&g_exports[idx], 0, sizeof(export_t));
    strncpy(g_exports[idx].export_path_ebcdic, export_path_ebcdic, MAX_PATH - 1);
    g_exports[idx].export_path_ebcdic[MAX_PATH - 1] = '\0';
    ebcdic_to_ascii(g_exports[idx].export_path,
        g_exports[idx].export_path_ebcdic, MAX_PATH - 1);
    g_exports[idx].ndatasets = 0;
    return idx;
}

/* ------------------------------------------------------------------ */
/* exports_load: parse config_file and populate the exports table.    */
/*                                                                    */
/* Format: "<nfs-export-path>  <pds-dataset-name>", one dataset per   */
/* line.  Multiple lines with the same export path group several PDS  */
/* datasets under one export; each appears as a directory (lower-case */
/* dsname) under the export root.                                     */
/*                                                                    */
/* Returns number of exports loaded, or -1 on error.                  */
/* ------------------------------------------------------------------ */
int exports_load(const char *config_file)
{
    FILE          *fp;
    char           line[512];
    char          *p;
    char          *tok;
    char          *rest;
    int            len;
    int            exp_idx;
    export_t      *exp;
    pds_dataset_t *ds;

    fp = fopen(config_file, "r");
    if (!fp) return -1;

    g_nexports = 0;

    while (fgets(line, (int)sizeof(line), fp)) {
        /* strip trailing newline / CR */
        len = (int)strlen(line);
        while (len > 0 &&
               (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* skip leading whitespace */
        p = line;
        while (*p && isspace((unsigned char)*p)) p++;

        /* skip blank lines and comments */
        if (*p == '\0' || *p == '#') continue;

        /* First token: export (NFS) path */
        tok = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) { *p = '\0'; p++; }

        /* Skip whitespace between tokens */
        while (*p && isspace((unsigned char)*p)) p++;

        /* Second token: PDS dataset name (rest of line) */
        rest = p;
        len = (int)strlen(rest);
        while (len > 0 && isspace((unsigned char)rest[len-1]))
            rest[--len] = '\0';

        if (len == 0) {
            log_error("nfsd: warning: missing dataset name for export %s", tok);
            continue;
        }

        /* Find or create the export this dataset belongs to. */
        exp_idx = find_or_create_export(tok);
        if (exp_idx < 0) {
            log_error("nfsd: warning: max exports (%d) reached, ignoring %s",
                MAX_EXPORTS, tok);
            continue;
        }
        exp = &g_exports[exp_idx];

        if (exp->ndatasets >= MAX_PDS_PER_EXPORT) {
            log_error("nfsd: warning: max datasets (%d) reached for export %s, "
                "ignoring %s", MAX_PDS_PER_EXPORT, tok, rest);
            continue;
        }

        /* Append the dataset to this export. */
        ds = &exp->datasets[exp->ndatasets];
        dataset_init(ds, rest);
        exp->ndatasets++;

        /* Keep the legacy single-dataset fields (used by the compile-only
           mockvfs.c and the non-MVS dev build) in sync with datasets[0]. */
        if (exp->ndatasets == 1) {
            strncpy(exp->host_path_ebcdic, ds->dsname_ebcdic, MAX_PATH - 1);
            exp->host_path_ebcdic[MAX_PATH - 1] = '\0';
            strncpy(exp->host_path, ds->dsname_ascii, MAX_PATH - 1);
            exp->host_path[MAX_PATH - 1] = '\0';
            strncpy(exp->file_ext, ds->file_ext, MAX_FILE_EXT_LEN - 1);
            exp->file_ext[MAX_FILE_EXT_LEN - 1] = '\0';
            exp->dcbinfo = ds->dcbinfo;
        }

        log_info("nfsd: export '%s' + dataset '%s' -> dir '%s' (file ext '%s')",
            exp->export_path_ebcdic, ds->dsname_ebcdic,
            ds->dirname_ebcdic, ds->file_ext);
    }

    fclose(fp);

    return g_nexports;
}
 
/* ------------------------------------------------------------------ */
/* exports_count: number of configured exports                        */
/* ------------------------------------------------------------------ */
int exports_count(void)
{
    return g_nexports;
}
 
/* ------------------------------------------------------------------ */
/* exports_get: return pointer to export[idx], or NULL                */
/* ------------------------------------------------------------------ */
export_t *exports_get(int idx)
{
    if (idx < 0 || idx >= g_nexports) return NULL;
    return &g_exports[idx];
}
 
/* ------------------------------------------------------------------ */
/* exports_get_id: return the index of exp in the table, or -1        */
/* ------------------------------------------------------------------ */
int exports_get_id(const export_t *exp)
{
    int i;
    for (i = 0; i < g_nexports; i++) {
        if (&g_exports[i] == exp) return i;
    }
    return -1;
}
 
/* ------------------------------------------------------------------ */
/* exports_find_by_nfs_path: find an export whose NFS path matches.   */
/* The nfs_path comparison is case-sensitive and exact.               */
/* ------------------------------------------------------------------ */
export_t *exports_find_by_nfs_path(const char *nfs_path)
{
    int i;
    for (i = 0; i < g_nexports; i++) {
        if (strcmp(g_exports[i].export_path, nfs_path) == 0)
            return &g_exports[i];
    }
    return NULL;
}
 
/* ------------------------------------------------------------------ */
/* exports_find_by_id: find an export by its table index (export_id). */
/* ------------------------------------------------------------------ */
export_t *exports_find_by_id(uint32_t id)
{
    if (id >= (uint32_t)g_nexports) return NULL;
    return &g_exports[(int)id];
}

/* ------------------------------------------------------------------ */
/* exports_find_by_host_path: return pointer to export[idx], or NULL  */
/* ------------------------------------------------------------------ */
export_t *exports_find_by_host_path(const char *host_path_ebcdic)
{
    int i;
    for (i = 0; i < g_nexports; i++) {
        if ( !strcmp(host_path_ebcdic, g_exports[i].host_path_ebcdic) )
            return &g_exports[i];
    }
    return NULL;
}

/* ================================================================== */
/* Dataset provider                                                   */
/*                                                                    */
/* The root-directory iterator uses only these three calls to walk    */
/* the PDS datasets of an export.  Backed here by the static config    */
/* table; a future catalog-discovery implementation can replace them  */
/* without changing the VFS/NFS layers.                               */
/* ================================================================== */

/* Number of PDS datasets in an export (0 if the export is invalid). */
int export_dataset_count(int export_idx)
{
    if (export_idx < 0 || export_idx >= g_nexports) return 0;
    return g_exports[export_idx].ndatasets;
}

/* Return dataset[dataset_idx] of an export, or NULL if out of range. */
pds_dataset_t *export_dataset_get(int export_idx, int dataset_idx)
{
    export_t *exp;
    if (export_idx < 0 || export_idx >= g_nexports) return NULL;
    exp = &g_exports[export_idx];
    if (dataset_idx < 0 || dataset_idx >= exp->ndatasets) return NULL;
    return &exp->datasets[dataset_idx];
}

/* Find the dataset whose (EBCDIC) directory name matches; -1 if none. */
int export_dataset_find_by_dirname(int export_idx, const char *dirname_ebcdic)
{
    export_t *exp;
    int       i;
    if (export_idx < 0 || export_idx >= g_nexports) return -1;
    exp = &g_exports[export_idx];
    for (i = 0; i < exp->ndatasets; i++) {
        if (strcmp(exp->datasets[i].dirname_ebcdic, dirname_ebcdic) == 0)
            return i;
    }
    return -1;
}

/* Bump a dataset's directory mtime to "now" (called after a member STOW) so
 * NFS clients see the directory as changed and refresh their cached listing. */
void export_dataset_touch(int export_idx, int dataset_idx)
{
    pds_dataset_t *ds = export_dataset_get(export_idx, dataset_idx);
    if (ds != NULL)
        ds->dir_mtime = (uint32_t)time(NULL);
    else {
        log_warn("Server tried to upd dir_mtime for exp_idx %d, " 
            "ds_idx %d, which was not found\n", 
            export_idx, dataset_idx);
    }
}

