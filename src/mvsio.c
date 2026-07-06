

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <io.h>

#include "nfsd.h"
#include "mvsio.h"
#include "ebcdic.h"
#include "logger.h"

/* -------------------------------------------------------------------- */
/* mvs_member_name_valid: validate a PDS member name (the upper-cased    */
/* form, as it would be stored in the directory).  IBM rules:            */
/*   - 1 to 8 characters;                                                */
/*   - first character a letter (A-Z) or a national character (@ # $)    */
/*     -- NOT a digit;                                                   */
/*   - remaining characters letters, digits (0-9), or national chars;    */
/*   - no lowercase, no other special characters (e.g. '-', '.', ' ').   */
/*                                                                       */
/* Returns 0 if valid, otherwise an errno describing the fault:          */
/*   ENAMETOOLONG -- more than 8 characters                              */
/*   EINVAL       -- empty, starts with a digit, or has an invalid char  */
/*                                                                       */
/* Callers upper-case the client filename before validating, so a name   */
/* containing lowercase reaches here as non-upper and is rejected by the  */
/* isupper() check.  isupper()/isdigit() are EBCDIC-correct under JCC and  */
/* the national-char literals are EBCDIC on MVS.                          */
/* -------------------------------------------------------------------- */
int mvs_member_name_valid(const char *member)
{
    size_t        len;
    size_t        i;
    unsigned char c;

    len = strlen(member);
    if (len == 0)
        return EINVAL;
    if (len > 8)
        return ENAMETOOLONG;

    /* First character: a letter (A-Z) or a national character (@ # $),
       but not a digit.  isupper() is EBCDIC-correct under JCC. */
    c = (unsigned char)member[0];
    if (!(isupper(c) || c == '@' || c == '#' || c == '$'))
        return EINVAL;

    /* Remaining characters: letters, digits, or national characters. */
    for (i = 1; i < len; i++) {
        c = (unsigned char)member[i];
        if (!(isupper(c) || isdigit(c) || c == '@' || c == '#' || c == '$'))
            return EINVAL;
    }

    return 0;
}


/* -------------------------------------------------------------------- */
/* Classify a path as ROOT / DATASET (PDS dir) / PDS_MEMBER.            */
/*                                                                      */
/* The path is export-relative in the sense that it begins with an      */
/* export's NFS path; the components that follow name a PDS directory   */
/* (lower-case dsname) and, optionally, a member file:                  */
/*                                                                      */
/*   <export_path>                       -> ROOT                        */
/*   <export_path>/<dirname>             -> DATASET (dataset_idx set)    */
/*   <export_path>/<dirname>/<member>    -> PDS_MEMBER (dataset_idx set) */
/* -------------------------------------------------------------------- */
int mvs_path_type(const char *path, int *export_idx, int *dataset_idx)
{
    int         i;
    int         exp_count = exports_count();  /* export index is zero based */
    export_t   *nfs_export;
    size_t      exp_len;
    size_t      path_len;
    const char *rel;        /* path portion after "<export_path>/" */
    const char *slash;
    char        dirname[MAX_DSNAME_LEN];
    size_t      comp_len;
    int         ds_idx;

    if (export_idx)  *export_idx  = -1;
    if (dataset_idx) *dataset_idx = -1;

    path_len = strlen(path);

    for (i = 0; i < exp_count; i++) {
        nfs_export = exports_get(i);
        exp_len    = strlen(nfs_export->export_path_ebcdic);

        /* Exact match on the export path -> the export root. */
        if (strcmp(path, nfs_export->export_path_ebcdic) == 0) {
            if (export_idx) *export_idx = i;
            return MVS_PATH_TYPE_ROOT;
        }

        /* Otherwise it must be "<export_path>/<something>". */
        if (exp_len >= path_len) continue;
        if (strncmp(path, nfs_export->export_path_ebcdic, exp_len) != 0) continue;
        if (path[exp_len] != '/') continue;

        rel = path + exp_len + 1;    /* first component: the PDS dir name */

        /* Split off the first path component (the PDS directory name). */
        slash = strchr(rel, '/');
        comp_len = (slash == NULL) ? strlen(rel) : (size_t)(slash - rel);
        if (comp_len == 0 || comp_len >= sizeof(dirname))
            return MVS_PATH_NOT_EXPORTED;

        memcpy(dirname, rel, comp_len);
        dirname[comp_len] = '\0';

        ds_idx = export_dataset_find_by_dirname(i, dirname);
        if (ds_idx < 0)
            return MVS_PATH_NOT_EXPORTED;

        if (export_idx)  *export_idx  = i;
        if (dataset_idx) *dataset_idx = ds_idx;

        /* Exactly one component: the PDS directory itself. */
        if (slash == NULL)
            return MVS_PATH_TYPE_DATASET;

        /* A trailing slash names the directory too. */
        if (slash[1] == '\0')
            return MVS_PATH_TYPE_DATASET;

        /* Anything deeper than <dirname>/<member> is not supported. */
        if (strchr(slash + 1, '/') != NULL)
            return MVS_PATH_NOT_EXPORTED;

        return MVS_PATH_TYPE_PDS_MEMBER;
    }

    return MVS_PATH_NOT_EXPORTED;  /* path is not under any export */
}

