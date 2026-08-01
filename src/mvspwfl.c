/*
 * mvspwfl.c - Flush machinery for the pending-member write pool.
 *
 * Takes a buffered member from mvspww.c and puts it in the PDS: opens the
 * held allocation, writes the byte stream out as records (ASCII->EBCDIC),
 * STOWs it at close, applies the ISPF statistics, and invalidates the
 * caches that the directory change has just made stale.
 *
 * Split out of mvspww.c because it is the part that ABENDS.  Writing a PDS
 * member fails hard when the dataset fills, so every PDS-mutating step here
 * runs under a STAE (design_nfs_write.md Sec 7.3) -- and a guard belongs in
 * the same module as the thing it guards.  The out-of-space memory lives
 * here for the same reason: the flush is what discovers a dataset is full
 * and what discovers it has room again, so it owns that table and the write
 * path only consults it.
 *
 * The interface is deliberately tiny -- see mvspwfl.h.  Everything else is
 * static.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <setjmp.h>      /* jmp_buf                                          */
#include <mvsutils.h>    /* _setjmp_stae / _setjmp_canc -- STAE abend trap   */

#include "nfsd.h"
#include "mvspww.h"      /* pending_member_t, pww_read_range()               */
#include "mvspwfl.h"
#include "mvspdir.h"
#include "mvsdol.h"
#include "ebcdic.h"
#include "logger.h"
#include "mvsutl.h"      /* SDWA_ABEND_CODE()                                */
#include "asmutils.h"    /* MVS assembler helpers: mvs_dynalloc(), mvs_stow() */

/* Small reusable scratch for ASCII->EBCDIC translation during flush, so we
   never modify the (ASCII) member buffer -- it must survive across a COMMIT
   in case the client writes more before the slot is released. */
static uint8_t g_pww_xlate[4096];
/* -------------------------------------------------------------------- */
/* "Dataset is out of space" memory (design_nfs_write.md Sec 7.3)        */
/*                                                                       */
/* A full PDS abends on EVERY flush, and each abend is expensive: D37 +  */
/* B14 + STAE recovery + several console messages.  Without this, one    */
/* client copying a directory into a full dataset produces one abend per */
/* file (measured: 300 files -> ~600 abends, minutes of console spam).   */
/*                                                                       */
/* So once a flush has abended out-of-space we remember that dataset and */
/* refuse further writes to it up front.  That also closes the reporting */
/* gap for this case: CREATE and WRITE are SYNCHRONOUS, so the client    */
/* actually sees the ENOSPC instead of losing data silently the way an   */
/* idle-sweep flush failure does.                                        */
/*                                                                       */
/* The memory expires so the server recovers by itself once an operator  */
/* adds space, and any successful flush clears it immediately.           */
/*                                                                       */
/* "Full" here always means THE DATASET is out of space -- never the     */
/* slot pool, which is a separate concern owned by mvspww.c.             */
/* -------------------------------------------------------------------- */
#define PWW_FULL_REMEMBER    4     /* datasets remembered as full       */
#define PWW_FULL_EXPIRY_SEC  60    /* forget after this many seconds    */

static struct {
    char   dsname[MAX_DSNAME_LEN];
    time_t when;                   /* 0 = entry unused */
} g_pww_full[PWW_FULL_REMEMBER];

/* Remember that 'dsname' is out of space (refresh if already known; else take
   the oldest / an unused entry). */
static void pdsflush_dataset_mark_full(const char *dsname, time_t now)
{
    int i;
    int oldest = 0;

    for (i = 0; i < PWW_FULL_REMEMBER; i++) {
        if (g_pww_full[i].when != 0 &&
            strcmp(g_pww_full[i].dsname, dsname) == 0) {
            g_pww_full[i].when = now;
            return;
        }
        if (g_pww_full[i].when < g_pww_full[oldest].when)
            oldest = i;
    }
    strncpy(g_pww_full[oldest].dsname, dsname, MAX_DSNAME_LEN - 1);
    g_pww_full[oldest].dsname[MAX_DSNAME_LEN - 1] = '\0';
    g_pww_full[oldest].when = now;
}

/* Forget 'dsname' -- a flush to it has just succeeded, so it has room again. */
static void pdsflush_dataset_clear_full(const char *dsname)
{
    int i;
    for (i = 0; i < PWW_FULL_REMEMBER; i++) {
        if (g_pww_full[i].when != 0 &&
            strcmp(g_pww_full[i].dsname, dsname) == 0)
            g_pww_full[i].when = 0;
    }
}

/* Is 'dsname' known to be out of space?  Expired entries are dropped here, so
   the table needs no separate sweep. */
