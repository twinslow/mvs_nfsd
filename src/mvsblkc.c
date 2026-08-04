/*
 * mvsblkc.c - Will this member still fit in the PDS?
 *
 * See doc/design_pds_full_prediction.md.  The point of this module is to
 * refuse an NFS WRITE that we can see will not fit, so the flush never
 * abends SB14 out of space -- an abend that can leave JCC's file lock held
 * and hang the whole server (doc/analysis_io_lock_hang.md).
 *
 * Two halves:
 *
 *   1. RECORD COUNTING.  The flush does not build records itself: it opens
 *      the member "wt" and fwrites the whole EBCDIC stream, letting JCC
 *      split it on newline.  So the arithmetic here has to reproduce what
 *      JCC does, which is (measured, design Sec 7.3):
 *          - every line, INCLUDING empty ones and a trailing run with no
 *            newline, becomes at least one record;
 *          - a line longer than the record length WRAPS into whole records
 *            on both F/FB and V/VB;
 *          - F/FB records are padded to LRECL, V/VB records are not and
 *            carry a 4 byte RDW, with a 4 byte BDW per block.
 *      A carriage return is NOT special: on a CRLF stream the CR is an
 *      ordinary data byte and ends up in the record, which is exactly what
 *      the flush produces, so counting it as data is what makes the
 *      prediction match reality.
 *
 *   2. FITTING.  Blocks are turned into a position in the dataset and
 *      compared against what is allocated plus what it may still extend by.
 *      Deliberately conservative at every choice: predicting "fits" when it
 *      does not is the failure that costs us an abend, while predicting
 *      "full" when there was room only costs an ENOSPC the client reports.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>
#include <errno.h>

#include "types.h"
#include "nfsd.h"
#include "asmutils.h"
#include "mvsblkc.h"
#include "mvsutl.h"
#include "mvspww.h"
#include "logger.h"

/* Scratch for re-reading a member when the estimate has to be rebuilt.
   Only the recompute path uses it, and the server is single threaded. */
static uint8_t g_blkc_scan[4096];

/* A TTR as DS1LSTAR packs it: two bytes of track, one of record. */
#define BLKC_TTR(tt, r)   ((((uint32_t)(tt)) << 8) | ((uint32_t)(r) & 0xFFu))
#define BLKC_TTR_TT(ttr)  ((uint32_t)((ttr) >> 8))
#define BLKC_TTR_R(ttr)   ((uint32_t)((ttr) & 0xFFu))

/* Maximum extents a dataset may hold on one volume.  Once it has them all
   it cannot extend again however much secondary space is defined. */
#define BLKC_MAX_EXTENTS  16


/* ==================================================================== */
/* Record counting                                                       */
/* ==================================================================== */

/* Records a text line of n characters becomes.  An empty line is still one
   record; anything longer than the record length wraps into whole records. */
static int blkc_recs_for_line(int n, int reclen)
{
    if (reclen <= 0)
        return 0;
    if (n <= 0)
        return 1;
    return (n + reclen - 1) / reclen;
}

/* Fold one completed text line of 'n' characters into the running block
   totals for a FIXED format dataset.

   Records are all LRECL bytes and a block holds a whole number of them, so
   this is pure division -- no need to walk record by record. */
static void blkc_emit_line_f(blkcalc_info_t *bi, int n)
{
    int recs_per_blk = bi->dcb_blksize / bi->dcb_lrecl;
    int recs_open;
    int total;

    if (recs_per_blk <= 0)
        return;                       /* rejected at config time */

    recs_open = bi->size_last_partial_block / bi->dcb_lrecl;
    total     = recs_open + blkc_recs_for_line(n, bi->dcb_lrecl);

    bi->count_full_blocks       += total / recs_per_blk;
    bi->size_last_partial_block  = (total % recs_per_blk) * bi->dcb_lrecl;
}

/* The same for a VARIABLE format dataset.

   Records vary in length, so they are placed one at a time.  Each costs a
   4 byte RDW on top of its data, and each block costs a 4 byte BDW.  A
   record that will not fit the open block starts a new one; it always fits
   an empty block because config time rejects LRECL > BLKSIZE - 4. */
