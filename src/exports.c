/*
 * exports.c - Load the server configuration (exports and startup commands).
 *
 * The config file is a Windows .ini-style sectioned file:
 *
 *   [Init]
 *   set loglvl info
 *   set loglvl debug proc=write
 *
 *   [Exports]
 *   # comment lines start with #
 *   /exports    TEMP.TESTPROJ.C
 *   /exports    TEMP.TESTPROJ.CNTL
 *   /exports    TEMP.TESTPROJ.JCLLIB
 *
 * Sections:
 *   [Init]     Each line is an operator command, executed exactly as if it
 *              had been entered via the MVS MODIFY (F) interface -- it is
 *              handed to the same handler.  So anything valid in
 *              "F NFSD,<cmd>" is valid here.  See doc/readme_config.md.
 *   [Exports]  "<nfs-export-path>  <pds-dataset-name>", one dataset per
 *              line.  Repeating an export path groups several PDS datasets
 *              under it; each appears as a directory (the lower-case
 *              dsname) under the export root.
 *
 * Section names are case-insensitive.  Blank lines and lines beginning
 * with '#' are ignored anywhere.  Lines appearing BEFORE any section
 * header are treated as [Exports], so pre-section config files keep
 * working unchanged.  An unrecognised section is reported and its lines
 * are skipped, so a newer config stays loadable by an older server.
 *
 * Up to MAX_EXPORTS export paths, each with up to MAX_PDS_PER_EXPORT
 * datasets, are supported.
 *
 * Adding a section: add a CFG_SECT_* id, an entry in g_cfg_sections[],
 * and a case in the dispatch switch in exports_load().
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
/* Config sections                                                      */
/* ------------------------------------------------------------------ */

#define CFG_SECT_UNKNOWN   0
#define CFG_SECT_INIT      1
#define CFG_SECT_EXPORTS   2

/*
 * Section header delimiters.
 *
 * The config file is read as raw bytes, so on MVS its text is EBCDIC --
 * and so are C character literals under JCC.  That is exactly why the '#'
 * comment test below works, and the same reasoning applies here.
 *
 * NOTE: '[' and ']' are the most code-page-variable characters in EBCDIC
 * (they move between CP037 / CP1047 / CP500).  If a section header is ever
 * not recognised on MVS, this is the first place to look -- the fix is to
 * compare against ascii_to_ebcdic_c('[') instead of the literal.
 */
#define CFG_SECT_OPEN   '['
#define CFG_SECT_CLOSE  ']'

static const struct { const char *name; int id; } g_cfg_sections[] = {
    { "INIT",    CFG_SECT_INIT    },
    { "EXPORTS", CFG_SECT_EXPORTS }
};

/* Case-insensitive compare (C89 has no strcasecmp; toupper is
   EBCDIC-correct under JCC, and both operands are in the file's encoding). */
static int cfg_stricmp(const char *a, const char *b)
{
    int ca;
    int cb;

    while (*a != '\0' && *b != '\0') {
        ca = toupper((unsigned char)*a);
        cb = toupper((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Map a section name to its id, or CFG_SECT_UNKNOWN. */
static int cfg_section_id(const char *name)
{
    int i;
    int n = (int)(sizeof(g_cfg_sections) / sizeof(g_cfg_sections[0]));

    for (i = 0; i < n; i++) {
        if (cfg_stricmp(name, g_cfg_sections[i].name) == 0)
            return g_cfg_sections[i].id;
    }
    return CFG_SECT_UNKNOWN;
}

/*
 * Parse a "[name]" header line (p points at the '[').  Returns the section
 * id, or CFG_SECT_UNKNOWN if the header is malformed or the name is not
 * recognised.  The line buffer is modified in place.
 */
static int cfg_parse_section(char *p)
{
    char *close;
    char *name;
    int   len;

    close = strchr(p, CFG_SECT_CLOSE);
    if (close == NULL) {
        log_error("exports_load: malformed section header (no closing"
                  " bracket): %s", p);
        return CFG_SECT_UNKNOWN;
    }
    *close = '\0';

    /* Trim blanks inside the brackets: "[ Init ]" is accepted. */
    name = p + 1;
    while (*name != '\0' && isspace((unsigned char)*name)) name++;
    len = (int)strlen(name);
    while (len > 0 && isspace((unsigned char)name[len - 1]))
        name[--len] = '\0';

    return cfg_section_id(name);
}

/*
 * Execute one [Init] line as an operator command.
 *
 * The line is passed to the very same handler the MVS MODIFY (F) path uses,
 * so the two interfaces can never drift apart.  The text is already in the
 * file's encoding (EBCDIC on MVS), which is what the handler expects.
 */
static void cfg_do_init_line(const char *cmd)
{
    int rc;

    rc = log_handle_modify(cmd);
    if (rc == 1) {
        log_warn("exports_load: [Init] unrecognised command: %s", cmd);
    } else if (rc == 0) {
        log_debug("exports_load: [Init] applied: %s", cmd);
    }
    /* rc < 0: the handler already reported the specific fault. */
}
 
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
/* cfg_do_export_line: handle one [Exports] line.                     */
/*                                                                    */
/* "<nfs-export-path>  <pds-dataset-name>".  The line buffer is       */
/* modified in place.                                                 */
/* ------------------------------------------------------------------ */
static void cfg_do_export_line(char *p)
{
    char          *tok;
    char          *rest;
    int            len;
    int            exp_idx;
    export_t      *exp;
    pds_dataset_t *ds;

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
        return;
    }

    /* Find or create the export this dataset belongs to. */
    exp_idx = find_or_create_export(tok);
    if (exp_idx < 0) {
        log_error("nfsd: warning: max exports (%d) reached, ignoring %s",
            MAX_EXPORTS, tok);
        return;
    }
    exp = &g_exports[exp_idx];

    if (exp->ndatasets >= MAX_PDS_PER_EXPORT) {
        log_error("nfsd: warning: max datasets (%d) reached for export %s, "
            "ignoring %s", MAX_PDS_PER_EXPORT, tok, rest);
        return;
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

/* ------------------------------------------------------------------ */
/* exports_load: parse config_file, run [Init] commands and populate  */
/* the exports table.  See the file header for the format.            */
/*                                                                    */
/* Returns number of exports loaded, or -1 if the file cannot be read. */
/* ------------------------------------------------------------------ */
int exports_load(const char *config_file)
{
    FILE *fp;
    char  line[512];
    char *p;
    int   len;
    int   section;
    int   warned_unknown;

    fp = fopen(config_file, "r");
    if (!fp) return -1;

    g_nexports = 0;

    /* Lines before any header are [Exports], so a pre-section config file
       loads unchanged. */
    section        = CFG_SECT_EXPORTS;
    warned_unknown = 0;

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

        /* A section header switches context and consumes the line. */
        if (*p == CFG_SECT_OPEN) {
            section = cfg_parse_section(p);
            if (section == CFG_SECT_UNKNOWN) {
                log_warn("exports_load: ignoring unknown section: %s", p + 1);
                warned_unknown = 1;
            } else {
                log_debug("exports_load: entering section %s", p + 1);
            }
            continue;
        }

        switch (section) {
        case CFG_SECT_INIT:
            cfg_do_init_line(p);
            break;
        case CFG_SECT_EXPORTS:
            cfg_do_export_line(p);
            break;
        default:
            /* Inside an unknown section: skip quietly (already warned once
               at the header) so a newer config loads on an older server. */
            break;
        }
    }

    fclose(fp);

    if (warned_unknown)
        log_warn("exports_load: one or more unknown sections were skipped");

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

