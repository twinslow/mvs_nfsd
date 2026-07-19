/*
 * cfgopts.c - Export keyword-option parsing.  See cfgopts.h.
 *
 * Pure and I/O-free (bar log_error diagnostics), so it is unit-tested
 * directly by tests/tcfgopts.c.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>
#include <stdlib.h>   /* strtol */
#include <ctype.h>    /* toupper */

#include "types.h"
#include "cfgopts.h"
#include "logger.h"

int cfg_stricmp(const char *a, const char *b)
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

/*
 * strtol base 8 stops at the first non-octal byte, so *end MUST be checked
 * or "778" would silently become 07.  On MVS the digits are EBCDIC; strtol
 * is the native JCC library call and handles them.
 */
int cfg_parse_octal(const char *v, uint16_t *out)
{
    char *end;
    long  val;

    if (v == NULL || *v == '\0')
        return -1;

    val = strtol(v, &end, 8);
    if (*end != '\0')
        return -1;
    if (val < 0 || val > 0777)
        return -1;

    *out = (uint16_t)val;
    return 0;
}

/*
 * If 'tok' is "<key>=<value>" (key compared case-insensitively), set *val to
 * the value part and return 1; otherwise return 0.  '=' is an EBCDIC literal
 * under JCC, matching the EBCDIC config token.
 */
static int cfg_kw_match(char *tok, const char *key, char **val)
{
    int i;
    int klen = (int)strlen(key);

    for (i = 0; i < klen; i++) {
        if (tok[i] == '\0')
            return 0;
        if (toupper((unsigned char)tok[i]) != toupper((unsigned char)key[i]))
            return 0;
    }
    if (tok[i] != '=')
        return 0;

    *val = tok + i + 1;
    return 1;
}

int cfg_parse_keywords(char *toks[], int n, cfg_opts_t *out,
                       int level, const char *ctx)
{
    int   i;
    char *val;

    memset(out, 0, sizeof(*out));

    for (i = 0; i < n; i++) {
        char *t = toks[i];

        if (cfg_stricmp(t, "RO") == 0) {
            out->has_readonly = 1;
            out->readonly     = 1;
        } else if (cfg_stricmp(t, "RW") == 0) {
            out->has_readonly = 1;
            out->readonly     = 0;
        } else if (cfg_kw_match(t, "DIRPERM", &val)) {
            if (cfg_parse_octal(val, &out->dirperm) < 0) {
                log_error("exports_load: %s: bad dirperm value '%s'"
                          " (want octal 0..777)", ctx, val);
                return -1;
            }
            out->has_dirperm = 1;
        } else if (cfg_kw_match(t, "MEMPERM", &val)) {
            if (cfg_parse_octal(val, &out->memperm) < 0) {
                log_error("exports_load: %s: bad memperm value '%s'"
                          " (want octal 0..777)", ctx, val);
                return -1;
            }
            out->has_memperm = 1;
        } else if (cfg_kw_match(t, "ROOTPERM", &val)) {
            if (level != CFG_LEVEL_EXPORT) {
                log_error("exports_load: %s: 'rootperm' is an export-level"
                          " keyword, not valid on a dataset", ctx);
                return -1;
            }
            if (cfg_parse_octal(val, &out->rootperm) < 0) {
                log_error("exports_load: %s: bad rootperm value '%s'"
                          " (want octal 0..777)", ctx, val);
                return -1;
            }
            out->has_rootperm = 1;
        } else if (cfg_kw_match(t, "FILEEXT", &val)) {
            int j;
            if (val[0] == '\0') {
                log_error("exports_load: %s: 'fileext' needs a value"
                          " (e.g. fileext=jcl)", ctx);
                return -1;
            }
            if ((int)strlen(val) > CFG_FILEEXT_MAX - 1) {
                log_error("exports_load: %s: fileext '%s' too long"
                          " (max %d chars)", ctx, val, CFG_FILEEXT_MAX - 1);
                return -1;
            }
            /* A '.' or '/' breaks member-name parsing (the extension is the
               text after the LAST dot), so reject it rather than fail silently.
               Store lower-case, matching the dsname-derived default. */
            for (j = 0; val[j] != '\0'; j++) {
                if (val[j] == '.' || val[j] == '/') {
                    log_error("exports_load: %s: fileext '%s' must not contain"
                              " '.' or '/'", ctx, val);
                    return -1;
                }
                out->fileext[j] = (char)tolower((unsigned char)val[j]);
            }
            out->fileext[j]  = '\0';
            out->has_fileext = 1;
        } else if (cfg_stricmp(t, "NOFILEEXT") == 0) {
            out->has_nofileext = 1;
        } else {
            log_error("exports_load: %s: unknown keyword '%s'", ctx, t);
            return -1;
        }
    }

    /* 'fileext=' and 'nofileext' on the same line are contradictory. */
    if (out->has_fileext && out->has_nofileext) {
        log_error("exports_load: %s: 'fileext' and 'nofileext' are mutually"
                  " exclusive", ctx);
        return -1;
    }
    return 0;
}

int cfg_resolve_opts(const cfg_opts_t *exp_opts, const cfg_opts_t *ds_opts,
                     uint8_t *readonly_out, uint16_t *dirperm_out,
                     uint16_t *memperm_out, char *fileext_out,
                     const char *ctx)
{
    int      readonly = 0;
    uint16_t dirperm  = CFG_PERM_DIR_DEFAULT;
    uint16_t memperm  = CFG_PERM_MEM_DEFAULT;

    if (exp_opts != NULL) {
        if (exp_opts->has_dirperm) dirperm = exp_opts->dirperm;
        if (exp_opts->has_memperm) memperm = exp_opts->memperm;
        if (exp_opts->has_readonly && exp_opts->readonly) readonly = 1; /* ceiling */
    }

    if (ds_opts->has_dirperm) dirperm = ds_opts->dirperm;
    if (ds_opts->has_memperm) memperm = ds_opts->memperm;

    if (ds_opts->has_readonly) {
        if (ds_opts->readonly == 0) {   /* explicit rw */
            if (exp_opts != NULL && exp_opts->has_readonly && exp_opts->readonly) {
                log_error("exports_load: %s: 'rw' dataset inside a read-only"
                          " export is not allowed", ctx);
                return -1;
            }
            readonly = 0;
        } else {
            readonly = 1;
        }
    }

    /* Extension: export-level default then dataset-level override (dataset
       wins).  'fileext=' sets an explicit extension, 'nofileext' clears it to
       empty; within one level the two are mutually exclusive (rejected at
       parse).  If neither keyword appears at either level, fileext_out keeps
       the caller's dsname-derived default. */
    if (exp_opts != NULL) {
        if (exp_opts->has_fileext) {
            strncpy(fileext_out, exp_opts->fileext, CFG_FILEEXT_MAX - 1);
            fileext_out[CFG_FILEEXT_MAX - 1] = '\0';
        } else if (exp_opts->has_nofileext) {
            fileext_out[0] = '\0';
        }
    }
    if (ds_opts->has_fileext) {
        strncpy(fileext_out, ds_opts->fileext, CFG_FILEEXT_MAX - 1);
        fileext_out[CFG_FILEEXT_MAX - 1] = '\0';
    } else if (ds_opts->has_nofileext) {
        fileext_out[0] = '\0';
    }

    *readonly_out = (uint8_t)readonly;
    *dirperm_out  = dirperm;
    *memperm_out  = memperm;
    return 0;
}