static void blkc_emit_line_v(blkcalc_info_t *bi, int n)
{
    int data_max = bi->dcb_lrecl - 4;      /* usable data per record */
    int nrecs;
    int i;
    int left = n;

    if (data_max <= 0)
        return;                       /* rejected at config time */

    nrecs = blkc_recs_for_line(n, data_max);

    for (i = 0; i < nrecs; i++) {
        int chunk  = (left > data_max) ? data_max : left;
        int reclen;

        if (chunk < 0)
            chunk = 0;
        reclen = 4 + chunk;
        left  -= chunk;

        if (bi->size_last_partial_block == 0)
            bi->size_last_partial_block = 4;              /* BDW */

        if (bi->size_last_partial_block + reclen > bi->dcb_blksize) {
            bi->count_full_blocks++;
            bi->size_last_partial_block = 4 + reclen;     /* BDW + record */
        } else {
            bi->size_last_partial_block += reclen;
        }
    }
}

static void blkc_emit_line(blkcalc_info_t *bi, int n)
{
    if (bi->dcb_recfm == RECFM_V)
        blkc_emit_line_v(bi, n);
    else
        blkc_emit_line_f(bi, n);
}

void blkcalc_info_init(blkcalc_info_t *bi, blkcalc_recfm_t recfm,
                       int block_size, int logical_rec_len)
{
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(*bi));
    bi->dcb_recfm   = recfm;
    bi->dcb_blksize = block_size;
    bi->dcb_lrecl   = logical_rec_len;
}

int blkcalc_add_blocks_for_data(blkcalc_info_t *bi,
                                const char *buff, int buff_len)
{
    int run;
    int i;

    if (bi == NULL || (buff == NULL && buff_len > 0))
        return -1;

    /* Pick up a line left unterminated by the previous call, so a line
       split across two writes is counted once from its FULL length --
       which is what decides how many records it wraps into. */
    run = bi->last_unterm_text_line_chars;

    for (i = 0; i < buff_len; i++) {
        if (buff[i] == 0x0A) {        /* ASCII LF: the stream is ASCII here */
            blkc_emit_line(bi, run);
            run = 0;
        } else {
            run++;
        }
    }

    /* Carried, NOT emitted: more data may still arrive for this line. */
    bi->last_unterm_text_line_chars = run;
    return 0;
}

int blkcalc_total_blocks(const blkcalc_info_t *bi)
{
    blkcalc_info_t tmp;

    if (bi == NULL)
        return 0;

    /* A trailing run with no newline IS written as a record, so count it --
       on a copy, because asking the question must not change the answer. */
    tmp = *bi;
    if (tmp.last_unterm_text_line_chars > 0) {
        blkc_emit_line(&tmp, tmp.last_unterm_text_line_chars);
        tmp.last_unterm_text_line_chars = 0;
    }

    return tmp.count_full_blocks + (tmp.size_last_partial_block > 0 ? 1 : 0);
}


/* ==================================================================== */
/* Fitting                                                               */
/* ==================================================================== */

/*
 * Blocks that will still fit on track 'tt', given 'r' records already on it.
 *
 * The two cases are NOT the same question, and treating them alike was the
 * original mistake here:
 *
 *   - 'tt' is the track DS1LSTAR names, i.e. the REAL end of the dataset.
 *     A TTR's R is a physical BLOCK number on the track, and those blocks
 *     are whatever was actually written: short member-tail blocks, 256 byte
 *     directory blocks, the EOF marker.  It is NOT a count of full sized
 *     blocks, so "blocks_per_track - r" means nothing -- a live dataset was
 *     observed at lstar=15.14 with only 6 full blocks to a track.  DS1TRBAL,
 *     the bytes still free on that track, is the only sound measure.
 *
 *   - 'tt' is a track this module PREDICTED, while chaining one pending
 *     member onto the end of another.  Every block we count is assumed full
 *     sized, so there r really is in units of blocks and the subtraction is
 *     exact.  DS1TRBAL does not describe that track at all.
 */
