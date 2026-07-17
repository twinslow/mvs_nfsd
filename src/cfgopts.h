/*
 * cfgopts.h - Export keyword-option parsing (config [Exports] keywords).
 *
 * This is the pure, I/O-free half of the config parser: it turns keyword
 * tokens (ro / rw / dirperm= / memperm= / rootperm=) into a cfg_opts_t and
 * folds export-level defaults with dataset-level overrides.  It has no
 * dependency on the exports table, so it can be unit-tested on its own
 * (tests/tcfgopts.c) while exports.c keeps the line/section/block handling
 * and the table.
 *
 * See doc/design_export_options.md and doc/readme_config.md.
 *
 * JCC C89 compliance: no C99 features.
 */

#ifndef CFGOPTS_H_INCLUDED
#define CFGOPTS_H_INCLUDED

#include "types.h"

/*
 * MVS 8-character external-symbol aliases.  cfg_parse_octal and
 * cfg_parse_keywords collide in their first 8 characters ("cfg_pars"), so
 * the colliding externals are given distinct short names for the linker --
 * the same pattern as ebcdic.h / mvspww.h.
 */
#if defined(__MVS__)
#define cfg_stricmp        cfgStrcm
#define cfg_parse_octal    cfgOctal
#define cfg_parse_keywords cfgKwds
#define cfg_resolve_opts   cfgRslv
#endif

/* Keyword level: where a keyword is permitted to appear. */
#define CFG_LEVEL_EXPORT   0   /* keywords before '{' (or on a block header) */
#define CFG_LEVEL_DATASET  1   /* keywords after a dataset name              */

/* Default reported modes. */
#define CFG_PERM_DIR_DEFAULT   0777
#define CFG_PERM_MEM_DEFAULT   0777
#define CFG_PERM_ROOT_DEFAULT  0555

/*
 * Parsed keyword set.  has_* records whether the keyword was PRESENT (needed
 * for inheritance and level checks); a value field is meaningful only when
 * its matching has_* is set.
 */
typedef struct {
    int      has_readonly;
    int      readonly;      /* 1 = ro, 0 = rw */
    int      has_dirperm;
    uint16_t dirperm;
    int      has_memperm;
    uint16_t memperm;
    int      has_rootperm;
    uint16_t rootperm;
} cfg_opts_t;

/*
 * Case-insensitive compare (C89 has no strcasecmp).  toupper is
 * EBCDIC-correct under JCC and both operands are in the file's encoding.
 */
int cfg_stricmp(const char *a, const char *b);

/*
 * Parse an octal permission value ("755" -> 0755).  Rejects empty, non-octal
 * digits / trailing junk, and values above 0777.  Returns 0 on success (with
 * *out set), -1 otherwise.
 */
int cfg_parse_octal(const char *v, uint16_t *out);

/*
 * Parse keyword tokens toks[0..n-1] into *out.  'level' is CFG_LEVEL_EXPORT
 * or CFG_LEVEL_DATASET; 'ctx' names the export/dataset for messages.
 * Returns 0, or -1 on any unknown keyword, bad value, or a keyword used at
 * the wrong level (each fully reported via log_error).
 */
int cfg_parse_keywords(char *toks[], int n, cfg_opts_t *out,
                       int level, const char *ctx);

/*
 * Fold export-level defaults and dataset-level overrides into the resolved
 * readonly / dirperm / memperm outputs.  exp_opts may be NULL (the flat form
 * has no export-level keywords).  Returns 0, or -1 for the one semantic
 * error: 'rw' on a dataset inside an 'ro' export (ro is a ceiling, not a
 * default -- design §2.4).
 */
int cfg_resolve_opts(const cfg_opts_t *exp_opts, const cfg_opts_t *ds_opts,
                     uint8_t *readonly_out, uint16_t *dirperm_out,
                     uint16_t *memperm_out, const char *ctx);

#endif /* CFGOPTS_H_INCLUDED */
