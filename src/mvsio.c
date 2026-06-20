

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

#include "nfsd.h"
#include "mvsio.h"
#include "ebcdic.h"



/* -------------------------------------------------------------------- */
/* Determine if path is PDS dataset, or PDS member?                     */
/* -------------------------------------------------------------------- */
int mvs_path_type(const char *path, int *export_idx) {
    int i;
    int exp_count = exports_count();        /* export index number is zero based. */
    export_t *nfs_export;
    size_t host_path_len;

    for (i = 0; i < exp_count; i++) {
        nfs_export = exports_get(i);
        if (strcmp(path, nfs_export->host_path_ebcdic) == 0) {
            *export_idx = i;
            return MVS_PATH_TYPE_DATASET; // Path is an export path
        }
        /* Does the given path up to the last '/' match an export_path? */
        host_path_len = strlen(nfs_export->host_path_ebcdic);
        if (host_path_len < strlen(path) && 
            strncmp(path, nfs_export->host_path_ebcdic, host_path_len) == 0 &&
            path[host_path_len] == '/') {
            *export_idx = i;
            return MVS_PATH_TYPE_PDS_MEMBER; // Path is a file (PDS member)
        }
    }
    return MVS_PATH_NOT_EXPORTED; // Path is not an export path
}

/* -------------------------------------------------------------------- */
/* Split the file path                                                  */
/* Get the real PDS dataset name from the export definition and         */
/* and extract the PDS member name from the file name in the path       */
/* -------------------------------------------------------------------- */
int mvs_get_pds_dsn_and_member(
    const char          *path, 
    char                *pds_dsname, 
    char                *pds_member_name, 
    int                 export_idx) 
{
    char      *export_path;
    char      *host_path;
    char       file_name[MAX_NAME];
    char       file_ext[MAX_FILE_EXT_LEN];
    size_t     host_path_len;
    char      *last_slash;
    char      *last_dot;
    size_t     member_name_len;
    size_t     i;
    int        retcode;

    export_path = exports_get(export_idx)->export_path_ebcdic;
    host_path = exports_get(export_idx)->host_path_ebcdic;

    /* The dataset name is the host path from the export definition */
    strncpy(pds_dsname, host_path, 44);
    pds_dsname[44] = '\0'; // Ensure null-termination

    /* The member name is the part of the path after the export path */
    host_path_len = strlen(host_path);
    if (strlen(path) <= host_path_len) {
        pds_member_name[0] = '\0'; // No member name, this is the dataset itself
        retcode = 0;
        goto return_exit;
    }

    /* Extract the file name from the end of the path, which can include file extension*/
    //Find the last '/' in the path
    last_slash = strrchr(path, '/');
    if (last_slash) {
        strncpy(file_name, last_slash + 1, MAX_NAME - 1);
        file_name[MAX_NAME - 1] = '\0'; // Ensure null-termination
    }
    /* Get the file extension (if any) from the file name */
    last_dot = strrchr(file_name, '.');
    if (last_dot) {
        strncpy(file_ext, last_dot + 1, MAX_FILE_EXT_LEN - 1);
        file_ext[MAX_FILE_EXT_LEN - 1] = '\0'; // Ensure null-termination
        *last_dot = '\0'; // Remove extension from file_name
    } else {
        file_ext[0] = '\0'; // No extension
    }
     /* PDS member name is the upper case version of remaining file name, truncated to 8 characters*/
    member_name_len = strlen(file_name);
    if (member_name_len > 8) member_name_len = 8;
    for (i = 0; i < member_name_len; i++) {
        pds_member_name[i] = toupper((unsigned char)file_name[i]);  
    }

    pds_member_name[member_name_len] = '\0'; // Ensure null-termination

    /*Does the file name extension match the expected extension which is in the export definition?*/
    /*Case of file extension does not matter                                                      */
    if (exports_get(export_idx)->file_ext[0] != '\0') {
        if (strcasecmp(file_ext, exports_get(export_idx)->file_ext) != 0) {
            errno = ENOENT; // File extension does not match expected extension, treat as file not found
            retcode = -1;
            goto return_exit;
        }
    }

    retcode = 0;
    
return_exit:

    log_debug("mvs_get_pds_dsn_and_member: Returning dsname %s and member %s, with retcode %d",
        pds_dsname, pds_member_name, retcode);

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


