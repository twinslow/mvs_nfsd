#ifndef MVSIO_H_INCLUDED
#define MVSIO_H_INCLUDED

#include "types.h"

typedef struct {
    int32_t          crdate;
    int32_t          chgdate;
    int32_t          size;
    int32_t          initSize;
    int32_t          modCount;
    char             name[9];
    char             user[9];
    uint16_t         first_block_tt;
    uint8_t          entry_type;
    uint8_t          ver;
    uint8_t          mod;
    uint8_t          first_block_rec;
    uint8_t          ispf_flags;
} pds_member_entry_t;

int mvs_path_type(const char *path, int *export_idx);
int mvs_get_pds_dsn_and_member(
    const char          *path, 
    char                *pds_dsname, 
    char                *pds_member_name, 
    int                 export_idx);
pds_member_entry_t *mvs_pds_get_member_entry(const char *dsname, const char *member, int export_idx);

#endif