int pdsflush_dataset_is_full(const char *dsname, time_t now)
{
    int i;
    for (i = 0; i < PWW_FULL_REMEMBER; i++) {
        if (g_pww_full[i].when == 0)
            continue;
        if (now - g_pww_full[i].when > PWW_FULL_EXPIRY_SEC) {
            g_pww_full[i].when = 0;
            continue;
        }
        if (strcmp(g_pww_full[i].dsname, dsname) == 0)
            return 1;
    }
    return 0;
}
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

/* The member stream, reachable from the recovery path: the abend unwinds out
   of pdsflush_write_member, so its local FILE * is gone by the time we regain
   control.  The server is single-threaded and flushes one slot at a time, so a
   single module-scope handle is sufficient.  Set on open, cleared on close or
   recovery -- and NEVER closed by the recovery path (the runtime already did
   that, which is what raised the abend). */
static FILE *g_flush_fp = NULL;

/* Set by the region-A guard when the member write abended, so pdsflush_slot
   can tell a genuine abend from an ordinary I/O failure and release the slot
   instead of leaving it to be retried. */
static int g_flush_abended = 0;

/* Set by the region-B guard to the completion code when the ISPF-stats STOW
   abended (0 = it did not), so the warning can name it. */
static int g_stats_abend = 0;

/* Is this an abend we deliberately trap and recover from?
 *
 * ONLY the out-of-space family.  These are environmental: the dataset filled
 * up, which is outside our control and must not take the server down.  B14 is
 * the visible face of an out-of-space condition (see above) and is also how a
 * directory-full STOW surfaces, so it belongs here too.  All map to ENOSPC ->
 * NFS3ERR_NOSPC, which is the diagnosis the client should see.
 *
 * Everything else -- S0C4, S0C1, ... -- means a PROGRAM ERROR, and recovering
 * from it would be actively harmful: it would turn a deterministic crash into a
 * silent, repeating "I/O error" and hide the bug.  Those are reported loudly
 * and set the fatal flag instead (see pww_fatal_abend). */
static int pdsflush_abend_recoverable(unsigned int code)
{
    switch (code) {
    case 0xB14:                                    /* CLOSE/STOW failed     */
    case 0xB37: case 0xD37: case 0xE37:            /* out of space/extents  */
        return 1;
    default:
        return 0;
    }
}

/* Publish / clear the in-flight member stream for the recovery path. */
#define PWW_FLUSH_FP(f)   (g_flush_fp = (f))
/* Open 'open_target' ("//DDN:ddname" on MVS, "//DSN:dsname(member)"
   elsewhere), write the pending member's byte stream as text records, and STOW
   it at close.  The content is read in chunks via pww_read_range (memory or
   spill) into g_pww_xlate and translated ASCII->EBCDIC in place, so neither the
   in-memory buffer nor the spill file is disturbed.  Returns 0 on success, -1
   on failure (errno set).  The caller owns any dynamic allocation and frees it
   regardless of the result -- keeping that cleanup in one place is why this is
   split out. */
