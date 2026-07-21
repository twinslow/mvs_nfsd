/*
 * mvspww.c - Pending-member write pool (PDS member Write).
 *
 * See mvspww.h and doc/design_nfs_write.md.  Phase 1: in-memory buffers
 * only, per-member cap PWW_MAX_MEMBER_BYTES, pool of PWW_MAX_PENDING.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

#include "nfsd.h"
#include "mvspww.h"
#include "mvsspl.h"      /* spill store: spill_open/write/read/close (Phase 2) */
#include "mvspdir.h"
#include "mvsdol.h"
#include "ebcdic.h"
#include "logger.h"
#include "asmutils.h"    /* MVS assembler helpers: mvs_dynalloc(), mvs_stow() */

/* -------------------------------------------------------------------- */
/* Static pool of pending members                                        */
/* -------------------------------------------------------------------- */
static pending_member_t g_pww_pool[PWW_MAX_PENDING];

/* Small reusable scratch for ASCII->EBCDIC translation during flush, so we
   never modify the (ASCII) member buffer -- it must survive across a COMMIT
   in case the client writes more before the slot is released. */
static uint8_t g_pww_xlate[4096];

/* ==================================================================== */
/* Slot pool internals                                                   */
/*                                                                       */
/* Lifecycle of the pending_member_t slots -- find / acquire / release,  */
/* per-slot init, and buffer growth.  All static; the flush machinery    */
/* and the public API below operate through these.                       */
/* ==================================================================== */

/* Defined in the flush-machinery section below; pww_slot_acquire calls it
   to flush a dirty slot before evicting it. */
static int pww_flush_slot(pending_member_t *pm);

#ifdef __MVS__
/* Build the SPFEDIT enqueue RNAME: dsname(44) + member(8), blank-padded --
   the resource ISPF/EDIT (and REVIEW) hold while a member is being edited. */
static void pww_spfedit_rname(const char *dsname_ebcdic,
                              const char *member_name, char *rname_out)
{
    sprintf(rname_out, "%-44.44s%-8.8s", dsname_ebcdic, member_name);
}

/* Acquire serialisation + allocation for a freshly-initialised slot: an
   EXCLUSIVE SPFEDIT enqueue, then a DISP=SHR allocation of DSN(member).  Both
   are held for the slot's lifetime (released by pww_unlock) and recorded
   in the slot's flags, so cleanup is driven by exactly what was acquired.
   Called at CREATE / first WRITE so the conflict surfaces to the client then
   -- at WRITE, which every client issues -- not at flush.
   Returns 0, or -1 (errno set) if the member is being edited elsewhere
   (EACCES) or cannot be allocated (EIO). */
static int pww_lock(pending_member_t *pm)
{
    char rname[44 + 8 + 1];
    char ddname[9];
    int  k;

    /* Exclusive SPFEDIT enqueue.  RET=USE (see mvsenq.asm) fails fast rather
       than waiting, so a member open in an editor does not hang the
       single-threaded server. */
    pww_spfedit_rname(pm->dsname_ebcdic, pm->member_name, rname);
    if (mvs_enq(MVS_ENQ_REQ_ENQ, MVS_ENQ_OPT_EXC, "SPFEDIT", rname) != 0) {
        log_warn("pww_lock: %s(%s) is held (SPFEDIT enqueue) -- refused",
                 pm->dsname_ebcdic, pm->member_name);
        errno = EACCES;
        return -1;
    }
    pm->enq_held = 1;

    /* Allocate DSN(member) DISP=SHR (mvs_dynalloc hard-codes SHR); keep the
       ddname for the flush to open via "//DDN:".  options = 0: no FREE=CLOSE
       -- pww_unlock unallocates. */
    ddname[0] = '\0';
    if (mvs_dynalloc(MVS_DYNALLOC_REQ_ALLOC, 0,
                     pm->dsname_ebcdic, pm->member_name, ddname) != 0) {
        log_error("pww_lock: dynalloc %s(%s) failed",
                  pm->dsname_ebcdic, pm->member_name);
        (void)mvs_enq(MVS_ENQ_REQ_DEQ, MVS_ENQ_OPT_EXC, "SPFEDIT", rname);
        pm->enq_held = 0;
        errno = EIO;
        return -1;
    }
    ddname[8] = '\0';   /* 8 blank-padded chars; trim for "//DDN:ddname" */
    for (k = 8; k > 0 && ddname[k - 1] == ' '; k--)
        ddname[k - 1] = '\0';
    strcpy(pm->ddname, ddname);
    pm->allocated = 1;

    log_debug("pww_lock: %s(%s) enqueued + allocated ddname=%s",
              pm->dsname_ebcdic, pm->member_name, pm->ddname);
    return 0;
}

