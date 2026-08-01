/*
 * mvsspl.h - Write-spill store: back a pending member's byte stream with a
 * temporary dataset once it grows past the in-memory threshold.
 *
 * See doc/design_nfs_write.md Sec 8 (Phase 2).  Used only by mvspww.c; the
 * "spill_" prefix keeps these clearly distinct from the member-write path
 * (pww_write / pww_write_member, which write the real PDS member).
 *
 * THIS IS THE WHOLE BOUNDARY.  Everything about a spilled member -- whether
 * it is spilled, where its bytes are, how big it is -- is reached through the
 * calls below.  pww_spill_t is declared in mvspww.h only because it is
 * embedded in pending_member_t; no module other than mvsspl.c reads or writes
 * its fields.  Keep it that way: a caller that tests pm->spill.fp itself has
 * re-opened the boundary this header exists to close.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#ifndef MVSSPL_H_INCLUDED
#define MVSSPL_H_INCLUDED

#include "types.h"
#include "mvspww.h"     /* pending_member_t, pww_spill_t (pm->spill) */

/*
 * MVS short-name aliases.  spill_truncate and spill_transition collide in the
 * first 8 characters, so the linker needs explicit distinct external names
 * (same reason as the pww_flush_* aliases in mvspww.h).
 */
#define spill_truncate     splTrunc
#define spill_transition   splTrans

/* This string will be concatenated into the file open mode for the spill */
/* temporary dataset at compile time                                      */
#define SPILL_DS_SPACE_PARMS   "pri=15,sec=15,rlse,unit=sysda"

/* Is this member's content currently backed by the scratch dataset rather
 * than by its in-memory buffer?  The single question the write pool needs to
 * ask about spill state -- everything that follows from the answer is one of
 * the calls below. */
int  has_spill_file_open(const pending_member_t *pm);

/* Move a member's content from memory to disk: open the slot's scratch
 * dataset and copy 'len' bytes of 'data' into it at offset 0.  slot_id names
 * the per-slot temp dataset (&&PWWSP<nn>) so it is reused, never accumulated;
 * the dataset is DSORG=PS RECFM=FB BLKSIZE=4096 opened binary "w+b".
 *
 * On ANY failure the scratch is closed again and -1 returned (errno set), so
 * the caller's buffer is still the member's content and the slot stays
 * coherent.  The caller owns that buffer and must free it only after this
 * returns 0.  'data' may be NULL when len == 0 (an empty member). */
int  spill_transition(pending_member_t *pm, int slot_id,
                      const uint8_t *data, uint32_t len);

/* Write 'len' bytes of 'data' at byte offset 'off' in the scratch dataset.  A
 * gap ahead of 'off' (a hole) is materialised as zeros.  Implemented as a
 * full-block read/modify/write so JCC only ever sees complete block writes --
 * partial-block writes are not durable across a seek (see mvsspl.c).  'data' may
 * be NULL when len == 0 (a pure zero-extend up to 'off').  Returns 0, or -1
 * (errno = EIO). */
int  spill_write(pending_member_t *pm, uint32_t off,
                 const uint8_t *data, uint32_t len);

/* Resize the spilled content to exactly 'size' bytes.  Growing zero-extends
 * the scratch; shrinking lowers the recorded extent only -- the blocks are
 * not reclaimed, which is safe because nothing reads past the member's
 * logical size and a later write back into that region goes through
 * spill_write, which zero-fills any hole ahead of the current extent.
 * Returns 0, or -1 (errno = EIO). */
int  spill_truncate(pending_member_t *pm, uint32_t size);

/* Read 'len' bytes at byte offset 'off' from the scratch dataset into 'dst'.
 * off+len must lie within the member's spilled size.  Returns 0, or -1
 * (errno = EIO). */
int  spill_read(pending_member_t *pm, uint32_t off,
                uint8_t *dst, uint32_t len);

/* Close the scratch dataset (if open) and forget the spilled content.  The
 * temp dataset itself is left for the slot to reuse (JCC deletes it at task
 * end).  Safe if not open. */
void spill_close(pending_member_t *pm);

#endif /* MVSSPL_H_INCLUDED */
