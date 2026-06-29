/*
 * mvsfsz.h - PDS member file-size cache.
 *
 * Primary purpose: provide mvsfsz_get_member_size(), a single call that
 * returns the NFS-visible byte count of a PDS member.  The NFS-visible
 * size is determined by reading the member in text mode (trailing spaces
 * stripped per record, each record terminated with '\n'), which differs
 * from the on-disk size reported by the PDS directory.
 *
 * The result is stored in a fixed-size LRU cache keyed on (dsname,
 * member_name).  Each cached entry carries the ISPF-statistics fields
 * that were current at the time the size was computed.  On the next
 * request the caller supplies the live pds_member_entry_t; if any
 * validity field has changed the entry is evicted and the size is
 * recomputed by reopening the member.
 *
 * The flat, copyable mvsfsz_entry_t struct and the put/get/invalidate
 * primitives are designed to support optional future persistence to an
 * MVS sequential dataset without structural change.
 *
 * JCC C89 compliance: block comments only; no C99 features.
 */

#ifndef MVSFSZ_H_INCLUDED
#define MVSFSZ_H_INCLUDED

#include "types.h"     /* uint8_t, uint16_t, uint64_t, int32_t */
#include "mvspdir.h"   /* pds_member_entry_t                   */

/* Maximum number of entries held simultaneously in the in-memory cache.
 * Sized to hold all members of a typical large PDS.               */
#define MVSFSZ_CACHE_CAPACITY   250

/* Buffer sizes including the NUL terminator. */
#define MVSFSZ_DSNAME_LEN        45   /* 44-char EBCDIC dataset name + NUL */
#define MVSFSZ_MEMBER_LEN         9   /*  8-char EBCDIC member name  + NUL */

/* -------------------------------------------------------------------- */
/* Cache entry                                                           */
/* -------------------------------------------------------------------- */

/*
 * One cached record.  All character fields are EBCDIC on MVS.
 *
 * Validity fields mirror the corresponding fields from pds_member_entry_t.
 * mvsfsz_get_member_size() compares these against the live directory entry
 * to decide whether the cached file_size is still accurate.
 *
 *   mvsfsz_entry.ttr_tt     <-> pds_member_entry_t.first_block_tt
 *   mvsfsz_entry.ttr_r      <-> pds_member_entry_t.first_block_rec
 *   mvsfsz_entry.ispf_size  <-> pds_member_entry_t.size
 *   mvsfsz_entry.ispf_mtime <-> pds_member_entry_t.chgdate
 */
typedef struct {
    char     dsname[MVSFSZ_DSNAME_LEN];      /* PDS dataset name                        */
    char     member_name[MVSFSZ_MEMBER_LEN]; /* PDS member name                         */
    uint64_t file_size;   /* true text-mode file size in bytes                          */
    uint16_t ttr_tt;      /* TT: relative track address component of TTR                */
    uint8_t  ttr_r;       /* R:  record-on-track component of TTR                       */
    int32_t  ispf_size;   /* member size (#lines) as reported by ISPF statistics        */
    int32_t  ispf_mtime;  /* ISPF modification timestamp (seconds since epoch)          */
} mvsfsz_entry_t;

/* -------------------------------------------------------------------- */
/* Primary API                                                           */
/* -------------------------------------------------------------------- */

/*
 * mvsfsz_get_member_size: return the NFS-visible byte count of a PDS member.
 *
 * Looks up (dsname, member_name) in the cache.  If a valid entry is found
 * (all four validity fields match member_entry) the cached file_size is
 * returned immediately.
 *
 * If no entry exists, or the entry is stale, the member is opened in text
 * mode via fopen("//DSN:<dsname>(<member>)", "rt") and read in full.  The
 * total byte count from fread() -- which reflects stripped trailing spaces
 * and appended newlines -- is stored in the cache and returned.
 *
 * When the cache is full the least recently used entry is evicted to make
 * room before the new entry is stored.
 *
 * Parameters:
 *   dsname        EBCDIC PDS dataset name (NUL-terminated, max 44 chars).
 *   member_name   EBCDIC PDS member name  (NUL-terminated, max  8 chars).
 *   member_entry  Pointer to the live PDS directory entry for this member.
 *                 Used for cache validation and for populating validity
 *                 fields in a new cache entry.
 *   file_size_out Receives the NFS-visible byte count on success.
 *
 * Returns  0 on success (*file_size_out is valid).
 * Returns -1 if the member file could not be opened or read.
 */
int mvsfsz_get_member_size(
    const char         *dsname,
    const char         *member_name,
    pds_member_entry_t *member_entry,
    uint64_t           *file_size_out);

/* -------------------------------------------------------------------- */
/* Cache primitives                                                      */
/* -------------------------------------------------------------------- */

/*
 * Initialise the cache.  Must be called once at startup before any
 * other mvsfsz_* function is used.
 */
void mvsfsz_init(void);

/*
 * Store or update the file-size entry for (dsname, member_name).
 *
 * If an entry already exists for this key it is updated in place.
 * If the cache is full the least recently used entry is evicted first.
 *
 * Returns  0 on success (always, with LRU eviction).
 * Returns -1 only in the event of an unexpected internal error.
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

/*
 * Load file-size records from a text file into the cache.
 *
 * Each non-comment, non-blank line must have the format:
 *
 *   <dsname>  <member>  <file_size>
 *
 * Fields are whitespace-delimited.  <dsname> is at most 44 characters,
 * <member> is at most 8 characters, and <file_size> is a decimal integer.
 * Lines whose first character is '#' are treated as comments and skipped.
 * Lines that do not parse as exactly three fields are silently skipped.
 *
 * Validity fields (ttr_tt, ttr_r, ispf_size, ispf_mtime) are zeroed for
 * every loaded entry.  This means mvsfsz_get_member_size() will treat
 * these entries as stale and recompute the size on first access unless
 * the validity fields are later updated via mvsfsz_put().
 *
 * Returns the number of entries successfully stored (>= 0).
 * Returns -1 if the file could not be opened.
 */
int mvsfsz_load(const char *filename);

#endif /* MVSFSZ_H_INCLUDED */