/* Release whatever pww_lock acquired, driven by the slot's flags so it
   is safe to call unconditionally and after a partial open.  Unallocate
   first, then DEQ (DEQ RET=HAVE is safe even if not held). */
static void pww_unlock(pending_member_t *pm)
{
    if (pm->allocated) {
        char scratch[9];   /* the ddname arg is unused for UNALLOC */
        if (mvs_dynalloc(MVS_DYNALLOC_REQ_UNALLOC, 0,
                         pm->dsname_ebcdic, pm->member_name, scratch) != 0)
            log_warn("pww_unlock: unalloc %s(%s) failed",
                     pm->dsname_ebcdic, pm->member_name);
        pm->allocated = 0;
        pm->ddname[0] = '\0';
    }
    if (pm->enq_held) {
        char rname[44 + 8 + 1];
        pww_spfedit_rname(pm->dsname_ebcdic, pm->member_name, rname);
        if (mvs_enq(MVS_ENQ_REQ_DEQ, MVS_ENQ_OPT_EXC, "SPFEDIT", rname) != 0)
            log_warn("pww_unlock: DEQ %s(%s) failed",
                     pm->dsname_ebcdic, pm->member_name);
        pm->enq_held = 0;
    }
}
#else
static int  pww_lock(pending_member_t *pm)   { (void)pm; return 0; }
static void pww_unlock(pending_member_t *pm) { (void)pm; }
#endif

static void pww_slot_release(pending_member_t *pm)
{
    pww_unlock(pm);        /* DEQ + unallocate whatever the slot still holds */
    spill_close(pm);       /* close the scratch dataset if the member spilled */
    if (pm->buf != NULL)
        free(pm->buf);
    memset(pm, 0, sizeof(*pm));
    pm->status = PWW_STATUS_FREE;
}

/* Find the USED slot for (dsname, member), or NULL. */
static pending_member_t *pww_slot_find(const char *dsname_ebcdic,
                                       const char *member_name)
{
    int i;
    for (i = 0; i < PWW_MAX_PENDING; i++) {
        if (g_pww_pool[i].status != PWW_STATUS_USED)
            continue;
        if (strcmp(g_pww_pool[i].dsname_ebcdic, dsname_ebcdic) == 0 &&
            strcmp(g_pww_pool[i].member_name,  member_name)  == 0)
            return &g_pww_pool[i];
    }
    return NULL;
}

/* Obtain a slot to use for (dsname, member): the existing one if present,
   otherwise a FREE slot, otherwise evict (flush if dirty) the least
   recently used slot.  Never returns NULL. */
static pending_member_t *pww_slot_acquire(const char *dsname_ebcdic,
                                          const char *member_name)
{
    pending_member_t *pm;
    int i;
    int lru;

    pm = pww_slot_find(dsname_ebcdic, member_name);
    if (pm != NULL)
        return pm;

    for (i = 0; i < PWW_MAX_PENDING; i++) {
        if (g_pww_pool[i].status == PWW_STATUS_FREE)
            return &g_pww_pool[i];
    }

    /* Pool full: evict the least-recently-used slot (flush it first). */
    lru = 0;
    for (i = 1; i < PWW_MAX_PENDING; i++) {
        if (g_pww_pool[i].last_write_time < g_pww_pool[lru].last_write_time)
            lru = i;
    }
    log_warn("pww_slot_acquire: pool full, evicting %s(%s)",
        g_pww_pool[lru].dsname_ebcdic, g_pww_pool[lru].member_name);
    if (g_pww_pool[lru].dirty)
        (void)pww_flush_slot(&g_pww_pool[lru]);
    pww_slot_release(&g_pww_pool[lru]);
    return &g_pww_pool[lru];
}

