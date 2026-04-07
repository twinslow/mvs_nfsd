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
#include "nfsd.h"

static export_t  g_exports[MAX_EXPORTS];
static int       g_nexports = 0;

/* ------------------------------------------------------------------ */
/* exports_load: parse config_file and populate the exports table.      */
/* Returns number of exports loaded, or -1 on error.                   */
/* ------------------------------------------------------------------ */
int exports_load(const char *config_file)
{
    FILE *fp;
    char  line[MAX_PATH * 2 + 4];
    char *p;
    char *tok;
    char *rest;
    int   len;

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

        if (g_nexports >= MAX_EXPORTS) {
            fprintf(stderr,
                "nfsd: warning: max exports (%d) reached, "
                "ignoring extra lines\n", MAX_EXPORTS);
            break;
        }

        /* First token: export (NFS) path */
        tok = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) { *p = '\0'; p++; }

        strncpy(g_exports[g_nexports].export_path, tok, MAX_PATH - 1);
        g_exports[g_nexports].export_path[MAX_PATH - 1] = '\0';

        /* Skip whitespace between tokens */
        while (*p && isspace((unsigned char)*p)) p++;

        /* Second token: local host path */
        rest = p;
        /* trim trailing whitespace */
        len = (int)strlen(rest);
        while (len > 0 && isspace((unsigned char)rest[len-1]))
            rest[--len] = '\0';

        if (len == 0) {
            fprintf(stderr,
                "nfsd: warning: missing host path for export %s\n",
                g_exports[g_nexports].export_path);
            continue;
        }

        strncpy(g_exports[g_nexports].host_path, rest, MAX_PATH - 1);
        g_exports[g_nexports].host_path[MAX_PATH - 1] = '\0';

        g_nexports++;
    }

    fclose(fp);
    return g_nexports;
}

/* ------------------------------------------------------------------ */
/* exports_count: number of configured exports                          */
/* ------------------------------------------------------------------ */
int exports_count(void)
{
    return g_nexports;
}

/* ------------------------------------------------------------------ */
/* exports_get: return pointer to export[idx], or NULL                  */
/* ------------------------------------------------------------------ */
export_t *exports_get(int idx)
{
    if (idx < 0 || idx >= g_nexports) return NULL;
    return &g_exports[idx];
}

/* ------------------------------------------------------------------ */
/* exports_get_id: return the index of exp in the table, or -1          */
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
/* exports_find_by_nfs_path: find an export whose NFS path matches.     */
/* The nfs_path comparison is case-sensitive and exact.                 */
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
/* exports_find_by_id: find an export by its table index (export_id).   */
/* ------------------------------------------------------------------ */
export_t *exports_find_by_id(uint32_t id)
{
    if (id >= (uint32_t)g_nexports) return NULL;
    return &g_exports[(int)id];
}
