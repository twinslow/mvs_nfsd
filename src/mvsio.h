#ifndef MVSIO_H_INCLUDED
#define MVSIO_H_INCLUDED

#include <stdio.h>
#include <time.h>
#include "types.h"

#define MVS_PATH_NOT_EXPORTED       -1
#define MVS_PATH_TYPE_ROOT          0   /* export root: virtual dir of PDS dirs */
#define MVS_PATH_TYPE_DATASET       1   /* a PDS directory (one dataset)        */
#define MVS_PATH_TYPE_PDS_MEMBER    2   /* a member file within a PDS           */

/* -------------------------------------------------------------------- */
/* Path classification                                                  */
/*                                                                      */
/* Classifies an (EBCDIC) NFS path into ROOT / DATASET / PDS_MEMBER and */
/* returns the owning export index and, for DATASET/PDS_MEMBER, the     */
/* index of the dataset within that export (dataset_idx = -1 for ROOT). */
/* Either output pointer may be NULL if the caller does not need it.    */
/* -------------------------------------------------------------------- */

int mvs_path_type(const char *path, int *export_idx, int *dataset_idx);

int mvs_get_pds_dsn_and_member(
    const char      *path,
    char            *pds_dsname,
    char            *pds_member_name,
    int              export_idx);

/*
 * Validate a PDS member name (the upper-cased stored form).  Returns 0 if it
 * is a valid member name, else an errno: ENAMETOOLONG (> 8 chars) or EINVAL
 * (empty, starts with a digit, or contains an invalid character).  IBM rules:
 * 1-8 chars; first char a letter (A-Z) or national (@ # $), not a digit;
 * remaining chars letters, digits, or national.
 */
int mvs_member_name_valid(const char *member);


/* -------------------------------------------------------------------- */
/* Bytes and special string handling                                    */
/* -------------------------------------------------------------------- */
void bytes_to_string(
    unsigned char *dest,
    unsigned char *source,
    int            source_len);

/* -------------------------------------------------------------------- */
/* BCD / date-time utilities                                            */
/* -------------------------------------------------------------------- */

int    bcd_byte_to_int(unsigned char bcdval_in);

/* -------------------------------------------------------------------- */
/* Get DCB info for dataset                                             */
/* -------------------------------------------------------------------- */

typedef struct {
    uint8_t     dsorg;
    uint8_t     recfm;
    uint8_t     keylen;
    uint16_t    lrecl;
    uint16_t    blksize;
} mvs_dcb_info_t;

#define MVS_DCB_DSORG_PO        0x40
#define MVS_DCB_DSORG_PS        0x02
#define MVS_DCB_RECFM_V         0x40
#define MVS_DCB_RECFM_VB        0x50
#define MVS_DCB_RECFM_F         0x80
#define MVS_DCB_RECFM_FB        0x90
#define MVS_DCB_RECFM_U         0xC0

int mvs_get_dcb_info_dsn(const char *dsname, mvs_dcb_info_t *dcb);


#endif