/* Initialise a freshly-acquired slot for (export, dataset, dsname, member). */
static void pww_slot_init(pending_member_t *pm, int export_idx, int dataset_idx,
                          const char *dsname_ebcdic, const char *member_name)
{
    memset(pm, 0, sizeof(*pm));
    pm->status      = PWW_STATUS_USED;
    pm->export_idx  = export_idx;
    pm->dataset_idx = dataset_idx;
    strncpy(pm->dsname_ebcdic, dsname_ebcdic, sizeof(pm->dsname_ebcdic) - 1);
    strncpy(pm->member_name,   member_name,   sizeof(pm->member_name) - 1);
    pm->buf             = NULL;
    pm->buf_cap         = 0;
    pm->high_water      = 0;
    pm->dirty           = 0;
    pm->last_write_time = time(NULL);
}

/* Ensure the in-memory pm->buf has capacity for at least 'need' bytes.  The
   in-memory buffer is capped at PWW_SPILL_THRESHOLD -- past that a member is
   moved to disk (pww_spill_transition), so callers must have kept 'need' within
   the threshold before calling. */
static int pww_slot_ensure_cap(pending_member_t *pm, uint32_t need)
{
    uint32_t new_cap;
    uint8_t *new_buf;

    if (need > (uint32_t)PWW_SPILL_THRESHOLD) {
        errno = ENOSPC;   /* should not happen: caller spills past the threshold */
        return -1;
    }
    if (need <= pm->buf_cap)
        return 0;

    new_cap = (pm->buf_cap != 0) ? pm->buf_cap : 4096u;
    while (new_cap < need)
        new_cap *= 2u;
    if (new_cap > (uint32_t)PWW_SPILL_THRESHOLD)
        new_cap = (uint32_t)PWW_SPILL_THRESHOLD;

    new_buf = (uint8_t *)realloc(pm->buf, (size_t)new_cap);
    if (new_buf == NULL) {
        log_error("pww_slot_ensure_cap: realloc to %u bytes failed", new_cap);
        errno = ENOMEM;
        return -1;
    }
    pm->buf     = new_buf;
    pm->buf_cap = new_cap;
    return 0;
}

/* Move an in-memory slot to disk: open its scratch dataset, copy the current
   buffer into it at offset 0, and free the buffer.  On any failure the slot is
   rolled back to in-memory (buffer intact, scratch closed) and -1 is returned
   with errno set, so the caller can fail the write and the slot stays coherent.
   Caller must have checked pm->spill_fp == NULL. */
static int pww_spill_transition(pending_member_t *pm)
{
    int slot = (int)(pm - g_pww_pool);

    if (spill_open(pm, slot) != 0)
        return -1;                          /* still fully in memory */

    if (pm->high_water > 0 && pm->buf != NULL) {
        if (spill_write(pm, 0, pm->buf, pm->high_water) != 0) {
            int saved = errno;
            spill_close(pm);                /* roll back to in-memory */
            errno = saved;
            return -1;
        }
    }

    if (pm->buf != NULL) {
        free(pm->buf);
        pm->buf     = NULL;
        pm->buf_cap = 0;
    }
    log_debug("pww_spill_transition: %s(%s) now on disk (%u bytes)",
        pm->dsname_ebcdic, pm->member_name, pm->high_water);
    return 0;
}

/* ==================================================================== */
/* Flush machinery -- write the buffered member to the PDS and STOW it   */
/* ==================================================================== */

#ifndef __MVS__
/* Build the "//DSN:dsname(member)" open path.  MVS goes through dynamic
   allocation + "//DDN:" instead (see pww_flush_slot), so this is only used
   by the non-MVS (dev/mock) build. */
static void pww_member_path(char *out, const char *dsname_ebcdic,
                            const char *member_name)
{
    strcpy(out, "//DSN:");
    strcat(out, dsname_ebcdic);
    strcat(out, "(");
    strcat(out, member_name);
    strcat(out, ")");
}
#endif

/* Read len bytes at off from a pending member's content into dst, from the
   in-memory buffer or the spill dataset -- the single accessor the flush and
   vfs_pread both use so neither cares where the member is backed. */
int pww_read_range(pending_member_t *pm, uint32_t off,
                   uint8_t *dst, uint32_t len)
{
    if (len == 0)
        return 0;
    if (pm->spill_fp == NULL) {
        memcpy(dst, pm->buf + off, (size_t)len);
        return 0;
    }
    return spill_read(pm, off, dst, len);   /* -1 with errno on read error */
}