static int pdsflush_write_member(pending_member_t *pm, const char *open_target,
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
        log_error("pdsflush_slot: fopen %s failed: %s",
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
            log_error("pdsflush_slot: read-back failed on %s at %u",
                      open_target, off);
            fclose(fh);
            PWW_FLUSH_FP(NULL);
            return -1;
        }
        /* Tripwire: the chunk about to be translated and written must not
           contain an RPC message.  A hit HERE with no matching "CORRUPT
           PAYLOAD ON ARRIVAL" from pww_write means the data was clean when
           stored and was corrupted inside the pool -- which rules out the
           whole receive path.  Log both the in-chunk offset and the member
           offset, to confirm the "60 bytes into a chunk" phase. */
        {
            int rpc_at = pww_find_rpc_header(g_pww_xlate, n);
            if (rpc_at >= 0)
                log_error("pdsflush_slot: CORRUPT CHUNK -- RPC CALL header at"
                          " in-chunk offset %d (member offset %u) flushing %s"
                          " (chunk off=%u len=%u)",
                          rpc_at, off + (uint32_t)rpc_at, open_target, off, n);
        }

        for (i = 0; i < n; i++)
            if (g_pww_xlate[i] == 0x0A)   /* ASCII LF -- the stream is ASCII */
                lines++;
        last = g_pww_xlate[n - 1];
        ascii_to_ebcdic(g_pww_xlate, g_pww_xlate, (size_t)n);   /* in place */
        w = fwrite(g_pww_xlate, 1, (size_t)n, fh);
        if (w != (size_t)n) {
            log_error("pdsflush_slot: short write on %s (%u of %u)",
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
        log_error("pdsflush_slot: fclose(STOW) %s failed: %s",
                  open_target, strerror(errno));
        return -1;
    }
    PWW_FLUSH_FP(NULL);
    return 0;
}

/* Region A of the flush (design_nfs_write.md Sec 7.3): run pdsflush_write_member
   under a STAE so an out-of-space abend fails THIS request instead of
   terminating the NFSD task.

   On an abend the member content is lost or partial -- a real failure -- so we
   set errno (ENOSPC for the out-of-space family) and return -1; the caller
   releases the slot rather than retrying, because a retry would only abend
   again.  We do NOT close the stream here: the runtime already closed it, and
   that close is what raised the abend. */
static int pdsflush_write_member_guarded(pending_member_t *pm,
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
        int frc = pdsflush_write_member(pm, open_target, lines_out);
        if (_setjmp_canc() != 0)
            log_warn("pdsflush_slot: _setjmp_canc failed");
        return frc;
    }

    if (rc == 1) {                          /* an abend was intercepted */
        code = SDWA_ABEND_CODE(sdwa);
        if (pdsflush_abend_recoverable(code)) {
            /* Expected: the dataset filled up.  Remember it, so the next write
               is refused up front instead of costing another abend -- and so
               the client gets a synchronous ENOSPC it can actually see. */
            pdsflush_dataset_mark_full(pm->dsname_ebcdic, time(NULL));
            log_error("pdsflush_slot: %s(%s) ABENDED S%03X (dataset out of"
                      " space) -- member NOT written; refusing writes to this"
                      " dataset for %d seconds",
                      pm->dsname_ebcdic, pm->member_name, code,
                      PWW_FULL_EXPIRY_SEC);
            err = ENOSPC;
        } else {
            /* NOT an out-of-space condition, so almost certainly a program
               error (bad pointer, overrun, ...).  Do not dress it up as an I/O
               error and carry on -- that would hide the bug and keep serving
               from state we no longer trust.  Say so unmistakably and ask for
               an orderly shutdown. */
            log_error("pdsflush_slot: %s(%s) ABENDED S%03X -- UNEXPECTED abend"
                      " (probable PROGRAM ERROR, not out of space);"
                      " requesting server shutdown",
                      pm->dsname_ebcdic, pm->member_name, code);
            pww_request_shutdown();
            err = EIO;
        }
        g_flush_fp      = NULL;             /* runtime already closed it */
        g_flush_abended = 1;
        errno = err;                        /* set last: logging clobbers it */
        return -1;
    }

    /* The STAE could not be established.  Refusing to flush would guarantee
       data loss, whereas this is rare and usually means the system is already
       in trouble -- so proceed, but say so loudly (this reaches the console). */
    log_error("pdsflush_slot: STAE not established (rc=%ld) -- writing"
              " %s(%s) UNPROTECTED", rc, pm->dsname_ebcdic, pm->member_name);
    return pdsflush_write_member(pm, open_target, lines_out);
}

/* Apply the ISPF statistics to the just-stowed member: allocate the PDS
   (FREE=CLOSE, dataset level -- BLDL/FIND/STOW operate on the directory, so no
   member qualifier) and STOW REPLACE the directory user data.
   Returns 0 on success, non-zero on failure. */
