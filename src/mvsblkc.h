#ifndef MVSBLKC_H_INCLUDED
#define MVSBLKC_H_INCLUDED

#include "types.h"


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
/*--------------------------------------------------------------------------*/
int blkcalc_add_blocks_for_data(
    blkcalc_info_t  *blkcalc_info,
    const char      *buff,                   /* Text buffer being processed */
    int              buff_len);              /* Length of the text buffer   */

/*--------------------------------------------------------------------------*/
/* Determine if the estimated block count + final block will fit into the   */
/* PDS if STOW'd.                                                           */
/* Returns 0 if it is predicted to fit, or -1 if it isn't                   */
/* Returns a predicted last block ttr                                       */
/*                                                                          */
/* This function can be called repeatedly, for each pending slot for the    */
/* same PDS to determine if all "pending" writes can be satisfied           */
/*                                                                          */
/* Uses export_idx and dataset_idx get current space allocation and usage   */
/* for the dataset.                                                         */
/*--------------------------------------------------------------------------*/
int blkcalc_will_member_fit(
    blkcalc_info_t  *blkcalc_info,
    int              export_idx,
    int              dataset_idx,
    uint32_t         last_block_ttr,
    uint32_t        *predicted_last_block_ttr
);


#endif /* MVSBLKK_H_INCLUDED */