/* Open 'open_target' ("//DDN:ddname" on MVS, "//DSN:dsname(member)"
   elsewhere), write the pending member's byte stream as text records, and STOW
   it at close.  The content is read in chunks via pww_read_range (memory or
   spill) into g_pww_xlate and translated ASCII->EBCDIC in place, so neither the
   in-memory buffer nor the spill file is disturbed.  Returns 0 on success, -1
   on failure (errno set).  The caller owns any dynamic allocation and frees it
   regardless of the result -- keeping that cleanup in one place is why this is
   split out. */
static int pww_write_member(pending_member_t *pm, const char *open_target,
                            int32_t *lines_out)
{
    FILE    *fh;
    uint32_t off;
    uint32_t n;
    uint32_t i;
    size_t   w;
    int32_t  lines = 0;
    uint8_t  last  = 0;

    fh = fopen(open_target, "wt");    /* text mode: record per '\n', STOW on close */
    if (fh == NULL) {
        log_error("pww_flush_slot: fopen %s failed: %s",
                  open_target, strerror(errno));
        return -1;
    }

    /* One read pass: read a chunk, count records off the ASCII (one per LF, plus
       a trailing partial line), then translate ASCII->EBCDIC in place and write.
       Counting here avoids a second full read pass over the (possibly spilled)
       content just to compute the ISPF line count. */
    for (off = 0; off < pm->high_water; off += n) {
        n = pm->high_water - off;
        if (n > sizeof(g_pww_xlate))
            n = sizeof(g_pww_xlate);
        if (pww_read_range(pm, off, g_pww_xlate, n) != 0) {
            log_error("pww_flush_slot: read-back failed on %s at %u",
                      open_target, off);
            fclose(fh);
            return -1;
        }
        for (i = 0; i < n; i++)
            if (g_pww_xlate[i] == 0x0A)   /* ASCII LF -- the stream is ASCII */
                lines++;
        last = g_pww_xlate[n - 1];
        ascii_to_ebcdic(g_pww_xlate, g_pww_xlate, (size_t)n);   /* in place */
        w = fwrite(g_pww_xlate, 1, (size_t)n, fh);
        if (w != (size_t)n) {
            log_error("pww_flush_slot: short write on %s (%u of %u)",
                      open_target, (unsigned)w, n);
            fclose(fh);
            return -1;
        }
    }

    if (pm->high_water > 0 && last != 0x0A)   /* trailing partial line is a record */
        lines++;
    *lines_out = lines;

    if (fclose(fh) != 0) {            /* STOW happens here */
        log_error("pww_flush_slot: fclose(STOW) %s failed: %s",
                  open_target, strerror(errno));
        return -1;
    }
    return 0;
}

/* Write the buffered member out in one pass and STOW it.
   Returns 0 on success, -1 on failure (errno set). */
