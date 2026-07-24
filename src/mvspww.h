/*
 * mvspww.h - Pending-member write pool (PDS member Write).
 *
 * NFSv3 WRITE is random-access (offset+count), but a PDS member can only be
 * written sequentially in a single open->write->close pass (fclose performs
 * the STOW).  So writes are reassembled in an in-memory buffer per member and
 * the whole member is written in one pass on COMMIT / idle timeout / shutdown.
 *
 * Phase 1: in-memory buffers only, with a hard per-member cap.  A future
 * phase can back the buffer with a scratch PS dataset without changing this
 * API.  See doc/design_nfs_write.md.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#ifndef MVSPWW_H_INCLUDED
#define MVSPWW_H_INCLUDED

#include <stdio.h>   /* FILE (the spill scratch handle in pending_member_t) */
#include <time.h>
#include "types.h"

/*
 * MVS short-name aliases (like ebcdic.h).  These three names collide in the
 * first 8 characters, so they need explicit distinct external names for the
 * MVS linker.  Defined here (before the prototypes) so aliasing is
 * consistent regardless of this header's include order relative to nfsd.h.
 */
#if defined(__MVS__)
#define pww_flush_member        pwwFlMbr
#define pww_flush_idle          pwwFlIdl
#define pww_flush_all           pwwFlAll
#endif

/* -------------------------------------------------------------------- */
/* Phase 1 parameters                                                    */
/* -------------------------------------------------------------------- */
#define PWW_MAX_PENDING            8                 /* concurrent pending members */
#define PWW_SPILL_THRESHOLD        (16 * 1024)      /* in-memory prefix; a member
                                                        larger than this spills to 
                                                        a temp dataset (mvsspl.c).*/
#define PWW_SPILL_DS_SPACE_PARMS   "pri=15,sec=15,rlse,unit=sysda"   /* This string
                                                        will be concatenated into
                                                        the file open for the spill
                                                        temporary dataset at 
                                                        compile time              */
#define PWW_MAX_MEMBER_BYTES       (1024 * 1024)     /* absolute per-member cap;
                                                        disk-backed past the
                                                        spill threshold           */
#define PWW_IDLE_TIMEOUT_SECONDS   3                 /* flush + release the slot
                                                        (dropping the SPFEDIT
                                                        enqueue) this long after
                                                        the last write/commit    */

#define PWW_STATUS_FREE            0
#define PWW_STATUS_USED            1

/* -------------------------------------------------------------------- */
/* One pending (being-written) PDS member                                */
/* -------------------------------------------------------------------- */
typedef struct {
    uint8_t   status;              /* FREE / USED                          */
    int       export_idx;
    int       dataset_idx;         /* dataset within export (RECFM/LRECL)  */
    char      dsname_ebcdic[45];   /* real PDS dataset name (EBCDIC)       */
    char      member_name[9];      /* member name (EBCDIC, <=8 + NUL)      */
    uint8_t  *buf;                 /* reassembled byte stream (ASCII)      */
    uint32_t  buf_cap;             /* allocated capacity of buf            */
    uint32_t  high_water;          /* logical size = highest offset+count  */
    time_t    last_write_time;     /* for the idle-flush sweep             */
    uint8_t   dirty;               /* has data not yet stowed              */

    /*
     * Serialisation / allocation state, acquired at CREATE or first WRITE and
     * held until the slot is released (see pww_lock / pww_unlock in
     * mvspww.c).  Each flag reflects a resource currently held, and drives
     * exactly what must be cleaned up on release or error.  Written the moment
     * the resource is acquired or freed, so the flags never lie.
     */
    uint8_t   enq_held;            /* 1 = SPFEDIT enqueue is held           */
    uint8_t   allocated;           /* 1 = the DSN(member) is allocated      */
    char      ddname[9];           /* ddname of that allocation (NUL-term)  */

    /*
     * Phase 2 spill (mvsspl.c): once the byte stream passes
     * PWW_SPILL_THRESHOLD the content lives in a temporary dataset and buf is
     * freed.  spill_fp != NULL is the "this member is spilled" flag.
     */
    FILE     *spill_fp;            /* open scratch dataset, or NULL         */
    uint32_t  spill_size;          /* bytes on disk (== high_water spilled) */
    int       spill_slot;          /* slot index -> &&PWWSP<nn>, for reopen */
    uint8_t   spill_dirty;         /* writes pending since the last commit  */
} pending_member_t;

