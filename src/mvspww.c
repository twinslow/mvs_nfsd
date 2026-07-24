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

#ifdef __MVS__
#include <setjmp.h>      /* jmp_buf                                          */
#include <mvsutils.h>    /* _setjmp_stae / _setjmp_canc -- STAE abend trap   */
#endif

/* -------------------------------------------------------------------- */
/* Static pool of pending members                                        */
/* -------------------------------------------------------------------- */
static pending_member_t g_pww_pool[PWW_MAX_PENDING];

/* Small reusable scratch for ASCII->EBCDIC translation during flush, so we
   never modify the (ASCII) member buffer -- it must survive across a COMMIT
   in case the client writes more before the slot is released. */
static uint8_t g_pww_xlate[4096];

#ifdef __MVS__
/* -------------------------------------------------------------------- */
/* Abend protection for the flush (design_nfs_write.md Sec 7.3)          */
/*                                                                       */
/* Writing a PDS member abends when the dataset fills, and without a     */
/* trap that terminates the whole NFSD task -- one user filling a PDS    */
/* would take the server down for everyone.  _setjmp_stae() establishes  */
/* an MVS STAE and behaves like setjmp: 0 = armed, 1 = an abend was      */
/* intercepted (the SDWA copy holds the diagnostics), anything else =    */
/* the STAE could not be established.  _setjmp_canc() cancels it and     */
/* MUST be called on the normal path -- establish/cancel are LIFO and    */
/* have to balance.                                                      */
/*                                                                       */
/* What we actually catch is B14, not D37: the data write hits D37, the  */
/* runtime absorbs it (it reaches the console, not us), and then the     */
/* runtime's own internal fclose() cannot complete its STOW and abends    */
/* B14 -- that is the code that lands in the SDWA.                        */
/* -------------------------------------------------------------------- */

/* System completion code out of the SDWA copy, e.g. 0xB14. */
#define PWW_ABEND_CODE(sdwa)   (((sdwa)[1] & 0x00FFF000u) >> 12)

/* The member stream, reachable from the recovery path: the abend unwinds out
   of pww_write_member, so its local FILE * is gone by the time we regain
   control.  The server is single-threaded and flushes one slot at a time, so a
   single module-scope handle is sufficient.  Set on open, cleared on close or
   recovery -- and NEVER closed by the recovery path (the runtime already did
   that, which is what raised the abend). */
static FILE *g_flush_fp = NULL;

/* Set by the region-A guard when the member write abended, so pww_flush_slot
   can tell a genuine abend from an ordinary I/O failure and release the slot
   instead of leaving it to be retried. */
static int g_flush_abended = 0;

/* Set by the region-B guard to the completion code when the ISPF-stats STOW
   abended (0 = it did not), so the warning can name it. */
static int g_stats_abend = 0;

/* Map an abend completion code onto errno.  B14 is the visible face of an
   out-of-space condition (see above) and is also how a directory-full STOW
   surfaces, so it maps to ENOSPC -> NFS3ERR_NOSPC, which is the diagnosis the
   client should see. */
static int pww_abend_errno(unsigned int code)
{
    switch (code) {
    case 0xB14:                                    /* CLOSE/STOW failed     */
    case 0xB37: case 0xD37: case 0xE37:            /* out of space/extents  */
        return ENOSPC;
    default:
        return EIO;
    }
}

/* Publish / clear the in-flight member stream for the recovery path. */
#define PWW_FLUSH_FP(f)   (g_flush_fp = (f))
#else
#define PWW_FLUSH_FP(f)   ((void)0)   /* no STAE off-MVS: nothing to publish */
#endif /* __MVS__ */

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

#ifdef __MVS__
/* Release a slot from the abend-recovery path, under its own STAE.
   Unallocating the member and DEQ'ing the SPFEDIT enqueue is MANDATORY after a
   failed flush -- a retained enqueue locks the member against ISPF for the life
   of the task -- but we are running just after an abend, so a secondary abend
   inside the unallocate/DEQ is plausible and must not escape either. */
