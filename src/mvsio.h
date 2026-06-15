#ifndef MVSIO_H_INCLUDED
#define MVSIO_H_INCLUDED

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
} pds_member_entry_t;

#define MVS_PATH_NOT_EXPORTED       -1
#define MVS_PATH_TYPE_DATASET       1
#define MVS_PATH_TYPE_PDS_MEMBER    2

#define MVS_PDSDIR_ENDMARK          "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
#define MVS_PDSDIR_USERDATA_COUNT_MASK ((unsigned char) 0x1F)
#define MVS_PDSDIR_ALIAS_MASK   ((unsigned char) 0x80)
#define MVS_PDSDIR_ISPF_EXT_STATS ((unsigned char) 0x04)

#define MVS_RCACHE_ENTRIES                      20
#define MVS_RCACHE_STATUS_USED                  1
#define MVS_RCACHE_STATUS_UNUSED                0
#define MVS_RCACHE_MAX_AGE_SECONDS              5

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
/* BCD / date-time utilities                                            */
/* -------------------------------------------------------------------- */

int    bcd_byte_to_int(unsigned char bcdval_in);

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
    int          export_idx);


/* -------------------------------------------------------------------- */
/* Read cache                                                           */
/* -------------------------------------------------------------------- */
typedef struct mvs_rcache_entry {
    uint8_t    status;     /* 0 = not in use, 1 = in use */

    char       dsname[45];
    char       member_name[9];
    int        export_idx;

    time_t     last_used_time; // Last used time for this cache entry, for LRU eviction

    uint32_t   last_offset; // Last read offset from NFS client
    uint32_t   last_nread;  // Number of bytes returned to client for last read
    fpos_t     last_getpos; // File position on host at end of the last read
    uint8_t    has_last_getpos; // 1 if last_getpos holds a usable position

} mvs_rcache_entry_t;

void mvs_rcache_init(void);
mvs_rcache_entry_t *mvs_rcache_find_entry(
    const char *dsname,
    const char *member_name,
    int export_idx);
void mvs_rcache_entry_reset(mvs_rcache_entry_t *entry);
mvs_rcache_entry_t *mvs_rcache_get_free_entry(void);

int mvs_pds_member_open(
    mvs_rcache_entry_t *cache_entry,
    FILE               **fh_return);

int mvs_pds_member_close(
    mvs_rcache_entry_t *cache_entry,
    FILE               *fh);

int mvs_pds_member_pread(
    mvs_rcache_entry_t *cache_entry,
    FILE               *fh,
    uint8_t            *buff,
    uint32_t            count,
    uint64_t            offset,
    uint32_t           *nread,
    int                *eof);


/* -------------------------------------------------------------------- */
/* High-level file read                                                 */
/* -------------------------------------------------------------------- */

int mvs_pds_member_read(
    const char  *dsname,
    const char  *member,
    int          export_idx,
    uint64_t     offset,
    uint32_t     count,
    uint8_t     *buf,
    uint32_t    *nread,
    int         *eof);


#endif