/* -------------------------------------------------------------------- */
/* API                                                                   */
/* -------------------------------------------------------------------- */

/* Initialise the pool (call once at startup). */
void pww_init(void);

/* Non-zero once the flush path has trapped an abend it does NOT recover from
 * (anything outside the out-of-space family -- i.e. a probable program error),
 * or once cleanup failed to release an allocation / SPFEDIT enqueue.
 *
 * The main select loop polls this and shuts down cleanly.  Recovering from an
 * unexpected abend and carrying on would hide a real bug and keep serving from
 * state we no longer trust; ending the task also lets MVS reclaim any resource
 * we could not release ourselves.  See doc/design_nfs_write.md Sec 7.3. */
int  pww_fatal_abend(void);

/* Reserve a pending member for a new/truncated file (NFS CREATE).
 * Returns 0 on success, -1 on error (errno set). */
int  pww_create(int export_idx, int dataset_idx,
                const char *dsname_ebcdic, const char *member_name);

/* Place count bytes of data at offset in the member's buffer (NFS WRITE),
 * allocating a pending slot on first write.  Returns 0 on success, -1 on
 * error (errno: ENOSPC over cap, ENOMEM on allocation failure). */
int  pww_write(int export_idx, int dataset_idx,
               const char *dsname_ebcdic, const char *member_name,
               const uint8_t *data, uint32_t count, uint64_t offset);

/* Set a pending member's logical size to 'size' bytes (NFS SETATTR size /
 * O_TRUNC).  size 0 starts a fresh empty member; a non-zero truncate of an
 * on-disk-only member is accepted as a no-op in Phase 1.  0 / -1. */
int  pww_truncate(int export_idx, int dataset_idx,
                  const char *dsname_ebcdic, const char *member_name,
                  uint32_t size);

/* Find a pending member, or NULL.  Used by vfs_stat so an in-progress
 * (not-yet-stowed) member is visible with its current size. */
pending_member_t *pww_find(const char *dsname_ebcdic, const char *member_name);

/* Read len bytes at off from a pending member's content -- from its in-memory
 * buffer, or transparently from the spill dataset once spilled.  off+len must
 * be <= high_water.  Returns 0, or -1 on a spill read error (errno set).  Lets
 * vfs_pread serve a not-yet-stowed member regardless of where it is backed. */
int  pww_read_range(pending_member_t *pm, uint32_t off,
                    uint8_t *dst, uint32_t len);

/* Discard any pending (buffered) write for a member WITHOUT flushing it.
 * Used by REMOVE so a delete cannot be undone by a later flush re-STOWing the
 * member.  Returns 1 if a pending member was dropped, 0 if none was pending. */
int  pww_discard(const char *dsname_ebcdic, const char *member_name);

/* Flush one member to the PDS (STOW) now (NFS COMMIT).  The slot is kept
 * (marked clean) so further writes can continue.  Returns 0 on success
 * (including "nothing pending"), -1 on write failure (errno set). */
int  pww_flush_member(const char *dsname_ebcdic, const char *member_name);

/* Flush and release any members idle longer than PWW_IDLE_TIMEOUT_SECONDS.
 * Call once per select() wake-up. */
void pww_flush_idle(time_t now);

/* Flush and release all pending members (server shutdown). */
void pww_flush_all(void);

/* SETATTR time change: refresh an existing member's ISPF "changed" date to
 * new_time via a stats-only STOW (no content rewrite, no mod-level change).
 * No-op unless the member already has ISPF stats and new_time differs (to the
 * second) from the stored changed date.  Always returns 0. */
int  pww_touch_stats(int export_idx, int dataset_idx,
                     const char *dsname_ebcdic,
                     const char *member_name, time_t new_time);

#endif /* MVSPWW_H_INCLUDED */
