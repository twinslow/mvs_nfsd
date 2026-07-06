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
#define PWW_MAX_PENDING            4                 /* concurrent pending members */
#define PWW_MAX_MEMBER_BYTES       (256 * 1024)      /* per-member in-memory cap   */
#define PWW_IDLE_TIMEOUT_SECONDS   8                 /* flush after this idle time  */

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
} pending_member_t;

/* -------------------------------------------------------------------- */
/* API                                                                   */
/* -------------------------------------------------------------------- */

/* Initialise the pool (call once at startup). */
void pww_init(void);

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

/* Flush one member to the PDS (STOW) now (NFS COMMIT).  The slot is kept
 * (marked clean) so further writes can continue.  Returns 0 on success
 * (including "nothing pending"), -1 on write failure (errno set). */
int  pww_flush_member(const char *dsname_ebcdic, const char *member_name);

/* Flush and release any members idle longer than PWW_IDLE_TIMEOUT_SECONDS.
 * Call once per select() wake-up. */
void pww_flush_idle(time_t now);

/* Flush and release all pending members (server shutdown). */
void pww_flush_all(void);

#endif /* MVSPWW_H_INCLUDED */
