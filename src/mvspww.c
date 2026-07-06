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
#include "mvsdol.h"
#include "ebcdic.h"
#include "logger.h"

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

/* Build the "//DSN:dsname(member)" open path. */
static void pww_member_path(char *out, const char *dsname_ebcdic,
                            const char *member_name)
{
    strcpy(out, "//DSN:");
    strcat(out, dsname_ebcdic);
    strcat(out, "(");
    strcat(out, member_name);
    strcat(out, ")");
}

/* Write the buffered member out in one pass and STOW it (fclose).
   The ASCII buffer is left intact (translated via g_pww_xlate in chunks).
   Returns 0 on success, -1 on failure (errno set by stdio). */
static int pww_flush_slot(pending_member_t *pm)
{
    FILE    *fh;
    char     path[6 + 44 + 1 + 8 + 1 + 1];
    uint32_t off;
    uint32_t n;
    size_t   w;

    pww_member_path(path, pm->dsname_ebcdic, pm->member_name);

    fh = fopen(path, "wt");        /* text mode: record per '\n', STOW on close */
    if (fh == NULL) {
        log_error("pww_flush_slot: fopen %s failed: %s", path, strerror(errno));
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
                path, (unsigned)w, n);
            fclose(fh);
            return -1;
        }
    }

    if (fclose(fh) != 0) {         /* STOW happens here */
        log_error("pww_flush_slot: fclose(STOW) %s failed: %s",
            path, strerror(errno));
        return -1;
    }

    pm->dirty = 0;
    log_info("pww_flush_slot: stowed %s(%s), %u bytes",
        pm->dsname_ebcdic, pm->member_name, pm->high_water);

    /* The PDS directory just changed: bump its mtime so clients invalidate
       their cached listing, and drop our own cached listing so the next
       readdir re-reads the directory and includes the new/replaced member. */
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