static int pdsflush_apply_stats(pending_member_t *pm, uint8_t *stats_ud)
{
    char ddname[9];
    int  rc;

    ddname[0] = '\0';
    rc = mvs_dynalloc(MVS_DYNALLOC_REQ_ALLOC, MVS_DYNALLOC_OPT_FREECLOSE,
                      pm->dsname_ebcdic, NULL, ddname);
    if (rc != 0) {
        log_debug("pdsflush_slot: mvs_dynalloc failed for %s (rc=%d)",
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
static int pdsflush_apply_stats_guarded(pending_member_t *pm, uint8_t *stats_ud)
{
    jmp_buf      env;
    unsigned int sdwa[26];      /* 104-byte SDWA copy (IHASDWA) */
    long         rc;

    g_stats_abend = 0;

    rc = _setjmp_stae(env, (unsigned char *)sdwa);
    if (rc == 0) {                          /* armed -- do the work */
        int src = pdsflush_apply_stats(pm, stats_ud);
        if (_setjmp_canc() != 0)
            log_warn("pdsflush_slot: _setjmp_canc failed (stats)");
        return src;
    }

    if (rc == 1) {                          /* an abend was intercepted */
        unsigned int code = SDWA_ABEND_CODE(sdwa);
        g_stats_abend = (int)code;
        if (!pdsflush_abend_recoverable(code)) {
            /* Same rule as region A: only the out-of-space family is expected
               here (a full PDS directory).  Anything else is a program error. */
            log_error("pdsflush_slot: stats update for %s(%s) ABENDED S%03X --"
                      " UNEXPECTED abend (probable PROGRAM ERROR);"
                      " requesting server shutdown",
                      pm->dsname_ebcdic, pm->member_name, code);
            pww_request_shutdown();
        }
        return -1;                          /* caller warns; flush still OK */
    }

    log_warn("pdsflush_slot: STAE not established (rc=%ld) -- applying stats"
             " to %s(%s) UNPROTECTED", rc, pm->dsname_ebcdic, pm->member_name);
    return pdsflush_apply_stats(pm, stats_ud);
}

/* Write the buffered member out in one pass and STOW it.
   Returns 0 on success, -1 on failure (errno set). */
int pdsflush_slot(pending_member_t *pm)
{
    char     path[6 + 44 + 1 + 8 + 1 + 1];

    pds_member_entry_t  existing_ent;
    pds_member_entry_t *existing;
    pds_member_entry_t  new_stats;
    uint8_t             stats_ud[MVS_ISPF_STATS_LEN];
    int                 want_stats;
    int32_t             line_count;

    /* Never flush into a dataset a previous flush abended out of space on:
       the SECOND abend deadlocks the task in the JCC runtime's lock (design
       Sec 7.3).  The content is lost either way -- the flush this replaces
       would have abended -- but the server survives. */
    if (pdsflush_dataset_is_full(pm->dsname_ebcdic, time(NULL))) {
        log_error("pdsflush_slot: %s(%s) NOT flushed -- %s is already known"
                  " out of space; content discarded",
                  pm->dsname_ebcdic, pm->member_name, pm->dsname_ebcdic);
        pm->dirty = 0;    /* as the abend path below: nothing retryable */
        errno = ENOSPC;
        return -1;
    }

    log_info("pdsflush_slot: Starting flush for %s(%s)",
        pm->dsname_ebcdic, pm->member_name);

    /* Read the member's current directory entry (if any) BEFORE opening it
       for output, so we never have the PDS open for input and output at once.
       NULL => the member does not yet exist (a new member). */
    existing = mvs_pds_get_member_entry(pm->dsname_ebcdic, pm->member_name,
                                        pm->export_idx, &existing_ent);

    /* The SPFEDIT enqueue and the DSN(member) DISP=SHR allocation were taken at
       CREATE / first WRITE (pww_lock) and are held for the slot's whole
       lifetime, so the flush neither enqueues nor allocates -- it simply opens
       the held allocation by its ddname ("//DDN:ddname") and writes + STOWs
       through it (fclose STOWs).  The enqueue and allocation are released only
       when the slot is released (mvspww.c: pww_slot_release), which
       also covers the ISPF-stats STOW below.  pdsflush_write_member also returns the
       record count (single read pass) for the ISPF stats applied afterwards. */
    strcpy(path, "//DDN:");
    strcat(path, pm->ddname);
    if (pdsflush_write_member_guarded(pm, path, &line_count) != 0) {  /* write + STOW */
        if (g_flush_abended) {
            /* Sec 7.3 region A: an abend (typically the dataset filling) is not
               worth retrying -- a retry just abends again -- so mark the slot
               clean and let it go on the next sweep.
               Do NOT release the slot here: the CALLER owns release
               (mvspww.c: pww_flush_idle / pww_flush_all already do it, under
               its STAE).  Releasing it here zeroed the slot underneath
               the caller, which then logged an empty "flush failed for ()" and
               released it a second time. */
            pm->dirty = 0;
        }
        return -1;
    }

    pm->dirty = 0;
    /* The dataset clearly has room, so drop any "out of space" memory of it
       (Sec 7.3): recovery is immediate once an operator adds space. */
    pdsflush_dataset_clear_full(pm->dsname_ebcdic);
    log_info("pdsflush_slot: stowed %s(%s), %u bytes",
        pm->dsname_ebcdic, pm->member_name, pm->high_water);

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
        rc = pdsflush_apply_stats_guarded(pm, stats_ud);

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
                    log_warn("pdsflush_slot: ISPF stats update ABENDED S%03X"
                             " for %s(%s) -- PDS directory may be full; member"
                             " IS stowed but without ISPF stats"
                             " (further warnings suppressed)",
                             (unsigned)g_stats_abend,
                             pm->dsname_ebcdic, pm->member_name);
                }
            } else if (!warned_rc) {
                warned_rc = 1;
                log_warn("pdsflush_slot: ISPF stats update failed for %s(%s)"
                         " (rc=%d) -- members stowed without ISPF stats"
                         " (further warnings suppressed)",
                         pm->dsname_ebcdic, pm->member_name, rc);
            }
        }
    }
    /* No DEQ / unallocate here: both are held until the slot is released. */

    /* The PDS directory just changed: bump its mtime so clients
       invalidate their cached listing, and drop our own cached listing so the
       next readdir re-reads the directory and includes the new member. */
    export_dataset_touch(pm->export_idx, pm->dataset_idx);
    dir_openlist_invalidate(pm->dsname_ebcdic);

    return 0;
}