static void pww_slot_release_guarded(pending_member_t *pm)
{
    jmp_buf      env;
    unsigned int sdwa[26];      /* 104-byte SDWA copy (IHASDWA) */
    long         rc;

    rc = _setjmp_stae(env, (unsigned char *)sdwa);
    if (rc == 0) {
        pww_slot_release(pm);
        if (_setjmp_canc() != 0)
            log_warn("pww_slot_release: _setjmp_canc failed");
        return;
    }
    if (rc == 1) {
        log_error("pww_slot_release: ABEND S%03X cleaning up %s(%s) -- the"
                  " allocation and/or SPFEDIT enqueue may still be held",
                  PWW_ABEND_CODE(sdwa), pm->dsname_ebcdic, pm->member_name);
        /* Free the slot anyway: leaving it USED would wedge the pool, which is
           worse than the leak we have just reported. */
        memset(pm, 0, sizeof(*pm));
        pm->status = PWW_STATUS_FREE;
        return;
    }
    log_warn("pww_slot_release: STAE not established (rc=%ld) --"
             " releasing unprotected", rc);
    pww_slot_release(pm);
}
#endif /* __MVS__ */

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
    PWW_FLUSH_FP(fh);                 /* visible to the abend-recovery path */

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
            PWW_FLUSH_FP(NULL);
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
            PWW_FLUSH_FP(NULL);
            return -1;
        }
    }

    if (pm->high_water > 0 && last != 0x0A)   /* trailing partial line is a record */
        lines++;
    *lines_out = lines;

    if (fclose(fh) != 0) {            /* STOW happens here */
        PWW_FLUSH_FP(NULL);
        log_error("pww_flush_slot: fclose(STOW) %s failed: %s",
                  open_target, strerror(errno));
        return -1;
    }
    PWW_FLUSH_FP(NULL);
    return 0;
}

#ifdef __MVS__
/* Region A of the flush (design_nfs_write.md Sec 7.3): run pww_write_member
   under a STAE so an out-of-space abend fails THIS request instead of
   terminating the NFSD task.

   On an abend the member content is lost or partial -- a real failure -- so we
   set errno (ENOSPC for the out-of-space family) and return -1; the caller
   releases the slot rather than retrying, because a retry would only abend
   again.  We do NOT close the stream here: the runtime already closed it, and
   that close is what raised the abend. */
static int pww_write_member_guarded(pending_member_t *pm,
                                    const char *open_target,
                                    int32_t *lines_out)
{
    jmp_buf      env;
    unsigned int sdwa[26];      /* 104-byte SDWA copy (IHASDWA) */
    long         rc;
    unsigned int code;
    int          err;

    g_flush_abended = 0;

    rc = _setjmp_stae(env, (unsigned char *)sdwa);
    if (rc == 0) {                          /* armed -- do the work */
        int frc = pww_write_member(pm, open_target, lines_out);
        if (_setjmp_canc() != 0)
            log_warn("pww_flush_slot: _setjmp_canc failed");
        return frc;
    }

    if (rc == 1) {                          /* an abend was intercepted */
        code = PWW_ABEND_CODE(sdwa);
        err  = pww_abend_errno(code);
        log_error("pww_flush_slot: %s(%s) ABENDED S%03X%s -- member NOT written",
                  pm->dsname_ebcdic, pm->member_name, code,
                  (err == ENOSPC) ? " (dataset out of space)" : "");
        g_flush_fp      = NULL;             /* runtime already closed it */
        g_flush_abended = 1;
        errno = err;                        /* set last: logging clobbers it */
        return -1;
    }

    /* The STAE could not be established.  Refusing to flush would guarantee
       data loss, whereas this is rare and usually means the system is already
       in trouble -- so proceed, but say so loudly (this reaches the console). */
    log_error("pww_flush_slot: STAE not established (rc=%ld) -- writing"
              " %s(%s) UNPROTECTED", rc, pm->dsname_ebcdic, pm->member_name);
    return pww_write_member(pm, open_target, lines_out);
}

/* Apply the ISPF statistics to the just-stowed member: allocate the PDS
   (FREE=CLOSE, dataset level -- BLDL/FIND/STOW operate on the directory, so no
   member qualifier) and STOW REPLACE the directory user data.
   Returns 0 on success, non-zero on failure. */
