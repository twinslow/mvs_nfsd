#ifndef MVSPDIR_H_INCLUDED
#define MVSPDIR_H_INCLUDED

#include <stdio.h>
#include <time.h>
#include "types.h"

/* -------------------------------------------------------------------- */
/* Member list and management functions                                 */
/* -------------------------------------------------------------------- */

#define MVSPDIR_MLIST_INITIAL_SIZE      40
#define MVSPDIR_MLIST_INCREMENT_SIZE    40

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

typedef struct {
    int32_t             list_size;
    int32_t             number_in_list;
    pds_member_entry_t *list;
} pds_member_list_t;

/* Initialize a new list structure. */
int mvspdir_mlist_init(pds_member_list_t *mlist);

/* Get the next free entry in the list -- expands list if required */
pds_member_entry_t *mvspdir_mlist_getfree(pds_member_list_t *mlist);

/* Add the entry to the list as last entry -- sets list size */
int mvspdir_mlist_setaslast(pds_member_list_t *mlist, pds_member_entry_t *entry);

/* Expand list */
int mvspdir_mlist_expand(pds_member_list_t *mlist);

/* Shrink list to its current used size (which may be zero) */
int mvspdir_mlist_shrink(pds_member_list_t *mlist);

/* Free the list's storage and reset it to an empty/initial state */
void mvspdir_mlist_free(pds_member_list_t *mlist);


/* -------------------------------------------------------------------- */
/* ISPF statistics                                                      */
/* -------------------------------------------------------------------- */
time_t convert_ispf_datetime(
    uint8_t         *dateptr,
    uint8_t         *hhmmf,
    uint8_t          seconds);

void mvs_extract_ispf_stats(
    pds_member_entry_t  *entry,
    unsigned char       *userdata,
    int                  user_data_len);

void mvs_set_no_ispf_stats(pds_member_entry_t *entry);

/* -------------------------------------------------------------------- */
/* Directory entry parsing                                              */
/* -------------------------------------------------------------------- */

#define MVS_PDSDIR_IFLG_ISPFSTATS        0x80
#define MVS_PDSDIR_ENDMARK          "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
#define MVS_PDSDIR_USERDATA_COUNT_MASK ((unsigned char) 0x1F)
#define MVS_PDSDIR_ALIAS_MASK   ((unsigned char) 0x80)
#define MVS_PDSDIR_ISPF_EXT_STATS ((unsigned char) 0x04)


void mvs_pds_member_entry_init(pds_member_entry_t *entry);

int  mvs_pds_member_entry_set(
    pds_member_entry_t  *entry,
    const uint8_t       *start_blockptr);

int  mvs_skip_dir_entry(const uint8_t *start_blockptr);

/* Append one member entry (parsed at blockptr) to mlist, growing it as
   needed.  Returns the byte length of the parsed entry, or -1 on
   allocation failure (errno=ENOMEM). */
int  mvs_extract_dir_entry(
    const uint8_t       *blockptr,
    pds_member_list_t   *mlist);

/* Append every member in the block whose name >= start_member to mlist,
   growing the list as needed.  Sets *end_of_dir when the PDS end mark is
   reached.  Returns 0 on success, -1 on error (errno set). */
int  mvs_process_dir_block(
    const uint8_t       *block_data,
    const char          *start_member,
    pds_member_list_t   *mlist,
    int                 *end_of_dir);

/* -------------------------------------------------------------------- */
/* PDS dataset I/O                                                      */
/* -------------------------------------------------------------------- */

int mvs_open_pds_dir(
    const char  *dsname,
    int          export_idx,
    FILE       **pds_dir_fh);

int mvs_close_pds_dir(FILE *pds_dir_fh);

/* Read directory blocks starting at start_member, appending every member
   with name >= start_member into mlist (which grows as needed) until the
   end of the directory.  Sets *end_of_dir when the PDS end mark is seen.
   Members are appended; the caller supplies an (empty) list.
   Returns 0 on success, -1 on error (errno set). */
int mvs_read_pds_dir(
    FILE                *pds_dir_fh,
    const char          *start_member,
    pds_member_list_t   *mlist,
    int                 *end_of_dir);

/* -------------------------------------------------------------------- */
/* High-level member access                                             */
/* -------------------------------------------------------------------- */

int mvs_pds_member_list(
    const char          *dsname,
    int                  export_idx,
    const char          *start_member,
    pds_member_list_t   *mlist,
    int                 *end_of_dir);

pds_member_entry_t *mvs_pds_get_member_entry(
    const char  *dsname,
    const char  *member,
    int          export_idx,
    pds_member_entry_t *member_entry);




#endif