/* -------------------------------------------------------------------- */
/* Split a member path into the real PDS dataset name and member name.  */
/*                                                                      */
/* Resolves the PDS-directory component to its dataset to obtain the    */
/* real dsname, then extracts the member name (upper-cased, <= 8 chars) */
/* from the leaf component.  The file extension is stripped and         */
/* validated against that dataset's expected extension.  For a PDS      */
/* directory (no member component) the member name is returned empty.   */
/* -------------------------------------------------------------------- */
int mvs_get_pds_dsn_and_member(
    const char          *path,
    char                *pds_dsname,
    char                *pds_member_name,
    int                 export_idx)
{
    int            level;
    int            ds_idx = -1;
    pds_dataset_t *ds;
    char           file_name[MAX_NAME];
    char           file_ext[MAX_FILE_EXT_LEN];
    char          *last_slash;
    char          *last_dot;
    size_t         i;
    int            retcode = 0;

    pds_dsname[0]      = '\0';
    pds_member_name[0] = '\0';
    file_name[0]       = '\0';
    file_ext[0]        = '\0';

    level = mvs_path_type(path, NULL, &ds_idx);
    if (level != MVS_PATH_TYPE_DATASET && level != MVS_PATH_TYPE_PDS_MEMBER) {
        errno = ENOENT;
        return -1;
    }

    ds = export_dataset_get(export_idx, ds_idx);
    if (ds == NULL) {
        errno = ENOENT;
        return -1;
    }

    /* The dataset name is the real PDS name for this directory. */
    strncpy(pds_dsname, ds->dsname_ebcdic, 44);
    pds_dsname[44] = '\0';

    /* A PDS directory itself has no member name. */
    if (level == MVS_PATH_TYPE_DATASET)
        goto return_exit;

    /* PDS member: parse the leaf component of the path. */
    last_slash = strrchr(path, '/');
    if (last_slash == NULL) {
        log_error("mvs_get_pds_dsn_and_member: no slash in member path %s", path);
        goto return_exit;
    }
    strncpy(file_name, last_slash + 1, MAX_NAME - 1);
    file_name[MAX_NAME - 1] = '\0';

    /* Split off the file extension (if any). */
    last_dot = strrchr(file_name, '.');
    if (last_dot) {
        strncpy(file_ext, last_dot + 1, MAX_FILE_EXT_LEN - 1);
        file_ext[MAX_FILE_EXT_LEN - 1] = '\0';
        *last_dot = '\0';
    }

    /* The leaf (extension stripped) becomes the member name.  Upper-case it,
       then validate it as a PDS member name -- we reject rather than silently
       truncate/mangle, so distinct files such as "report01a" and "report01b"
       cannot collapse to the same 8-char member and overwrite each other. */
    for (i = 0; file_name[i] != '\0'; i++)
        file_name[i] = (char)toupper((unsigned char)file_name[i]);

    retcode = mvs_member_name_valid(file_name);
    if (retcode != 0) {
        /* mvs_member_name_valid() reports the precise fault (ENAMETOOLONG for
           length, EINVAL for a bad/leading-digit char) -- logged below for
           diagnostics.  But we surface every "can't be a member name" case to
           the client as ENAMETOOLONG (-> NFS3ERR_NAMETOOLONG): Windows maps
           NFS3ERR_INVAL to the confusing "The volume for a file has been
           externally altered..." message, whereas NAMETOOLONG gives the clear
           "The filename or extension is too long." which points the user at
           the target filename -- the thing they actually need to fix. */
        log_debug("mvs_get_pds_dsn_and_member: invalid member name '%s' (errno %d)"
                  " -> reporting ENAMETOOLONG", file_name, retcode);
        errno   = ENAMETOOLONG;
        retcode = -1;
        goto return_exit;
    }

    /* Valid, so <= 8 characters -- copy to the caller's 9-byte buffer. */
    strncpy(pds_member_name, file_name, 8);
    pds_member_name[8] = '\0';

    /* Validate the extension against this dataset's expected extension.
       Case of the extension does not matter.
       This is deliberately strict: the extension is the per-dataset display
       convention (a ".cntl" dataset shows "name.cntl"), and enforcing it on
       input keeps the filename<->member mapping 1:1.  Without it, "name.jcl"
       and "name.txt" would both map to member NAME and silently overwrite
       each other.  A cross-dataset copy must therefore name the destination
       with the target dataset's extension (e.g. copy ... to name.cntl). */
    if (ds->file_ext[0] != '\0') {
        if (strcasecmp(file_ext, ds->file_ext) != 0) {
            errno   = ENOENT;
            retcode = -1;
            goto return_exit;
        }
    }

return_exit:
    log_debug("mvs_get_pds_dsn_and_member: dsname=%s member=%s (ext '%s' vs '%s') rc=%d",
        pds_dsname, pds_member_name, file_ext,
        (ds != NULL) ? ds->file_ext : "", retcode);

    return retcode;
}

