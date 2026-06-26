#ifndef MVSPDIR_H_INCLUDED
#define MVSPDIR_H_INCLUDED

#include <stdio.h>
#include <time.h>
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
    uint8_t          info_flags;
} pds_member_entry_t;

#define MVS_PDSDIR_IFLG_ISPFSTATS        0x80

#define MVS_PDSDIR_ENDMARK          "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
#define MVS_PDSDIR_USERDATA_COUNT_MASK ((unsigned char) 0x1F)
#define MVS_PDSDIR_ALIAS_MASK   ((unsigned char) 0x80)
#define MVS_PDSDIR_ISPF_EXT_STATS ((unsigned char) 0x04)

time_t convert_ispf_datetime(
    uint8_t         *dateptr,
    uint8_t         *hhmmf,
    uint8_t          seconds);

/* -------------------------------------------------------------------- */
/* ISPF statistics                                                      */
/* -------------------------------------------------------------------- */

void mvs_extract_ispf_stats(
    pds_member_entry_t  *entry,
    unsigned char       *userdata,
    int                  user_data_len);

void mvs_set_no_ispf_stats(pds_member_entry_t *entry);

/* -------------------------------------------------------------------- */
/* Directory entry parsing                                              */
/* -------------------------------------------------------------------- */

void mvs_pds_member_entry_init(pds_member_entry_t *entry);

int  mvs_pds_member_entry_set(
    pds_member_entry_t  *entry,
    const uint8_t       *start_blockptr);

int  mvs_skip_dir_entry(const uint8_t *start_blockptr);

int  mvs_extract_dir_entry(
    const uint8_t       *blockptr,
    pds_member_entry_t  *member_entries,
    int                  max_members,
    int                 *num_members_returned);

int  mvs_process_dir_block(
    const uint8_t       *block_data,
    const char          *start_member,
    int                  max_members,
    pds_member_entry_t  *member_entries,
    int                 *num_members_returned,
    int                 *end_of_dir);

/* -------------------------------------------------------------------- */
/* PDS dataset I/O                                                      */
/* -------------------------------------------------------------------- */

int mvs_open_pds_dir(
    const char  *dsname,
    int          export_idx,
    FILE       **pds_dir_fh);

int mvs_close_pds_dir(FILE *pds_dir_fh);

int mvs_read_pds_dir(
    FILE                *pds_dir_fh,
    const char          *start_member,
    int                  max_members,
    pds_member_entry_t  *member_entries,
    int                 *num_members_returned,
    int                 *end_of_dir);

/* -------------------------------------------------------------------- */
/* High-level member access                                             */
/* -------------------------------------------------------------------- */

int mvs_pds_member_list(
    const char          *dsname,
    int                  export_idx,
    const char          *start_member,
    int                  max_members,
    pds_member_entry_t  *member_entries,
    int                 *num_members_returned,
    int                 *end_of_dir);

pds_member_entry_t *mvs_pds_get_member_entry(
    const char  *dsname,
    const char  *member,
    int          export_idx,
    pds_member_entry_t *member_entry);




#endif