static uint32_t blkc_track_room(const dataset_dscb_info_t *ds,
                                uint32_t tt, uint32_t r)
{
    uint32_t bpt = (uint32_t)ds->blocks_per_track;
    uint32_t room;

    if (bpt == 0)
        return 0;

    if (tt == ds->lstar_tt) {
        uint32_t slot = ds->trklen / bpt;    /* bytes one full block costs */

        room = (slot > 0) ? ((uint32_t)ds->trbal / slot) : 0;
        if (room > bpt)
            room = bpt;                      /* cannot beat an empty track */

        /* Blocks we have already predicted onto this track since the VTOC
           was read.  DS1TRBAL knows nothing about them, so without this a
           chained member would be credited the same free space twice. */
        if (r > (uint32_t)ds->lstar_r) {
            uint32_t used = r - (uint32_t)ds->lstar_r;
            room = (used < room) ? (room - used) : 0;
        }
    } else {
        room = (r < bpt) ? (bpt - r) : 0;
    }

    /* R is one byte of a TTR, so a track can never carry us past 255. */
    if (r + room > 255u)
        room = (r < 255u) ? (255u - r) : 0;

    return room;
}

/*
 * Blocks that will still fit, from 'last_block_ttr' to the end of everything
 * the dataset could ever hold: the rest of the current track, the allocated
 * tracks after it, and the tracks it could still extend by (the secondary
 * quantity times the extents it has not used -- zero if there is no
 * secondary, or it is already at the 16 extent limit).
 */
static uint32_t blkc_blocks_available(const dataset_dscb_info_t *ds,
                                      uint32_t last_block_ttr)
{
    uint32_t bpt   = (uint32_t)ds->blocks_per_track;
    uint32_t tt    = BLKC_TTR_TT(last_block_ttr);
    uint32_t r     = BLKC_TTR_R(last_block_ttr);
    uint32_t avail;

    if (bpt == 0)
        return 0;

    avail = blkc_track_room(ds, tt, r);

    /* Whole tracks after this one that are already allocated. */
    if (ds->tracks > tt + 1)
        avail += (ds->tracks - tt - 1) * bpt;

    /* Tracks it could still extend by. */
    if (ds->sec_tracks > 0 && ds->nextents < BLKC_MAX_EXTENTS) {
        uint32_t more = (uint32_t)(BLKC_MAX_EXTENTS - ds->nextents);
        avail += ds->sec_tracks * more * bpt;
    }

    return avail;
}

/*
 * Advance a TTR by 'nblocks' full sized blocks.
 *
 * Fills the current track first (blkc_track_room decides how much of it is
 * really usable), then whole tracks.  The result is where the LAST block
 * lands, so the record number on the final track is 1..blocks_per_track.
 */
static uint32_t blkc_ttr_add(const dataset_dscb_info_t *ds,
                             uint32_t last_block_ttr, uint32_t nblocks)
{
    uint32_t bpt  = (uint32_t)ds->blocks_per_track;
    uint32_t tt   = BLKC_TTR_TT(last_block_ttr);
    uint32_t r    = BLKC_TTR_R(last_block_ttr);
    uint32_t room;
    uint32_t trks;

    if (bpt == 0 || nblocks == 0)
        return last_block_ttr;

    room = blkc_track_room(ds, tt, r);
    if (nblocks <= room)
        return BLKC_TTR(tt, r + nblocks);

    nblocks -= room;
    trks     = (nblocks + bpt - 1) / bpt;
    return BLKC_TTR(tt + trks, nblocks - (trks - 1) * bpt);
}

