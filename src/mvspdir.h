#ifndef MVSPDIR_H_INCLUDED
#define MVSPDIR_H_INCLUDED

#include <stdio.h>
#include <time.h>
#include "types.h"

/* -------------------------------------------------------------------- */
/* Member list and management functions                                 */
/* -------------------------------------------------------------------- */

#define MVSPDIR_MLIST_INITIAL_SIZE      40

/* NO LONGER USED.  mvspdir_mlist_expand() DOUBLES the capacity instead of
   adding a fixed increment: linear growth fragmented the C heap badly
   enough to make the region ratchet upwards (see the note in that
   function).  Kept only so an out-of-tree caller does not fail to compile;
   delete once nothing references it. */
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

/* Length (bytes) of the non-extended ISPF statistics user-data area.
   15 halfwords -- must be even and <= 62 for __setstow(). */
#define MVS_ISPF_STATS_LEN   30

/* Count the text records an ASCII buffer will produce when written to a
   PDS member: one per LF (0x0A), plus a final record if the buffer is
   non-empty and does not end in LF.  (The buffer is ASCII here -- it is
   translated to EBCDIC only at fwrite time -- so LF is 0x0A, NOT '\n',
   which would be the EBCDIC newline under JCC.) */
int32_t mvs_ispf_count_lines(const uint8_t *buf, uint32_t len);

/* Encode 'entry' into the 30-byte non-extended ISPF user-data area -- the
   strict inverse of mvs_extract_ispf_stats().  ver/mod are stored as 1-byte
   binary; size fields as native uint16 (clamped to 0xFFFF); dates as packed
   decimal via gmtime().  Returns MVS_ISPF_STATS_LEN. */
int mvs_encode_ispf_stats(const pds_member_entry_t *entry, uint8_t *userdata);

/* Build the ISPF stats to STOW when writing a member.  'existing' is the
   member's current directory entry, or NULL for a brand-new member.  Fills
   '*entry' with the stats to write and returns 1 if stats should be set, or
   0 if not (an existing member that had no ISPF stats is left without any).
   Rules: new -> ver/mod 01, created=now, init size=lines, id "NFSD";
   existing-with-stats -> keep ver/created/init/mod-count/id, bump changed to
   now, size=lines, modification level +1 (capped at 99). */
int mvs_build_write_stats(
    pds_member_entry_t        *entry,
    const pds_member_entry_t  *existing,
    int32_t                    line_count,
    time_t                     now);

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

/* Fold an in-memory member list into a 32-bit signature: FNV-1a over each
   entry's name and TTR (first_block_tt / first_block_rec), plus the count.
   Deterministic and stable for an unchanged, same-order list; changes on
   any add / remove / replace.  Pure (no I/O) so it can be unit-tested. */
uint32_t mvs_pds_dir_sig_calc(const pds_member_entry_t *list, int count);

/* Compute a cheap signature of a PDS directory's current contents by
   reading the directory and folding it via mvs_pds_dir_sig_calc.  The
   value changes when members are added, removed, or replaced (new TTR),
   letting a caller detect out-of-band changes that did not go through the
   NFS write path.  It cannot detect an in-place update that preserves
   name, length, and TTR (a "zap").
   Returns 0 and sets *sig_out on success, or -1 on error (errno set). */
int mvs_pds_dir_signature(
    const char  *dsname,
    int          export_idx,
    uint32_t    *sig_out);




#endif