/* -------------------------------------------------------------------- */
/* Some routines for handling bytes, strings, and BCD / packed decimal  */
/* -------------------------------------------------------------------- */
void bytes_to_string(
    unsigned char *dest,
    unsigned char *source,
    int            source_len)
{
    int x = source_len;
    memcpy(dest, source, source_len);
    dest[x--] = '\0';
    while ( dest[x] == ' ' && x >= 0 )
        dest[x--] = '\0';
}

int bcd_byte_to_int(unsigned char bcdval_in)
{
    int bcdval = bcdval_in;
    return 10 * ((bcdval >> 4) & 0x0F) + (bcdval & 0x0F);
}

/* -------------------------------------------------------------------- */
/* Get basic DCB info for dataset                                       */
/* -------------------------------------------------------------------- */
int mvs_get_dcb_info_dsn(const char *dsname, mvs_dcb_info_t *dcb) {
    int fh;
    int rc;
    int open_flags = _O_BINARY|_O_RDONLY;
    int permission_flags = 0;
    char filename[6 + 44 + 1];
    char *opendcb = "";

    strcpy(filename, "//DSN:");
    strcat(filename, dsname);

    fh = _open(filename, open_flags, permission_flags, opendcb);
    if ( fh < 0 )
        return -1;

    rc = __getdcb(fh, 
        &dcb->dsorg, &dcb->recfm, &dcb->keylen,
        &dcb->lrecl, &dcb->blksize );
    
    _close(fh);
    return rc;
}

char *mvs_dcb_dsorg_to_str(uint8_t dsorg) {
    
}