int blkcalc_will_member_fit(const blkcalc_info_t *bi,
                            const dataset_dscb_info_t *ds,
                            uint32_t last_block_ttr,
                            uint32_t *predicted_last_block_ttr)
{
    uint32_t need;
    uint32_t avail;

    if (bi == NULL || ds == NULL)
        return -1;
    if (!ds->valid || ds->blocks_per_track <= 0)
        return -1;                    /* cannot tell -- treat as "no" */

    need  = (uint32_t)blkcalc_total_blocks(bi);

    /* An EMPTY member is not free.  Stowing one still writes an end-of-file
       marker and takes a directory entry, so a dataset with no room cannot
       accept it either -- and a zero here would answer "fits" for every
       member on a completely full PDS.  Charge one block minimum. */
    if (need == 0)
        need = 1;

    avail = blkc_blocks_available(ds, last_block_ttr);

    /* Where this member would end, whether or not it fits: the caller
       chains this into the next pending member's starting point. */
    if (predicted_last_block_ttr != NULL)
        *predicted_last_block_ttr =
            blkc_ttr_add(ds, last_block_ttr, need);

    return (need <= avail) ? 0 : -1;
}


/* ==================================================================== */
/* Config time                                                           */
/* ==================================================================== */

/* Turn a DSCB 3 byte date (2 digit year + binary day) into yyyyddd. */
static uint32_t blkc_date(const uint8_t *d)
{
    if (d[0] == 0 && d[1] == 0 && d[2] == 0)
        return 0;
    return (uint32_t)MVS_DSCB_YEAR(d) * 1000u + (uint32_t)MVS_DSCB_DAY(d);
}

int blkcalc_dataset_init(dataset_dscb_info_t *out, const mvs_dscb_info_t *raw)
{
    int i;

    if (out == NULL || raw == NULL)
        return -1;

    memset(out, 0, sizeof(*out));

    if (raw->status != MVS_DSCB_ST_OK)
        return -1;

    out->dsorg    = raw->dsorg[0];        /* high byte carries PO / PS */
    out->recfm    = raw->recfm;
    out->blksize  = MVS_DSCB_U16(raw->blksize);
    out->lrecl    = MVS_DSCB_U16(raw->lrecl);
    out->nextents = raw->nextents;
    out->tracks   = raw->tracks;
    out->devcode  = raw->devtype[3];
    out->trklen   = MVS_DSCB_U16(raw->trklen);
    out->trkcyl   = MVS_DSCB_U16(raw->trkcyl);

    for (i = 0; i < 6; i++)
        out->volser[i] = raw->volser[i];
    out->volser[6] = '\0';
    while (i > 0 && out->volser[i - 1] == ' ')
        out->volser[--i] = '\0';

    out->sec_flags   = raw->sec_flags;
    out->sec_qty     = raw->sec_qty;
    out->sec_tracks  = raw->sec_tracks;

    /* A secondary given as an average block length is the one case the
       assembler cannot convert, because the answer needs the track
       capacity.  Finish it here now that we have that. */
    if (out->sec_tracks == 0 && out->sec_qty > 0 &&
        MVS_DSCB_SEC_UNITS(out->sec_flags) == MVS_DSCB_SEC_BLK) {
        int n = mvs_blocks_per_track(out->devcode, out->trklen, out->blksize);
        if (n > 0)
            out->sec_tracks = (out->sec_qty + (uint32_t)n - 1) / (uint32_t)n;
    }

    out->create_date = blkc_date(raw->create_date);
    out->ref_date    = blkc_date(raw->ref_date);

    out->lstar_tt = MVS_DSCB_U16(raw->lstar);
    out->lstar_r  = raw->lstar[2];
    out->trbal    = MVS_DSCB_U16(raw->trbal);

    out->blocks_per_track =
        mvs_blocks_per_track(out->devcode, out->trklen, out->blksize);

    /* Only claim validity when every input the prediction relies on is
       actually usable.  A zero here would otherwise become a silent
       "everything fits". */
    if (out->blocks_per_track > 0 && out->blksize > 0 &&
        out->lrecl > 0 && out->tracks > 0)
        out->valid = 1;

    return out->valid ? 0 : -1;
}


/* ==================================================================== */
/* The write path entry point                                            */
/* ==================================================================== */

/* Map the DCB record format byte onto what the block arithmetic needs. */
static blkcalc_recfm_t blkc_recfm_of(uint8_t recfm)
{
    switch (recfm & 0xC0) {
    case 0x80: return RECFM_F;
    case 0x40: return RECFM_V;
    default:   return RECFM_U;
    }
}