static int pww_flush_slot(pending_member_t *pm)
{
    char     path[6 + 44 + 1 + 8 + 1 + 1];

    pds_member_entry_t  existing_ent;
    pds_member_entry_t *existing;
    pds_member_entry_t  new_stats;
    uint8_t             stats_ud[MVS_ISPF_STATS_LEN];
    int                 want_stats;
    int32_t             line_count;

    /* Read the member's current directory entry (if any) BEFORE opening it
       for output, so we never have the PDS open for input and output at once.
       NULL => the member does not yet exist (a new member). */
    existing = mvs_pds_get_member_entry(pm->dsname_ebcdic, pm->member_name,
                                        pm->export_idx, &existing_ent);

#ifdef __MVS__
    /* The SPFEDIT enqueue and the DSN(member) DISP=SHR allocation were taken at
       CREATE / first WRITE (pww_lock) and are held for the slot's whole
       lifetime, so the flush neither enqueues nor allocates -- it simply opens
       the held allocation by its ddname ("//DDN:ddname") and writes + STOWs
       through it (fclose STOWs).  The enqueue and allocation are released only
       when the slot is released (pww_slot_release -> pww_unlock), which
       also covers the ISPF-stats STOW below.  pww_write_member also returns the
       record count (single read pass) for the ISPF stats applied afterwards. */
    strcpy(path, "//DDN:");
    strcat(path, pm->ddname);
    if (pww_write_member(pm, path, &line_count) != 0)   /* write + STOW */
        return -1;
#else
    pww_member_path(path, pm->dsname_ebcdic, pm->member_name);
    if (pww_write_member(pm, path, &line_count) != 0)
        return -1;
#endif

    pm->dirty = 0;
    log_info("pww_flush_slot: stowed %s(%s), %u bytes",
        pm->dsname_ebcdic, pm->member_name, pm->high_water);

#ifdef __MVS__
    /* Apply ISPF statistics.  This runs AFTER the member has been stowed by
       fclose, because mvs_stow() does BLDL+FIND+STOW REPLACE and so needs the
       member to already exist in the directory.  (JCC's __setstow() cannot do
       this for an FB member opened through stdio -- it returns EINVAL.)

       Two assembler helpers: mvs_dynalloc() dynamically allocates the PDS
       (DISP=SHR, FREE=CLOSE) and returns its system-assigned ddname; mvs_stow()
       opens that ddname, patches the directory user-data, and closes it -- and
       because the allocation is FREE=CLOSE, that close also frees it, so no
       explicit unallocate is needed.

       The allocation is dataset-level -- member is passed as NULL so the PDS is
       allocated without a member qualifier (BLDL/FIND/STOW work on the
       directory; mvs_stow() gets the member name for the BLDL separately). */
    want_stats = mvs_build_write_stats(&new_stats, existing, line_count,
                                       time(NULL));
    if (want_stats) {
        char ddname[9];
        int  rc;

        mvs_encode_ispf_stats(&new_stats, stats_ud);

        ddname[0] = '\0';
        rc = mvs_dynalloc(MVS_DYNALLOC_REQ_ALLOC, MVS_DYNALLOC_OPT_FREECLOSE,
                          pm->dsname_ebcdic, NULL, ddname);
        if (rc == 0) {
            ddname[8] = '\0';   /* dynalloc returns 8 blank-padded chars */
            rc = mvs_stow(ddname, pm->member_name,
                          stats_ud, (int)MVS_ISPF_STATS_LEN);
        } else {
            log_debug("pww_flush_slot: mvs_dynalloc failed for %s (rc=%d)",
                pm->dsname_ebcdic, rc);
        }

        if (rc != 0) {
            static int stats_warned = 0;
            if (!stats_warned) {
                stats_warned = 1;
                log_warn("pww_flush_slot: ISPF stats update failed for %s(%s)"
                         " (rc=%d) -- members stowed without ISPF stats"
                         " (further warnings suppressed)",
                         pm->dsname_ebcdic, pm->member_name, rc);
            }
        }
    }
    /* No DEQ / unallocate here: both are held until the slot is released. */
#else
    (void)existing;
    (void)line_count;
    (void)want_stats;
    (void)new_stats;
    (void)stats_ud;
#endif

    /* The PDS directory just changed: bump its mtime so clients
       invalidate their cached listing, and drop our own cached listing so the
       next readdir re-reads the directory and includes the new member. */
    export_dataset_touch(pm->export_idx, pm->dataset_idx);
    dir_openlist_invalidate(pm->dsname_ebcdic);

    return 0;
}

/* ==================================================================== */
/* Public API (declared in mvspww.h; called from the vfs_* layer)        */
/* ==================================================================== */

/* Initialise the pool.  Call once at startup. */
void pww_init(void)
{
    memset(g_pww_pool, 0, sizeof(g_pww_pool));
}

int pww_create(int export_idx, int dataset_idx,
               const char *dsname_ebcdic, const char *member_name)
{
    pending_member_t *pm;

    pm = pww_slot_acquire(dsname_ebcdic, member_name);
    if (pm->status != PWW_STATUS_USED ||
        strcmp(pm->dsname_ebcdic, dsname_ebcdic) != 0 ||
        strcmp(pm->member_name, member_name) != 0) {
        /* Fresh or reused slot: (re)initialise it, then take the SPFEDIT
           enqueue and allocate the member (held for the slot's lifetime).  If
           the member is being edited elsewhere, fail the create now. */
        pww_slot_init(pm, export_idx, dataset_idx, dsname_ebcdic, member_name);
        if (pww_lock(pm) != 0) {
            int saved_errno = errno;
            pww_slot_release(pm);
            errno = saved_errno;
            return -1;              /* errno set (EACCES held / EIO alloc) */
        }
    } else {
        /* Re-create over an existing pending member (already open): truncate. */
        pm->high_water = 0;
    }

    /* An empty create still needs to stow an empty member on COMMIT. */
    pm->dirty           = 1;
    pm->last_write_time = time(NULL);

    log_debug("pww_create: %s(%s)", dsname_ebcdic, member_name);
    return 0;
}

