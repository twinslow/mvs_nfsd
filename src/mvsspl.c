/*
 * mvsspl.c - Write-spill store.  See mvsspl.h and doc/design_nfs_write.md Sec 8.
 *
 * A pending member whose byte stream outgrows the in-memory threshold is moved
 * to a temporary PS dataset (DSORG=PS RECFM=FB LRECL=4096 BLKSIZE=4096) opened
 * binary "w+b": one handle creates/truncates the scratch and then serves the
 * write phase and the flush read-back.  One scratch per pool slot (&&PWWSP<nn>),
 * reused across successive members.
 *
 * Writes are done as full-block read/modify/write (spill_load_block /
 * spill_store_block): JCC does NOT keep a partial-block modification across a
 * seek to another block, so a byte written mid-block and read back after seeking
 * away returns the OLD data.  Reducing every write to complete 4096-byte blocks
 * at block-aligned offsets -- the pattern proven durable by t#seqrw2 -- avoids
 * that entirely.  Reads (fseek + fread) are byte-granular.
 *
 * Reading back is the sharp edge on MVS: the readable EOF of an open, still-
 * extending temp dataset does NOT advance past ~one track (12 * 4096 = 49152
 * bytes) until the dataset is CLOSED -- a block written beyond that reads back
 * as 0 even after fflush.  So before a read that could reach committed-but-
 * beyond-EOF data, spill_sync() closes and reopens the scratch "r+b" (no
 * truncate) to commit the EOF.  It runs lazily (only when writes are pending),
 * so a run of consecutive reads -- the flush read pass -- pays one reopen, and
 * the sequential-append RMW of just the tail block (buffer-resident) pays none.
 * On the dev host tmpfile() is anonymous (cannot be reopened) and reads see
 * writes after fflush, so there spill_sync() is just an fflush.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "types.h"
#include "mvspww.h"
#include "mvsspl.h"
#include "logger.h"

#define SPILL_BLK 4096

/* A 4096 read/modify/write scratch.  spill_write only ever writes COMPLETE
   blocks at block-aligned offsets, because on JCC a partial-block fwrite does
   not survive a seek to another block (a byte written mid-block, then a seek
   away and back, reads the old data): proven full-block random write/read works
   (t#seqrw2), so we reduce every write to that. */
static uint8_t g_spill_block[SPILL_BLK];

int spill_open(pending_member_t *pm, int slot_id)
{
    if (pm->spill_fp != NULL)      /* already spilled -- should not happen */
        return 0;

#ifdef __MVS__
    {
        char name[24];

        sprintf(name, "//DSN:&&PWWSP%02d", slot_id);
        pm->spill_fp = fopen(name,
            "w+b," PWW_SPILL_DS_SPACE_PARMS ","
            "dsorg=ps,recfm=fb,blksize=4096,lrecl=4096");
        if (pm->spill_fp == NULL) {
            log_error("spill_open: fopen %s failed: %s",
                      name, strerror(errno));
            errno = EIO;
            return -1;
        }
        log_info("spill_open: %s(%s) spilled to %s",
            pm->dsname_ebcdic, pm->member_name, name);
    }
#else
    (void)slot_id;
    pm->spill_fp = tmpfile();      /* dev/test host: anonymous binary temp */
    if (pm->spill_fp == NULL) {
        log_error("spill_open: tmpfile() failed: %s", strerror(errno));
        errno = EIO;
        return -1;
    }
#endif

    pm->spill_size  = 0;
    pm->spill_slot  = slot_id;
    pm->spill_dirty = 0;
    return 0;
}

/* Commit pending writes so the whole dataset is readable.
 *
 * On MVS the readable end-of-file of an open, still-extending temp dataset does
 * NOT advance past ~one track until the dataset is closed -- a block written
 * beyond that and then read back returns 0, even after fflush.  So we close and
 * reopen it "r+b" (no truncate) to commit the EOF.  Called lazily: only the
 * first read after a run of writes pays for it.  On the dev host a tmpfile()
 * is anonymous (cannot be reopened) and reads see writes after fflush, so there
 * a plain fflush suffices. */
static int spill_sync(pending_member_t *pm)
{
    if (!pm->spill_dirty)
        return 0;

#ifdef __MVS__
    {
        char name[24];

        fflush(pm->spill_fp);
        if (fclose(pm->spill_fp) != 0) {
            log_error("spill_sync: fclose failed: %s", strerror(errno));
            pm->spill_fp = NULL;
            errno = EIO;
            return -1;
        }
        sprintf(name, "//DSN:&&PWWSP%02d", pm->spill_slot);
        pm->spill_fp = fopen(name, "r+b");    /* committed; no truncate */
        if (pm->spill_fp == NULL) {
            log_error("spill_sync: reopen %s failed: %s", name, strerror(errno));
            errno = EIO;
            return -1;
        }
    }
#else
    fflush(pm->spill_fp);
#endif

    pm->spill_dirty = 0;
    return 0;
}

/* Load physical block 'b' into g_spill_block; a block at or beyond the current
   extent reads as zeros (it does not exist yet).  We only extend by whole
   zero-initialised blocks, so an in-extent block is always a full 4096. */
