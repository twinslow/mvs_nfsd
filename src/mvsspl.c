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

int has_spill_file_open(const pending_member_t *pm)
{
    return pm->spill.fp != NULL;
}

/* Open the slot's scratch dataset.  Private: the only way a member becomes
   spilled is spill_transition(), which owns the rollback on failure. */
static int spill_open(pending_member_t *pm, int slot_id)
{
    if (pm->spill.fp != NULL)      /* already spilled -- should not happen */
        return 0;

    {
        char name[24];

        sprintf(name, "//DSN:&&PWWSP%02d", slot_id);
        pm->spill.fp = fopen(name,
            "w+b," SPILL_DS_SPACE_PARMS
            ",dsorg=ps,recfm=fb,blksize=4096,lrecl=4096");
        if (pm->spill.fp == NULL) {
            logmsg_error("NFSIS010E", "spill_open: fopen %s failed: %s",
                      name, strerror(errno));
            errno = EIO;
            return -1;
        }
        logmsg_info("NFSIS020I", "spill_open: %s(%s) spilled to %s",
            pm->dsname_ebcdic, pm->member_name, name);
    }

    pm->spill.size  = 0;
    pm->spill.slot  = slot_id;
    pm->spill.dirty = 0;
    return 0;
}

/* Commit pending writes so the whole dataset is readable.
 *
 * On MVS the readable end-of-file of an open, still-extending temp dataset does
 * NOT advance past ~one track until the dataset is closed -- a block written
 * beyond that and then read back returns 0, even after fflush.  So we close and
 * reopen it "r+b" (no truncate) to commit the EOF.  Called lazily: only the
 * first read after a run of writes pays for it. */
static int spill_sync(pending_member_t *pm)
{
    if (!pm->spill.dirty)
        return 0;

    {
        char name[24];

        fflush(pm->spill.fp);
        if (fclose(pm->spill.fp) != 0) {
            logmsg_error("NFSIS030E", "spill_sync: fclose failed: %s", strerror(errno));
            pm->spill.fp = NULL;
            errno = EIO;
            return -1;
        }
        sprintf(name, "//DSN:&&PWWSP%02d", pm->spill.slot);
        pm->spill.fp = fopen(name, "r+b");    /* committed; no truncate */
        if (pm->spill.fp == NULL) {
            logmsg_error("NFSIS040E", "spill_sync: reopen %s failed: %s", name, strerror(errno));
            errno = EIO;
            return -1;
        }
    }

    pm->spill.dirty = 0;
    return 0;
}

/* Load physical block 'b' into g_spill_block; a block at or beyond the current
   extent reads as zeros (it does not exist yet).  We only extend by whole
   zero-initialised blocks, so an in-extent block is always a full 4096. */
static int spill_load_block(pending_member_t *pm, uint32_t b)
{
    uint32_t phys = (pm->spill.size + SPILL_BLK - 1) / SPILL_BLK;
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
    fflush(pm->spill.fp);
    if (fseek(pm->spill.fp, (long)b * SPILL_BLK, SEEK_SET) != 0) {
        logmsg_error("NFSIS050E", "spill: fseek(load) block %u failed: %s", b, strerror(errno));
        errno = EIO;
        return -1;
    }
    r = fread(g_spill_block, 1, SPILL_BLK, pm->spill.fp);
    if (r < SPILL_BLK)                       /* short read -- should not happen */
        memset(g_spill_block + r, 0, SPILL_BLK - (size_t)r);
    return 0;
}

/* Write g_spill_block back as complete block 'b'.  Blocks are always written in
   increasing order up to the new extent, so this fseek is never past EOF. */
static int spill_store_block(pending_member_t *pm, uint32_t b)
{
    size_t w;

    if (fseek(pm->spill.fp, (long)b * SPILL_BLK, SEEK_SET) != 0) {
        logmsg_error("NFSIS060E", "spill: fseek(store) block %u failed: %s", b, strerror(errno));
        errno = EIO;
        return -1;
    }
    w = fwrite(g_spill_block, 1, SPILL_BLK, pm->spill.fp);
    if (w != SPILL_BLK) {
        logmsg_error("NFSIS070E", "spill: store block %u short write (%u of %d)",
                  b, (unsigned)w, SPILL_BLK);
        errno = EIO;
        return -1;
    }
    pm->spill.dirty = 1;    /* a read must commit (reopen) before it can see this */
    return 0;
}

int spill_write(pending_member_t *pm, uint32_t off,
                const uint8_t *data, uint32_t len)
{
    uint32_t end      = off + len;
    uint32_t new_size = pm->spill.size;
    uint32_t cur_blocks;
    uint32_t need_blocks;
    uint32_t first;
    uint32_t last;
    uint32_t b;

    if (len > 0 && end > new_size)
        new_size = end;
    else if (len == 0 && off > new_size)
        new_size = off;
    if (new_size == pm->spill.size && len == 0)
        return 0;                            /* nothing to extend or write */

    cur_blocks  = (pm->spill.size + SPILL_BLK - 1) / SPILL_BLK;
    need_blocks = (new_size      + SPILL_BLK - 1) / SPILL_BLK;

    /* First block to (re)write: the data's first block if it reaches into
       existing content, otherwise the current extent (any gap ahead is filled
       with zero blocks so the file stays hole-free).  Last block: up to the new
       extent when extending, else just the data's last block. */
    if (len > 0 && off / SPILL_BLK < cur_blocks)
        first = off / SPILL_BLK;
    else
        first = cur_blocks;

    if (new_size > pm->spill.size)
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

    pm->spill.size = new_size;
    return 0;
}

int spill_truncate(pending_member_t *pm, uint32_t size)
{
    /* Grow: zero-extend to 'size' with an empty write (spill_write fills the
       hole ahead of the current extent). */
    if (size > pm->spill.size)
        return spill_write(pm, size, NULL, 0);

    /* Shrink: lowering the recorded extent is all that is needed.  The blocks
       stay on disk but nothing reads them -- every reader is bounded by the
       member's logical size -- and a later write back into the freed region
       goes through spill_write, which zero-fills any hole ahead of the
       current extent, so the stale bytes can never resurface. */
    pm->spill.size = size;
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
    if (fseek(pm->spill.fp, (long)off, SEEK_SET) != 0) {
        logmsg_error("NFSIS080E", "spill_read: fseek to %u failed: %s",
                  off, strerror(errno));
        errno = EIO;
        return -1;
    }
    r = fread(dst, 1, (size_t)len, pm->spill.fp);
    if (r != (size_t)len) {
        logmsg_error("NFSIS090E", "spill_read: short read at %u (%u of %u)",
                  off, (unsigned)r, len);
        errno = EIO;
        return -1;
    }
    return 0;
}

void spill_close(pending_member_t *pm)
{
    if (pm->spill.fp != NULL) {
        logmsg_debug("NFSIS100D", "spill_close: closing spill for %s(%s) ...",
                  pm->dsname_ebcdic, pm->member_name);
        fclose(pm->spill.fp);
        pm->spill.fp = NULL;
    }
    pm->spill.size  = 0;
    pm->spill.dirty = 0;
}

int spill_transition(pending_member_t *pm, int slot_id,
                     const uint8_t *data, uint32_t len)
{
    if (spill_open(pm, slot_id) != 0)
        return -1;                   /* nothing opened; caller keeps its buffer */

    if (len > 0 && data != NULL) {
        if (spill_write(pm, 0, data, len) != 0) {
            int saved = errno;
            spill_close(pm);         /* roll back: the member is in memory again */
            errno = saved;
            return -1;
        }
    }
    return 0;
}