int pww_write(int export_idx, int dataset_idx,
              const char *dsname_ebcdic, const char *member_name,
              const uint8_t *data, uint32_t count, uint64_t offset)
{
    pending_member_t *pm;
    uint64_t          end;

    end = offset + (uint64_t)count;
    if (end > (uint64_t)PWW_MAX_MEMBER_BYTES) {
        log_warn("pww_write: %s(%s) exceeds %d-byte cap (offset=%llu count=%u)",
            dsname_ebcdic, member_name, PWW_MAX_MEMBER_BYTES,
            (unsigned long long)offset, count);
        errno = ENOSPC;   /* JCC has no EFBIG; NOSPC maps to NFS3ERR_NOSPC */
        return -1;
    }

    pm = pww_slot_acquire(dsname_ebcdic, member_name);
    if (pm->status != PWW_STATUS_USED ||
        strcmp(pm->dsname_ebcdic, dsname_ebcdic) != 0 ||
        strcmp(pm->member_name, member_name) != 0) {
        /* First write to this member: initialise the slot, then take the
           SPFEDIT enqueue and allocate it (held until the slot is released).
           A member being edited elsewhere fails the write here -- every
           client issues WRITE, so the conflict always surfaces. */
        pww_slot_init(pm, export_idx, dataset_idx, dsname_ebcdic, member_name);
        if (pww_lock(pm) != 0) {
            int saved_errno = errno;
            pww_slot_release(pm);
            errno = saved_errno;
            return -1;              /* errno set (EACCES held / EIO alloc) */
        }
    }

    /* In memory while the member stays under the spill threshold; once it (or
       this write) would exceed it, move to a temp dataset and keep it there.
       Either way memory use per pending member is bounded by the threshold. */
    if (pm->spill_fp == NULL && end <= (uint64_t)PWW_SPILL_THRESHOLD) {
        if (pww_slot_ensure_cap(pm, (uint32_t)end) < 0)
            return -1;    /* errno set (ENOSPC/ENOMEM) */

        /* Zero-fill any gap between the current end and this write's offset. */
        if (offset > (uint64_t)pm->high_water)
            memset(pm->buf + pm->high_water, 0,
                   (size_t)(offset - (uint64_t)pm->high_water));

        if (count > 0)
            memcpy(pm->buf + offset, data, (size_t)count);
    } else {
        /* Spill path: transition on the write that first crosses the threshold,
           then place this segment in the scratch dataset (spill_write handles
           the hole zero-fill and the "cannot fseek past EOF" extension). */
        if (pm->spill_fp == NULL) {
            if (pww_spill_transition(pm) != 0)
                return -1;    /* errno set; slot rolled back to in-memory */
        }
        if (spill_write(pm, (uint32_t)offset, data, count) != 0)
            return -1;        /* errno set (EIO) */
    }

    if (end > (uint64_t)pm->high_water)
        pm->high_water = (uint32_t)end;

    pm->dirty           = 1;
    pm->last_write_time = time(NULL);

    log_debug("pww_write: %s(%s) off=%llu cnt=%u hw=%u",
        dsname_ebcdic, member_name, (unsigned long long)offset, count,
        pm->high_water);
    return 0;
}

