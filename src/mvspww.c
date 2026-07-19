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

/* -------------------------------------------------------------------- */
/* Init                                                                  */
/* -------------------------------------------------------------------- */
void pww_init(void)
{
    memset(g_pww_pool, 0, sizeof(g_pww_pool));
}

/* -------------------------------------------------------------------- */
/* Slot helpers                                                          */
/* -------------------------------------------------------------------- */

static void pww_release_slot(pending_member_t *pm)
{
    if (pm->buf != NULL)
        free(pm->buf);
    memset(pm, 0, sizeof(*pm));
    pm->status = PWW_STATUS_FREE;
}

/* Find the USED slot for (dsname, member), or NULL. */
static pending_member_t *pww_find_slot(const char *dsname_ebcdic,
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

/* Open 'open_target' ("//DDN:ddname" on MVS, "//DSN:dsname(member)"
   elsewhere), write the pending member's buffer as text records, and STOW it
   at close.  The ASCII buffer is left intact (translated via g_pww_xlate in
   chunks).  Returns 0 on success, -1 on failure (errno set by stdio).
   The caller owns any dynamic allocation and frees it regardless of the
   result -- keeping that cleanup in one place is why this is split out. */
static int pww_write_member(pending_member_t *pm, const char *open_target)
{
    FILE    *fh;
    uint32_t off;
    uint32_t n;
    size_t   w;

    fh = fopen(open_target, "wt");    /* text mode: record per '\n', STOW on close */
    if (fh == NULL) {
        log_error("pww_flush_slot: fopen %s failed: %s",
                  open_target, strerror(errno));
        return -1;
    }

    for (off = 0; off < pm->high_water; off += n) {
        n = pm->high_water - off;
        if (n > sizeof(g_pww_xlate))
            n = sizeof(g_pww_xlate);
        ascii_to_ebcdic(g_pww_xlate, pm->buf + off, (size_t)n);
        w = fwrite(g_pww_xlate, 1, (size_t)n, fh);
        if (w != (size_t)n) {
            log_error("pww_flush_slot: short write on %s (%u of %u)",
                      open_target, (unsigned)w, n);
            fclose(fh);
            return -1;
        }
    }

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
#ifdef __MVS__
    char     ddname[9];
    char     rname[44 + 8 + 1];   /* dsname(44) + member(8) + NUL */
    int      k;
    int      wrc;
    int      enqrc;
#endif

    /* Read the member's current directory entry (if any) BEFORE opening it
       for output, so we never have the PDS open for input and output at once.
       NULL => the member does not yet exist (a new member). */
    existing = mvs_pds_get_member_entry(pm->dsname_ebcdic, pm->member_name,
                                        pm->export_idx, &existing_ent);

    /* Decide the ISPF stats to STOW (count records from the ASCII buffer). */
    line_count = mvs_ispf_count_lines(pm->buf, pm->high_water);
    want_stats = mvs_build_write_stats(&new_stats, existing, line_count,
                                       time(NULL));

#ifdef __MVS__
    /* Step 1: serialise with ISPF/EDIT.  Take an EXCLUSIVE enqueue on the
       resource an editor uses -- QNAME "SPFEDIT", RNAME = dsname(44) +
       member(8), blank-padded.  ENQ RET=USE (see mvsenq.asm) fails fast
       rather than waiting, so a member open in an editor does NOT hang the
       single-threaded server.

       If we cannot get it, the member is being edited elsewhere: fail the
       flush -- the same outcome the replaced DISP=OLD open produced when the
       dataset was held.  (A later change moves the ENQ + allocation to the
       FIRST WRITE, so the conflict surfaces to the client at WRITE -- which
       every client issues -- rather than at flush, where a Linux client that
       never COMMITs would never see it.) */

    log_debug("pww_flush_slot: Constructing SPFEDIT ENQ RNAME");
    sprintf(rname, "%-44.44s%-8.8s", pm->dsname_ebcdic, pm->member_name);
    log_debug("pww_flush_slot: SPFEDIT ENQ RNAME=%s (len=%d)", rname, strlen(rname));

    enqrc = mvs_enq(
        MVS_ENQ_REQ_ENQ, MVS_ENQ_OPT_EXC, 
        "SPFEDIT", rname );
        
    if ( enqrc != 0) {
        log_warn("pww_flush_slot: %s(%s) is held (SPFEDIT enqueue) --"
                 " flush failed", pm->dsname_ebcdic, pm->member_name);
        errno = EIO;
        return -1;
    } else {
        log_debug("pww_flush_slot: %s(%s) is held (SPFEDIT enqueue)",
                 pm->dsname_ebcdic, pm->member_name);
    }

    /* Step 2: allocate DSN(MEMBER) DISP=SHR (mvs_dynalloc hard-codes SHR) and
       write the member through the returned ddname -- shares the PDS with
       ISPF/TSO, unlike JCC's "//DSN:" (DISP=OLD).  options = 0: NO FREE=CLOSE,
       so we unallocate explicitly below. */
    ddname[0] = '\0';
    if (mvs_dynalloc(MVS_DYNALLOC_REQ_ALLOC, 0,
                     pm->dsname_ebcdic, pm->member_name, ddname) != 0) {
        log_error("pww_flush_slot: dynalloc %s(%s) failed",
                  pm->dsname_ebcdic, pm->member_name);
        errno = EIO;
        wrc   = -1;
    } else {
        /* The ddname is 8 blank-padded chars, not NUL-terminated; trim the
           pad before building "//DDN:ddname" (a trailing blank breaks it). */
        ddname[8] = '\0';
        for (k = 8; k > 0 && ddname[k - 1] == ' '; k--)
            ddname[k - 1] = '\0';
        strcpy(path, "//DDN:");
        strcat(path, ddname);

        wrc = pww_write_member(pm, path);   /* steps 3-5: write + STOW */

        /* Step 6: unallocate on success AND failure (no FREE=CLOSE).  The
           member name MUST be supplied -- unallocating by dataset name alone
           does not release a DSN(MEMBER) allocation (confirmed via the SYSDSN
           enqueues in IMON/370). */
        if (mvs_dynalloc(MVS_DYNALLOC_REQ_UNALLOC, 0,
                         pm->dsname_ebcdic, pm->member_name, ddname) != 0)
            log_warn("pww_flush_slot: unalloc %s(%s) failed",
                     pm->dsname_ebcdic, pm->member_name);
    }

    if (wrc != 0) {
        /* Release the enqueue before failing; preserve the write's errno. */
        int saved_errno = errno;
        (void)mvs_enq(MVS_ENQ_REQ_DEQ, MVS_ENQ_OPT_EXC, "SPFEDIT", rname);
        errno = saved_errno;
        return -1;
    }
    /* Success: the SPFEDIT enqueue stays held across the stats STOW too and
       is released at step 8 below. */
#else
    pww_member_path(path, pm->dsname_ebcdic, pm->member_name);
    if (pww_write_member(pm, path) != 0)
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

    /* Step 8: release the SPFEDIT enqueue held across the write and stats.
       DEQ RET=HAVE (see mvsenq.asm) is safe even if we somehow no longer hold
       it -- it returns non-zero rather than abending. */
    if (mvs_enq(MVS_ENQ_REQ_DEQ, MVS_ENQ_OPT_EXC, "SPFEDIT", rname) != 0) {
        log_warn("pww_flush_slot: DEQ %s(%s) failed",
                 pm->dsname_ebcdic, pm->member_name);
    } else {
        log_debug("pww_flush_slot: DEQ %s(%s) successful",
                 pm->dsname_ebcdic, pm->member_name);

    } 

#else
    (void)want_stats;
    (void)new_stats;
    (void)stats_ud;
#endif

    /* Step 9: the PDS directory just changed: bump its mtime so clients
       invalidate their cached listing, and drop our own cached listing so the
       next readdir re-reads the directory and includes the new member. */
    export_dataset_touch(pm->export_idx, pm->dataset_idx);
    dir_openlist_invalidate(pm->dsname_ebcdic);

    return 0;
}

/* Obtain a slot to use for (dsname, member): the existing one if present,
   otherwise a FREE slot, otherwise evict (flush if dirty) the least
   recently used slot.  Never returns NULL. */
static pending_member_t *pww_acquire_slot(const char *dsname_ebcdic,
                                          const char *member_name)
{
    pending_member_t *pm;
    int i;
    int lru;

    pm = pww_find_slot(dsname_ebcdic, member_name);
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
    log_warn("pww_acquire_slot: pool full, evicting %s(%s)",
        g_pww_pool[lru].dsname_ebcdic, g_pww_pool[lru].member_name);
    if (g_pww_pool[lru].dirty)
        (void)pww_flush_slot(&g_pww_pool[lru]);
    pww_release_slot(&g_pww_pool[lru]);
    return &g_pww_pool[lru];
}

/* Initialise a freshly-acquired slot for (export, dataset, dsname, member). */
static void pww_init_slot(pending_member_t *pm, int export_idx, int dataset_idx,
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

/* Ensure pm->buf has capacity for at least 'need' bytes (<= cap limit). */
static int pww_ensure_cap(pending_member_t *pm, uint32_t need)
{
    uint32_t new_cap;
    uint8_t *new_buf;

    if (need > (uint32_t)PWW_MAX_MEMBER_BYTES) {
        errno = ENOSPC;   /* JCC has no EFBIG; NOSPC maps to NFS3ERR_NOSPC */
        return -1;
    }
    if (need <= pm->buf_cap)
        return 0;

    new_cap = (pm->buf_cap != 0) ? pm->buf_cap : 65536u;
    while (new_cap < need)
        new_cap *= 2u;
    if (new_cap > (uint32_t)PWW_MAX_MEMBER_BYTES)
        new_cap = (uint32_t)PWW_MAX_MEMBER_BYTES;

    new_buf = (uint8_t *)realloc(pm->buf, (size_t)new_cap);
    if (new_buf == NULL) {
        log_error("pww_ensure_cap: realloc to %u bytes failed", new_cap);
        errno = ENOMEM;
        return -1;
    }
    pm->buf     = new_buf;
    pm->buf_cap = new_cap;
    return 0;
}

/* -------------------------------------------------------------------- */
/* Public API                                                            */
/* -------------------------------------------------------------------- */

int pww_create(int export_idx, int dataset_idx,
               const char *dsname_ebcdic, const char *member_name)
{
    pending_member_t *pm;

    pm = pww_acquire_slot(dsname_ebcdic, member_name);
    if (pm->status != PWW_STATUS_USED ||
        strcmp(pm->dsname_ebcdic, dsname_ebcdic) != 0 ||
        strcmp(pm->member_name, member_name) != 0) {
        /* Fresh or reused slot: (re)initialise it. */
        pww_init_slot(pm, export_idx, dataset_idx, dsname_ebcdic, member_name);
    } else {
        /* Re-create over an existing pending member: truncate contents. */
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

    pm = pww_acquire_slot(dsname_ebcdic, member_name);
    if (pm->status != PWW_STATUS_USED ||
        strcmp(pm->dsname_ebcdic, dsname_ebcdic) != 0 ||
        strcmp(pm->member_name, member_name) != 0) {
        pww_init_slot(pm, export_idx, dataset_idx, dsname_ebcdic, member_name);
    }

    if (pww_ensure_cap(pm, (uint32_t)end) < 0)
        return -1;    /* errno set (ENOSPC/ENOMEM) */

    /* Zero-fill any gap between the current end and this write's offset. */
    if (offset > (uint64_t)pm->high_water)
        memset(pm->buf + pm->high_water, 0,
               (size_t)(offset - (uint64_t)pm->high_water));

    if (count > 0)
        memcpy(pm->buf + offset, data, (size_t)count);

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

    pm = pww_find_slot(dsname_ebcdic, member_name);

    if (pm == NULL) {
        /* No pending member.  A truncate-to-empty (the O_TRUNC case) starts a
           fresh empty member so COMMIT replaces the on-disk member with the
           new content.  A truncate to a non-zero size of an on-disk-only
           member is not supported in Phase 1; accept it as a no-op so the
           client is not blocked (see doc/design_nfs_write.md). */
        if (size != 0)
            return 0;
        pm = pww_acquire_slot(dsname_ebcdic, member_name);
        pww_init_slot(pm, export_idx, dataset_idx, dsname_ebcdic, member_name);
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

    /* Adjust the existing pending member to exactly 'size' bytes. */
    if (size > pm->buf_cap) {
        if (pww_ensure_cap(pm, size) < 0)
            return -1;
    }
    if (size > pm->high_water)
        memset(pm->buf + pm->high_water, 0, (size_t)(size - pm->high_water));

    pm->high_water      = size;
    pm->dirty           = 1;
    pm->last_write_time = time(NULL);
    log_debug("pww_truncate: %s(%s) -> %u", dsname_ebcdic, member_name, size);
    return 0;
}

pending_member_t *pww_find(const char *dsname_ebcdic, const char *member_name)
{
    return pww_find_slot(dsname_ebcdic, member_name);
}

int pww_discard(const char *dsname_ebcdic, const char *member_name)
{
    pending_member_t *pm = pww_find_slot(dsname_ebcdic, member_name);
    if (pm == NULL)
        return 0;                  /* nothing buffered for this member */
    /* Drop the buffer WITHOUT flushing -- the caller (REMOVE) is deleting the
       member, so it must not be re-STOWed by a later flush. */
    pww_release_slot(pm);
    return 1;
}

int pww_flush_member(const char *dsname_ebcdic, const char *member_name)
{
    pending_member_t *pm;

    pm = pww_find_slot(dsname_ebcdic, member_name);
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
        pww_release_slot(pm);
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
        pww_release_slot(pm);
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
    int                 rc;

    /* If content is still pending (dirty), the upcoming flush will set the
       changed date -- skip, or this STOW would just be overwritten. */
    pm = pww_find_slot(dsname_ebcdic, member_name);
    if (pm != NULL && pm->dirty)
        return 0;

    ep = mvs_pds_get_member_entry(dsname_ebcdic, member_name, export_idx, &entry);
    if (ep == NULL)
        return 0;                       /* not found -- nothing to update */
    if (!(entry.info_flags & MVS_PDSDIR_IFLG_ISPFSTATS))
        return 0;                       /* no ISPF stats -- do not fabricate */
    if (entry.chgdate == (int32_t)new_time)
        return 0;                       /* already that time (1s resolution) */

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
    return 0;
#else
    (void)export_idx; (void)dsname_ebcdic; (void)member_name; (void)new_time;
    return 0;
#endif
}