static int pww_apply_stats(pending_member_t *pm, uint8_t *stats_ud)
{
    char ddname[9];
    int  rc;

    ddname[0] = '\0';
    rc = mvs_dynalloc(MVS_DYNALLOC_REQ_ALLOC, MVS_DYNALLOC_OPT_FREECLOSE,
                      pm->dsname_ebcdic, NULL, ddname);
    if (rc != 0) {
        log_debug("pww_flush_slot: mvs_dynalloc failed for %s (rc=%d)",
                  pm->dsname_ebcdic, rc);
        return rc;
    }
    ddname[8] = '\0';       /* dynalloc returns 8 blank-padded chars */
    return mvs_stow(ddname, pm->member_name,
                    stats_ud, (int)MVS_ISPF_STATS_LEN);
}

/* Region B of the flush (design_nfs_write.md Sec 7.3): run the stats update
   under its OWN STAE, separate from region A, because it fails with a
   different meaning.

   Adding the 30-byte user data makes the directory entry longer, so a PDS that
   has run out of DIRECTORY blocks can fail here even though the member itself
   was written and stowed perfectly -- by abend, or by a non-zero mvs_stow
   return code (a directory-full can surface either way, so both are treated
   alike).

   Returns 0 if the stats were applied, non-zero if not.  A non-zero result must
   NOT fail the flush: the member content is already safe on disk, and returning
   ENOSPC for a write that actually worked would be a false negative. */
static int pww_apply_stats_guarded(pending_member_t *pm, uint8_t *stats_ud)
{
    jmp_buf      env;
    unsigned int sdwa[26];      /* 104-byte SDWA copy (IHASDWA) */
    long         rc;

    g_stats_abend = 0;

    rc = _setjmp_stae(env, (unsigned char *)sdwa);
    if (rc == 0) {                          /* armed -- do the work */
        int src = pww_apply_stats(pm, stats_ud);
        if (_setjmp_canc() != 0)
            log_warn("pww_flush_slot: _setjmp_canc failed (stats)");
        return src;
    }

    if (rc == 1) {                          /* an abend was intercepted */
        g_stats_abend = (int)PWW_ABEND_CODE(sdwa);
        return -1;                          /* caller warns; flush still OK */
    }

    log_warn("pww_flush_slot: STAE not established (rc=%ld) -- applying stats"
             " to %s(%s) UNPROTECTED", rc, pm->dsname_ebcdic, pm->member_name);
    return pww_apply_stats(pm, stats_ud);
}
#endif /* __MVS__ */

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
    if (pww_write_member_guarded(pm, path, &line_count) != 0) {  /* write + STOW */
        if (g_flush_abended) {
            /* Sec 7.3 region A: an abend (typically the dataset filling) is
               not worth retrying -- a retry just abends again.  Release the
               slot NOW so the DISP=SHR allocation and, above all, the SPFEDIT
               enqueue are dropped: a retained enqueue would lock the member
               against ISPF for the life of the task.  Preserve errno across
               the cleanup so the caller still reports ENOSPC. */
            int saved = errno;
            pww_slot_release_guarded(pm);
            errno = saved;
        }
        return -1;
    }
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
        int rc;

        mvs_encode_ispf_stats(&new_stats, stats_ud);
        rc = pww_apply_stats_guarded(pm, stats_ud);

        /* Sec 7.3 region B: a stats failure does NOT fail the flush.  The
           member content is written and stowed; only its statistics are
           missing, and returning an error here would report ENOSPC for a write
           that actually succeeded.  The buffered data is not retried either --
           it is already on disk.  This also degrades into a path the read side
           already handles: an entry with no user data falls back to
           mvs_set_no_ispf_stats(), and mvs_build_write_stats() will not ask for
           stats on that member again, so the condition settles instead of
           re-abending on every write.

           Two suppression flags, so a serious directory-full abend can never be
           masked by an earlier benign return-code warning. */
        if (rc != 0) {
            static int warned_abend = 0;
            static int warned_rc    = 0;

            if (g_stats_abend != 0) {
                if (!warned_abend) {
                    warned_abend = 1;
                    log_warn("pww_flush_slot: ISPF stats update ABENDED S%03X"
                             " for %s(%s) -- PDS directory may be full; member"
                             " IS stowed but without ISPF stats"
                             " (further warnings suppressed)",
                             (unsigned)g_stats_abend,
                             pm->dsname_ebcdic, pm->member_name);
                }
            } else if (!warned_rc) {
                warned_rc = 1;
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