int pww_truncate(int export_idx, int dataset_idx,
                 const char *dsname_ebcdic, const char *member_name,
                 uint32_t size)
{
    pending_member_t *pm;

    if (size > (uint32_t)PWW_MAX_MEMBER_BYTES) {
        errno = ENOSPC;
        return -1;
    }

    pm = pww_slot_find(dsname_ebcdic, member_name);

    if (pm == NULL) {
        /* No pending member.  A truncate-to-empty (the O_TRUNC case) starts a
           fresh empty member so COMMIT replaces the on-disk member with the
           new content.  A truncate to a non-zero size of an on-disk-only
           member is not supported in Phase 1; accept it as a no-op so the
           client is not blocked (see doc/design_nfs_write.md). */
        if (size != 0)
            return 0;
        pm = pww_slot_acquire(dsname_ebcdic, member_name);
        pww_slot_init(pm, export_idx, dataset_idx, dsname_ebcdic, member_name);
        if (pww_lock(pm) != 0) {     /* enqueue + allocate the new member */
            int saved_errno = errno;
            pww_slot_release(pm);
            errno = saved_errno;
            return -1;                    /* errno set (EACCES held / EIO alloc) */
        }
        pm->dirty           = 1;
        pm->last_write_time = time(NULL);
        log_debug("pww_truncate: %s(%s) -> 0 (new empty)",
            dsname_ebcdic, member_name);
        return 0;
    }

    /* A truncate to the size the member already has is a no-op: return WITHOUT
       re-dirtying the slot.  Otherwise a SETATTR(size) that the client issues
       after a COMMIT (its sequence is WRITE -> COMMIT -> SETATTR -> COMMIT)
       would mark the just-flushed member dirty again and trigger a redundant
       second STOW. */
    if (size == pm->high_water)
        return 0;

    /* Adjust the existing pending member to exactly 'size' bytes, spilling if
       the new size crosses the in-memory threshold. */
    if (pm->spill_fp != NULL) {
        /* Already spilled: grow by zero-extending the scratch; shrink just
           lowers the logical extent so a later write into the freed region
           zero-fills again (the flush reads only [0..high_water]). */
        if (size > pm->high_water) {
            if (spill_write(pm, size, NULL, 0) != 0)
                return -1;               /* errno set (EIO) */
        } else {
            pm->spill_size = size;
        }
    } else if (size <= (uint32_t)PWW_SPILL_THRESHOLD) {
        /* Stays in memory. */
        if (size > pm->buf_cap) {
            if (pww_slot_ensure_cap(pm, size) < 0)
                return -1;
        }
        if (size > pm->high_water)
            memset(pm->buf + pm->high_water, 0,
                   (size_t)(size - pm->high_water));
    } else {
        /* In memory but growing past the threshold: spill, then zero-extend the
           scratch to 'size' (an in-memory member is <= threshold < size, so
           this is necessarily a grow). */
        if (pww_spill_transition(pm) != 0)
            return -1;
        if (spill_write(pm, size, NULL, 0) != 0)
            return -1;
    }

    pm->high_water      = size;
    pm->dirty           = 1;
    pm->last_write_time = time(NULL);
    log_debug("pww_truncate: %s(%s) -> %u", dsname_ebcdic, member_name, size);
    return 0;
}

pending_member_t *pww_find(const char *dsname_ebcdic, const char *member_name)
{
    return pww_slot_find(dsname_ebcdic, member_name);
}

int pww_discard(const char *dsname_ebcdic, const char *member_name)
{
    pending_member_t *pm = pww_slot_find(dsname_ebcdic, member_name);
    if (pm == NULL)
        return 0;                  /* nothing buffered for this member */
    /* Drop the buffer WITHOUT flushing -- the caller (REMOVE) is deleting the
       member, so it must not be re-STOWed by a later flush. */
    pww_slot_release(pm);
    return 1;
}

int pww_flush_member(const char *dsname_ebcdic, const char *member_name)
{
    pending_member_t *pm;

    pm = pww_slot_find(dsname_ebcdic, member_name);
    if (pm == NULL)
        return 0;                  /* nothing pending: already committed */
    if (!pm->dirty)
        return 0;                  /* nothing new to flush */

    /* Keep the slot after a successful flush (the client may write more);
       the idle sweep releases it later. */
    return pww_flush_slot(pm);
}

void pww_flush_idle(time_t now)
{
    int i;
    for (i = 0; i < PWW_MAX_PENDING; i++) {
        pending_member_t *pm = &g_pww_pool[i];
        if (pm->status != PWW_STATUS_USED)
            continue;
        if (now - pm->last_write_time <= PWW_IDLE_TIMEOUT_SECONDS)
            continue;

        if (pm->dirty) {
            if (pww_flush_slot(pm) < 0)
                log_error("pww_flush_idle: flush failed for %s(%s)",
                    pm->dsname_ebcdic, pm->member_name);
        }
        pww_slot_release(pm);
    }
}