static int spill_load_block(pending_member_t *pm, uint32_t b)
{
    uint32_t phys = (pm->spill_size + SPILL_BLK - 1) / SPILL_BLK;
    size_t   r;

    if (b >= phys) {
        memset(g_spill_block, 0, SPILL_BLK);
        return 0;
    }
    /* fflush commits the buffer (and does the C-required write->read switch on
       the "+" stream).  Reaching an EARLIER, already-committed block needs more
       than that -- a reopen -- but spill_write does that once up front when the
       write reaches below the highest block; the highest (tail) block read here
       during a sequential append is buffer-resident and reads fine. */
    fflush(pm->spill_fp);
    if (fseek(pm->spill_fp, (long)b * SPILL_BLK, SEEK_SET) != 0) {
        log_error("spill: fseek(load) block %u failed: %s", b, strerror(errno));
        errno = EIO;
        return -1;
    }
    r = fread(g_spill_block, 1, SPILL_BLK, pm->spill_fp);
    if (r < SPILL_BLK)                       /* short read -- should not happen */
        memset(g_spill_block + r, 0, SPILL_BLK - (size_t)r);
    return 0;
}

/* Write g_spill_block back as complete block 'b'.  Blocks are always written in
   increasing order up to the new extent, so this fseek is never past EOF. */
static int spill_store_block(pending_member_t *pm, uint32_t b)
{
    size_t w;

    if (fseek(pm->spill_fp, (long)b * SPILL_BLK, SEEK_SET) != 0) {
        log_error("spill: fseek(store) block %u failed: %s", b, strerror(errno));
        errno = EIO;
        return -1;
    }
    w = fwrite(g_spill_block, 1, SPILL_BLK, pm->spill_fp);
    if (w != SPILL_BLK) {
        log_error("spill: store block %u short write (%u of %d)",
                  b, (unsigned)w, SPILL_BLK);
        errno = EIO;
        return -1;
    }
    pm->spill_dirty = 1;    /* a read must commit (reopen) before it can see this */
    return 0;
}

int spill_write(pending_member_t *pm, uint32_t off,
                const uint8_t *data, uint32_t len)
{
    uint32_t end      = off + len;
    uint32_t new_size = pm->spill_size;
    uint32_t cur_blocks;
    uint32_t need_blocks;
    uint32_t first;
    uint32_t last;
    uint32_t b;

    if (len > 0 && end > new_size)
        new_size = end;
    else if (len == 0 && off > new_size)
        new_size = off;
    if (new_size == pm->spill_size && len == 0)
        return 0;                            /* nothing to extend or write */

    cur_blocks  = (pm->spill_size + SPILL_BLK - 1) / SPILL_BLK;
    need_blocks = (new_size      + SPILL_BLK - 1) / SPILL_BLK;

    /* First block to (re)write: the data's first block if it reaches into
       existing content, otherwise the current extent (any gap ahead is filled
       with zero blocks so the file stays hole-free).  Last block: up to the new
       extent when extending, else just the data's last block. */
    if (len > 0 && off / SPILL_BLK < cur_blocks)
        first = off / SPILL_BLK;
    else
        first = cur_blocks;

    if (new_size > pm->spill_size)
        last = need_blocks - 1;
    else
        last = (end - 1) / SPILL_BLK;        /* pure overwrite (len > 0) */

    /* If this write reads back an EARLIER, already-committed block (an overwrite
       reaching below the highest block), commit pending writes once first so
       those loads see real data -- a written-but-open block past ~one track is
       not readable until the dataset is reopened (spill_sync).  A plain
       sequential append (first == the current tail block) reads only the
       buffer-resident tail and needs no reopen. */
    if (first + 1 < cur_blocks) {
        if (spill_sync(pm) != 0)
            return -1;
    }

    /* Full-block read/modify/write of each block in [first, last].  Bytes not
       covered by the data stay as loaded -- zero for a new/never-written region
       (so holes read back as zeros), or the existing content for an overwrite. */
    for (b = first; b <= last; b++) {
        uint32_t blk_lo = b * SPILL_BLK;

        if (spill_load_block(pm, b) != 0)
            return -1;
        if (len > 0) {
            uint32_t lo = (off > blk_lo)              ? off : blk_lo;
            uint32_t hi = (end < blk_lo + SPILL_BLK)  ? end : blk_lo + SPILL_BLK;
            if (lo < hi)
                memcpy(g_spill_block + (lo - blk_lo),
                       data + (lo - off), (size_t)(hi - lo));
        }
        if (spill_store_block(pm, b) != 0)
            return -1;
    }

    pm->spill_size = new_size;
    return 0;
}

int spill_read(pending_member_t *pm, uint32_t off,
               uint8_t *dst, uint32_t len)
{
    size_t r;

    if (len == 0)
        return 0;

    /* Commit any pending writes (reopen on MVS) so the read-back sees the whole
       dataset, not just the ~one track the open EOF exposes.  No-op if nothing
       is pending, so a run of consecutive reads pays the reopen only once. */
    if (spill_sync(pm) != 0)
        return -1;
    if (fseek(pm->spill_fp, (long)off, SEEK_SET) != 0) {
        log_error("spill_read: fseek to %u failed: %s",
                  off, strerror(errno));
        errno = EIO;
        return -1;
    }
    r = fread(dst, 1, (size_t)len, pm->spill_fp);
    if (r != (size_t)len) {
        log_error("spill_read: short read at %u (%u of %u)",
                  off, (unsigned)r, len);
        errno = EIO;
        return -1;
    }
    return 0;
}

void spill_close(pending_member_t *pm)
{
    if (pm->spill_fp != NULL) {
        fclose(pm->spill_fp);        /* on the dev host this deletes tmpfile */
        pm->spill_fp = NULL;
    }
    pm->spill_size  = 0;
    pm->spill_dirty = 0;
}
