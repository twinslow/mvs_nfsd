/*
 * mvsfsz.h - PDS member file-size cache.
 *
 * Caches the true (text-mode) file sizes of PDS members together with
 * the ISPF-statistics fields needed to detect staleness.  The caller is
 * responsible for validity checking: retrieve the cached entry with
 * mvsfsz_get(), compare ttr_tt/ttr_r/ispf_size/ispf_mtime against the
 * current PDS directory entry, and discard (mvsfsz_invalidate()) if any
 * field differs.
 *
 * The flat, copyable mvsfsz_entry_t struct and the simple put/get/
 * invalidate API are designed to support optional future persistence to
 * an MVS sequential dataset without structural changes.
 */

#ifndef MVSFSZ_H_INCLUDED
#define MVSFSZ_H_INCLUDED

#include "types.h"   /* uint8_t, uint16_t, uint64_t, int32_t */

/* Maximum number of entries held simultaneously in the in-memory cache. */
/* Making this small for now ... need some LRU mechanism for flush out old entries */
#define MVSFSZ_CACHE_CAPACITY    16

/* Buffer sizes including the NUL terminator. */
#define MVSFSZ_DSNAME_LEN        45   /* 44-char EBCDIC dataset name + NUL */
#define MVSFSZ_MEMBER_LEN         9   /*  8-char EBCDIC member name  + NUL */

/* -------------------------------------------------------------------- */
/* Cache entry                                                           */
/* -------------------------------------------------------------------- */

/*
 * One cached record.  All character fields are EBCDIC on MVS.
 *
 * Validity fields (ttr_tt, ttr_r, ispf_size, ispf_mtime) mirror the
 * corresponding fields from pds_member_entry_t.  If any of these differ
 * from the live PDS directory entry the cached file_size is stale.
 */
typedef struct {
    char     dsname[MVSFSZ_DSNAME_LEN];      /* PDS dataset name          */
    char     member_name[MVSFSZ_MEMBER_LEN]; /* PDS member name           */
    uint64_t file_size;   /* true text-mode file size in bytes             */
    uint16_t ttr_tt;      /* TT: relative track address component of TTR  */
    uint8_t  ttr_r;       /* R:  record-on-track component of TTR         */
    int32_t  ispf_size;   /* member size as reported by ISPF statistics   */
    int32_t  ispf_mtime;  /* ISPF modification timestamp (seconds, epoch) */
} mvsfsz_entry_t;

/* -------------------------------------------------------------------- */
/* API                                                                   */
/* -------------------------------------------------------------------- */

/*
 * Initialise the cache.  Must be called once at startup before any
 * other mvsfsz_* function is used.
 */
void mvsfsz_init(void);

/*
 * Store or update the file-size entry for (dsname, member_name).
 *
 * If an entry already exists for this key it is updated in place and
 * the count is unchanged.  If no entry exists a new slot is allocated.
 *
 * Returns  0 on success.
 * Returns -1 if the cache is full and no existing entry was found.
 */
int mvsfsz_put(
    const char *dsname,
    const char *member_name,
    uint64_t    file_size,
    uint16_t    ttr_tt,
    uint8_t     ttr_r,
    int32_t     ispf_size,
    int32_t     ispf_mtime);

/*
 * Retrieve the cached entry for (dsname, member_name).
 * Copies all fields into *entry_out on success.
 *
 * Returns  0 if found.
 * Returns -1 if not found.
 */
int mvsfsz_get(
    const char     *dsname,
    const char     *member_name,
    mvsfsz_entry_t *entry_out);

/*
 * Remove the cached entry for (dsname, member_name).
 *
 * Returns  0 if the entry was found and removed.
 * Returns -1 if no matching entry exists.
 */
int mvsfsz_invalidate(
    const char *dsname,
    const char *member_name);

/*
 * Return the number of entries currently held in the cache.
 */
int mvsfsz_count(void);

#endif /* MVSFSZ_H_INCLUDED */
