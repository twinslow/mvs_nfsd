#ifndef MVSIO_H_INCLUDED
#define MVSIO_H_INCLUDED

#include <stdio.h>
#include <time.h>
#include "types.h"

#define MVS_PATH_NOT_EXPORTED       -1
#define MVS_PATH_TYPE_DATASET       1
#define MVS_PATH_TYPE_PDS_MEMBER    2

/* -------------------------------------------------------------------- */
/* Path classification                                                  */
/* -------------------------------------------------------------------- */

int mvs_path_type(const char *path, int *export_idx);

int mvs_get_pds_dsn_and_member(
    const char      *path,
    char            *pds_dsname,
    char            *pds_member_name,
    int              export_idx);


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

#endif
