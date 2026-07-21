/*
 * mvsspl.h - Write-spill store: back a pending member's byte stream with a
 * temporary dataset once it grows past the in-memory threshold.
 *
 * See doc/design_nfs_write.md Sec 8 (Phase 2).  Used only by mvspww.c; the
 * "spill_" prefix keeps these clearly distinct from the member-write path
 * (pww_write / pww_write_member, which write the real PDS member).
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#ifndef MVSSPL_H_INCLUDED
#define MVSSPL_H_INCLUDED

#include "types.h"
#include "mvspww.h"     /* pending_member_t (spill_fp / spill_size) */

/* Open the scratch dataset for a slot that is spilling to disk.  slot_id names
 * the per-slot temp dataset (&&PWWSP<nn>) so it is reused, never accumulated.
 * On MVS this is a DSORG=PS RECFM=FB BLKSIZE=4096 temp dataset opened binary
 * "w+b"; on the dev/test host it is tmpfile().  Sets pm->spill_fp and
 * pm->spill_size = 0.  Returns 0, or -1 (errno = EIO). */
int  spill_open(pending_member_t *pm, int slot_id);

/* Write 'len' bytes of 'data' at byte offset 'off' in the scratch dataset.  A
 * gap ahead of 'off' (a hole) is materialised as zeros.  Implemented as a
 * full-block read/modify/write so JCC only ever sees complete block writes --
 * partial-block writes are not durable across a seek (see mvsspl.c).  'data' may
 * be NULL when len == 0 (a pure zero-extend up to 'off').  Updates
 * pm->spill_size.  Returns 0, or -1 (errno = EIO). */
int  spill_write(pending_member_t *pm, uint32_t off,
                 const uint8_t *data, uint32_t len);

/* Read 'len' bytes at byte offset 'off' from the scratch dataset into 'dst'.
 * off+len must lie within pm->spill_size.  Returns 0, or -1 (errno = EIO). */
int  spill_read(pending_member_t *pm, uint32_t off,
                uint8_t *dst, uint32_t len);

/* Close the scratch dataset (if open) and clear pm->spill_fp / spill_size.
 * The temp dataset itself is left for the slot to reuse (JCC deletes it at task
 * end); on the dev host tmpfile() is deleted by fclose.  Safe if not open. */
void spill_close(pending_member_t *pm);

#endif /* MVSSPL_H_INCLUDED */
