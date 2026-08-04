#ifndef MVSBLKC_H_INCLUDED
#define MVSBLKC_H_INCLUDED

#include "types.h"
#include "nfsd.h"        /* dataset_dscb_info_t, mvs_dscb_info_t via asmutils */
#include "asmutils.h"

/*--------------------------------------------------------------------------*/
/* JCC truncates external symbols to 8 characters, and "blkcalc_" is         */
/* already 8, so EVERY public name here needs an alias -- without them all   */
/* of them would collide with each other.                                    */
/*--------------------------------------------------------------------------*/
#define blkcalc_info_init            blkcInit
#define blkcalc_add_blocks_for_data  blkcAddB
#define blkcalc_will_member_fit      blkcFit
#define blkcalc_dataset_init         blkcDsIn
#define blkcalc_admit_write          blkcAdmW
#define blkcalc_total_blocks         blkcTotB
#define blkcalc_slot_reset           blkcRset


typedef enum {
    RECFM_F = 1,
    RECFM_V,
    RECFM_U
} blkcalc_recfm_t;

typedef struct
{
    blkcalc_recfm_t     dcb_recfm;
    int                 dcb_blksize;
    int                 dcb_lrecl;

    int                 count_full_blocks;
    int                 size_last_partial_block;    /* Or 0 if last block was full */

    /* This is the number of characters in the last text line of the       */
    /* write segment that wasn't terminated by a NL ... or 0               */
    int                 last_unterm_text_line_chars;

    /* Byte offset of the member content this estimate has folded in.      */
    /* The estimate is only meaningful while writes arrive as a strict     */
    /* sequential append; this is what lets blkcalc_admit_write() detect   */
    /* an overwrite or an out-of-order write and rebuild from scratch      */
    /* instead of silently double counting.  See                           */
    /* doc/design_pds_full_prediction.md Sec 7.1.                          */
    uint32_t            consumed_upto;

} blkcalc_info_t;


/*--------------------------------------------------------------------------*/
/* Initialize the blkcalc_info_t structure                                  */
/*--------------------------------------------------------------------------*/
void blkcalc_info_init(
    blkcalc_info_t  *blkcalc_info,
    blkcalc_recfm_t  recfm,
    int              block_size,
    int              logical_rec_len);

/*--------------------------------------------------------------------------*/
/* For the given data buffer, add the estimated block space required        */
/* This is updating the cumulative count_full_blocks and                    */
/* size_last_partial_block in the blkcalc_info_t structure                  */
/* Called for each NFS write                                                */
/*                                                                          */
/* Returns 0.  A trailing line with no NL is NOT counted here: its length   */
/* is carried in last_unterm_text_line_chars so that a line split across    */
/* two writes is counted once, from its full length.                        */
/*--------------------------------------------------------------------------*/
int blkcalc_add_blocks_for_data(
    blkcalc_info_t  *blkcalc_info,
    const char      *buff,                   /* Text buffer being processed */
    int              buff_len);              /* Length of the text buffer   */

/*--------------------------------------------------------------------------*/
/* Total blocks this member would occupy, INCLUDING any partial last block  */
/* and any trailing unterminated line.  Does not modify the estimate.       */
/*--------------------------------------------------------------------------*/
int blkcalc_total_blocks(const blkcalc_info_t *blkcalc_info);

/*--------------------------------------------------------------------------*/
/* Determine if the estimated block count + final block will fit into the   */
/* PDS if STOW'd.                                                           */
/* Returns 0 if it is predicted to fit, or -1 if it isn't                   */
/* Returns a predicted last block ttr                                       */
/*                                                                          */
/* This function can be called repeatedly, for each pending slot for the    */
/* same PDS to determine if all "pending" writes can be satisfied: pass     */
/* the predicted_last_block_ttr of one call as the last_block_ttr of the    */
/* next.                                                                    */
/*                                                                          */
/* ds_info supplies the geometry and the current end of the dataset.        */
/* Taking it as a pointer rather than looking it up from export/dataset     */
/* indexes keeps this function pure arithmetic over its arguments, so       */
/* tests/tmvsblkc.c can exercise it with no export table and no VTOC.       */
/*                                                                          */
/* A TTR here is packed as (track << 8) | record, matching DS1LSTAR.        */
/*--------------------------------------------------------------------------*/
int blkcalc_will_member_fit(
    const blkcalc_info_t      *blkcalc_info,
    const dataset_dscb_info_t *ds_info,
    uint32_t                   last_block_ttr,
    uint32_t                  *predicted_last_block_ttr
);

/*--------------------------------------------------------------------------*/
/* Config time: decode one raw VTOC entry into the C friendly form and      */
/* derive blocks_per_track.  Sets out->valid only when everything needed    */
/* for a space prediction is present and usable.                            */
/* Returns 0 on success, -1 if the entry cannot be used (out->valid = 0).   */
/*--------------------------------------------------------------------------*/
int blkcalc_dataset_init(
    dataset_dscb_info_t   *out,
    const mvs_dscb_info_t *raw);

/*--------------------------------------------------------------------------*/
/* The single entry point the write path calls.                             */
/*                                                                          */
/* Decides whether one NFS WRITE can be admitted: folds the data into a     */
/* COPY of the member's estimate, re-reads the dataset's end from the VTOC, */
/* then checks that this member AND every other member pending for the same */
/* dataset would still fit.  Only on success is the copy committed back.    */
/* On refusal nothing is modified, so no rollback is needed.                */
/*                                                                          */
/* 'pm' is a pending_member_t *; declared void * so this header does not    */
/* have to include mvspww.h (which includes this one).                      */
/*                                                                          */
/* Returns 0 to admit, or -1 with errno set (ENOSPC predicted full, EIO on  */
/* a VTOC read failure that leaves us unable to decide).                    */
/*--------------------------------------------------------------------------*/
int blkcalc_admit_write(
    void            *pm,
    const uint8_t   *data,
    uint32_t         count,
    uint64_t         offset);

/*--------------------------------------------------------------------------*/
/* (Re)initialise a pending slot's estimate from its dataset's record        */
/* format.  Call when a slot is created, and again when a CREATE truncates   */
/* one that already existed -- otherwise the re-created member inherits the  */
/* previous one's block count.                                              */
/*                                                                          */
/* 'pm' is a pending_member_t *; void * for the same reason as above.        */
/*--------------------------------------------------------------------------*/
void blkcalc_slot_reset(void *pm);


#endif /* MVSBLKC_H_INCLUDED */