void pww_flush_all(void)
{
    int i;
    for (i = 0; i < PWW_MAX_PENDING; i++) {
        pending_member_t *pm = &g_pww_pool[i];
        if (pm->status != PWW_STATUS_USED)
            continue;
        if (pm->dirty) {
            if (pww_flush_slot(pm) < 0)
                log_error("pww_flush_all: flush failed for %s(%s)",
                    pm->dsname_ebcdic, pm->member_name);
        }
        pww_slot_release(pm);
    }
}

/* Refresh an existing member's ISPF "changed" date from a SETATTR time change,
   via a stats-only STOW (no content rewrite, no mod-level change).  Applies
   only to a member that already carries ISPF stats, and only when new_time
   actually differs (ISPF resolves to one second) from the stored changed date
   -- so a plain copy (whose mtime matches the just-stowed date) does nothing,
   while a touch or a timestamp-preserving copy updates it.  Always returns 0
   (a SETATTR must not fail; clients issue it inside the write sequence). */
int pww_touch_stats(int export_idx, const char *dsname_ebcdic,
                    const char *member_name, time_t new_time)
{
#ifdef __MVS__
    pending_member_t   *pm;
    pds_member_entry_t  entry;
    pds_member_entry_t *ep;
    uint8_t             stats_ud[MVS_ISPF_STATS_LEN];
    char                ddname[9];
    char                rname[44 + 8 + 1];
    int                 rc;
    int                 own_enq;

    /* If content is still pending (dirty), the upcoming flush will set the
       changed date -- skip, or this STOW would just be overwritten. */
    pm = pww_slot_find(dsname_ebcdic, member_name);
    if (pm != NULL && pm->dirty)
        return 0;

    ep = mvs_pds_get_member_entry(dsname_ebcdic, member_name, export_idx, &entry);
    if (ep == NULL)
        return 0;                       /* not found -- nothing to update */
    if (!(entry.info_flags & MVS_PDSDIR_IFLG_ISPFSTATS))
        return 0;                       /* no ISPF stats -- do not fabricate */
    if (entry.chgdate == (int32_t)new_time)
        return 0;                       /* already that time (1s resolution) */

    /* Serialise with an editor: this STOW rewrites the PDS directory, so hold
       the same SPFEDIT enqueue used for a write.  If a pending slot for this
       member already holds it (a non-dirty slot kept after a COMMIT flush),
       reuse that -- re-enqueuing our own task would report "already held" and
       a matching DEQ would prematurely drop the slot's enqueue.  Otherwise
       take our own; if the member is being edited elsewhere, skip the touch --
       the editor's save will set the changed date anyway. */
    own_enq = 0;
    if (pm == NULL || !pm->enq_held) {
        pww_spfedit_rname(dsname_ebcdic, member_name, rname);
        if (mvs_enq(MVS_ENQ_REQ_ENQ, MVS_ENQ_OPT_EXC, "SPFEDIT", rname) != 0) {
            log_debug("pww_touch_stats: %s(%s) is held -- skipping stats touch",
                dsname_ebcdic, member_name);
            return 0;
        }
        own_enq = 1;
    }

    entry.chgdate = (int32_t)new_time;  /* update the changed date only */
    mvs_encode_ispf_stats(&entry, stats_ud);

    ddname[0] = '\0';
    rc = mvs_dynalloc(MVS_DYNALLOC_REQ_ALLOC, MVS_DYNALLOC_OPT_FREECLOSE,
                      dsname_ebcdic, NULL, ddname);
    if (rc == 0) {
        ddname[8] = '\0';
        rc = mvs_stow(ddname, member_name, stats_ud, (int)MVS_ISPF_STATS_LEN);
    }
    if (rc != 0)
        log_warn("pww_touch_stats: stats update failed for %s(%s) rc=%d",
            dsname_ebcdic, member_name, rc);

    if (own_enq)
        (void)mvs_enq(MVS_ENQ_REQ_DEQ, MVS_ENQ_OPT_EXC, "SPFEDIT", rname);
    return 0;
#else
    (void)export_idx; (void)dsname_ebcdic; (void)member_name; (void)new_time;
    return 0;
#endif
}
