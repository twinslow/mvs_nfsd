/*
 * mvspwfl.h - Flush machinery for the pending-member write pool.
 *
 * mvspww.c reassembles a member in memory (or in a spill dataset); this
 * module is what puts it in the PDS.  See mvspwfl.c for why the two are
 * separate: everything in there can abend, and the STAE guards belong with
 * the code they guard.
 *
 * The interface is two functions.  Everything else -- the member write, the
 * ISPF-stats STOW, both STAE regions, the abend classification and the
 * out-of-space table -- is private to mvspwfl.c.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#ifndef MVSPWFL_H_INCLUDED
#define MVSPWFL_H_INCLUDED

#include "types.h"
#include "mvspww.h"     /* pending_member_t */

/*
 * MVS short-name aliases -- REQUIRED, not optional, for every function this
 * header publishes.
 *
 * The linker distinguishes external names by their first 8 characters, and
 * "pdsflush" is exactly 8, so every pdsflush_* external collides with every
 * other one.  That is the price of a prefix this descriptive; it is paid
 * here, once per public function, and it buys the thing the prefix was
 * chosen for -- you can tell from any call site which module the code lives
 * in.  Statics inside mvspwfl.c are unaffected and need no alias.
 *
 * Add a line here for any new function added below.
 */
#define pdsflush_slot              pdsFlSlt
#define pdsflush_dataset_is_full   pdsDsFul

/*
 * Write a pending member out to its PDS and STOW it, then apply the ISPF
 * statistics and invalidate the caches the directory change has made stale.
 *
 * The slot's SPFEDIT enqueue and DISP=SHR allocation were taken at CREATE /
 * first WRITE and are still held, so this opens the allocation by its ddname
 * and neither enqueues nor allocates.  The slot is NOT released here -- the
 * caller owns that, and releasing it here would zero the slot underneath a
 * caller that is about to use it.
 *
 * Returns 0 on success.  Returns -1 with errno set on failure; ENOSPC means
 * the dataset is full, either discovered now (the flush abended, and it is
 * remembered so later flushes fail fast) or already known.  A failed flush
 * leaves the member marked clean: there is nothing retryable, since a retry
 * would only abend again.
 */
int pdsflush_slot(pending_member_t *pm);

/*
 * Is 'dsname' known to have run out of space recently?
 *
 * Set when a flush abends out of space and cleared by the next successful
 * flush; entries also expire, so the server recovers by itself once an
 * operator adds space.  The write path consults this to refuse a WRITE or
 * CREATE up front -- synchronously, so the client actually sees the ENOSPC
 * instead of losing the data to a background flush failure.
 */
int pdsflush_dataset_is_full(const char *dsname, time_t now);

#endif /* MVSPWFL_H_INCLUDED */