/* Re-read just the two fields that move.  Everything else in ds_info was
   settled at config time and does not change under us. */
static int blkc_vtoc_read(pds_dataset_t *ds)
{
    const char      *dsnlist[2];
    mvs_dscb_info_t  raw;
    int              rc;

    dsnlist[0] = ds->dsname_ebcdic;
    dsnlist[1] = NULL;

    memset(&raw, 0, sizeof(raw));
    rc = mvs_dscb(MVS_DSCB_REQ_INFO, 0, dsnlist, &raw);
    if (rc > 4 || raw.status != MVS_DSCB_ST_OK) {
        log_error("blkcalc: VTOC re-read failed for %s (rc=%d status=%d)",
                  ds->dsname_ebcdic, rc, (int)raw.status);
        return -1;
    }

    ds->dscb.lstar_tt = MVS_DSCB_U16(raw.lstar);
    ds->dscb.lstar_r  = raw.lstar[2];
    ds->dscb.trbal    = MVS_DSCB_U16(raw.trbal);

    /* The allocation itself can grow when another task extends the
       dataset, so take these too -- they are cheap and already read. */
    ds->dscb.tracks   = raw.tracks;
    ds->dscb.nextents = raw.nextents;
    return 0;
}

/* Fold a stored byte range of the member into 'bi'.  Used only when the
   estimate has to be rebuilt; reads through pww_read_range so it works the
   same whether the member is in memory or spilled to disk. */
static int blkc_scan_stored(pending_member_t *pm, blkcalc_info_t *bi,
                            uint32_t from, uint32_t to)
{
    uint32_t off;

    for (off = from; off < to; ) {
        uint32_t n = to - off;

        if (n > (uint32_t)sizeof(g_blkc_scan))
            n = (uint32_t)sizeof(g_blkc_scan);
        if (pww_read_range(pm, off, g_blkc_scan, n) != 0)
            return -1;
        blkcalc_add_blocks_for_data(bi, (const char *)g_blkc_scan, (int)n);
        off += n;
    }
    return 0;
}

/* Fold 'n' zero bytes into 'bi' -- the hole pww_store_range would create
   when a write lands beyond the current end of the member. */
static void blkc_scan_gap(blkcalc_info_t *bi, uint32_t n)
{
    uint32_t left = n;

    memset(g_blkc_scan, 0, sizeof(g_blkc_scan));
    while (left > 0) {
        uint32_t c = left;

        if (c > (uint32_t)sizeof(g_blkc_scan))
            c = (uint32_t)sizeof(g_blkc_scan);
        blkcalc_add_blocks_for_data(bi, (const char *)g_blkc_scan, (int)c);
        left -= c;
    }
}

/*
 * Build the estimate for what the member will contain AFTER this write.
 *
 * The cheap case is a strict sequential append, which is the overwhelmingly
 * common NFS pattern: the running estimate is already correct up to the
 * write's offset, so the new bytes just fold in.
 *
 * Anything else -- an overwrite, an out of order write into a slot CREATE
 * made, a write after a truncate -- would double count or miss content, so
 * the estimate is rebuilt from the member itself in three ordered pieces:
 * what precedes the write, the write, and whatever follows it.  Block
 * accumulation is sequential and additive, so feeding those in order gives
 * exactly the post-write content.  See design Sec 7.1.
 */
static int blkc_build_trial(pending_member_t *pm, blkcalc_info_t *trial,
                            const uint8_t *data, uint32_t count,
                            uint64_t offset)
{
    uint32_t off32 = (uint32_t)offset;
    uint32_t end;

    if (off32 == pm->high_water && pm->blkcalc.consumed_upto == pm->high_water) {
        *trial = pm->blkcalc;
        blkcalc_add_blocks_for_data(trial, (const char *)data, (int)count);
        trial->consumed_upto = off32 + count;
        return 0;
    }

    log_debug("blkcalc: rebuilding estimate for %s(%s) -- write at %u is not"
              " a sequential append (hw=%u consumed=%u)",
              pm->dsname_ebcdic, pm->member_name, off32,
              pm->high_water, pm->blkcalc.consumed_upto);

    blkcalc_info_init(trial, pm->blkcalc.dcb_recfm,
                      pm->blkcalc.dcb_blksize, pm->blkcalc.dcb_lrecl);

    if (off32 <= pm->high_water) {
        if (blkc_scan_stored(pm, trial, 0, off32) != 0)
            return -1;
    } else {
        if (blkc_scan_stored(pm, trial, 0, pm->high_water) != 0)
            return -1;
        blkc_scan_gap(trial, off32 - pm->high_water);
    }

    blkcalc_add_blocks_for_data(trial, (const char *)data, (int)count);

    end = off32 + count;
    if (end < pm->high_water) {
        if (blkc_scan_stored(pm, trial, end, pm->high_water) != 0)
            return -1;
        end = pm->high_water;
    }

    trial->consumed_upto = end;
    return 0;
}

int blkcalc_admit_write(void *pmv, const uint8_t *data,
                        uint32_t count, uint64_t offset)
{
    pending_member_t *pm = (pending_member_t *)pmv;
    pds_dataset_t    *ds;
    blkcalc_info_t    trial;
    uint32_t          ttr;
    int               i;

    if (pm == NULL)
        return 0;

    ds = export_dataset_get(pm->export_idx, pm->dataset_idx);
    if (ds == NULL || !ds->dscb.valid)
        return 0;         /* no usable geometry: fall back to the backstop */

    /* Read every time.  DS1LSTAR / DS1TRBAL move whenever ANY member of
       this dataset is written, including by another of our own pending
       slots being flushed mid-sequence, so a value cached for the life of
       a write sequence would be wrong exactly when it matters
       (design Sec 7.4). */
    if (blkc_vtoc_read(ds) != 0) {
        errno = EIO;
        return -1;
    }

    if (blkc_build_trial(pm, &trial, data, count, offset) != 0) {
        errno = EIO;
        return -1;
    }

    /* This member first, from the real end of the dataset ... */
    ttr = BLKC_TTR(ds->dscb.lstar_tt, ds->dscb.lstar_r);
    if (blkcalc_will_member_fit(&trial, &ds->dscb, ttr, &ttr) != 0) {
        log_warn("blkcalc: %s(%s) predicted NOT to fit -- %d blocks needed + EOF block",
                 pm->dsname_ebcdic, pm->member_name,
                 blkcalc_total_blocks(&trial));
        errno = ENOSPC;
        return -1;
    }

    /* ... then every OTHER member pending for the same dataset, each
       starting where the previous one was predicted to end.  They will all
       be stowed eventually, so they all have to fit together. */
    for (i = 0; i < PWW_MAX_PENDING; i++) {
        pending_member_t *other = pww_slot_at(i);

        if (other == NULL || other == pm)
            continue;
        if (strcmp(other->dsname_ebcdic, pm->dsname_ebcdic) != 0)
            continue;

        if (blkcalc_will_member_fit(&other->blkcalc, &ds->dscb,
                                    ttr, &ttr) != 0) {
            log_warn("blkcalc: %s(%s) refused -- pending %s would not also"
                     " fit", pm->dsname_ebcdic, pm->member_name,
                     other->member_name);
            errno = ENOSPC;
            return -1;
        }
    }

    pm->blkcalc = trial;      /* committed only now that it is admitted */
    return 0;
}

/* Set up a slot's estimate from its dataset's record format.  Called when a
   slot is created and again when a CREATE truncates one that already
   existed -- a re-create must not inherit the old member's blocks. */
void blkcalc_slot_reset(void *pmv)
{
    pending_member_t *pm = (pending_member_t *)pmv;
    pds_dataset_t    *ds;

    if (pm == NULL)
        return;

    ds = export_dataset_get(pm->export_idx, pm->dataset_idx);
    if (ds == NULL) {
        memset(&pm->blkcalc, 0, sizeof(pm->blkcalc));
        return;
    }

    blkcalc_info_init(&pm->blkcalc,
        blkc_recfm_of(ds->dscb.recfm),
        (int)ds->dscb.blksize,
        (int)ds->dscb.lrecl);
}
