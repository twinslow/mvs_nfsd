

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "nfsd.h"
#include "mvsio.h"
#include "mvspdir.h"
#include "mvsutl.h"    /* mvs_local_epoch_to_utc / mvs_utc_to_local_epoch */
#include "logger.h"

/* -------------------------------------------------------------------- */
/* Open a PDS dataset so that we can read the directory contents        */
/* -------------------------------------------------------------------- */
int mvs_open_pds_dir(
    const char *dsname, 
    int export_idx,
    FILE **pds_dir_fh)
{
    FILE *pds_dir_fh_local;
    const char *open_prefix = "//DSN:";
    char  open_path_name[6 + 45]; // 6 for prefix + 44 for max dsname length + null terminator
    char  *open_mode = "rb,klen=0,lrecl=256,blksize=256,recfm=u,force";

    logmsg_debug("NFSID010D", "mvs_open_pds_dir: Opening PDS %s for directory read", dsname);

    if (strlen(dsname) > 44) {
        logmsg_error("NFSID020E", "mvs_open_pds_dir: Dataset name %s is too long", dsname);
        errno = EINVAL; // Dataset name too long
        return -1;
    }

    strcpy(open_path_name, open_prefix);
    strcat(open_path_name, dsname);

    // Open the dataset ... recfm=U and return a handle for reading directory blocks
    logmsg_debug("NFSID030D", "mvs_open_pds_dir: Calling fopen on Opening PDS %s for directory read, mode \"%s\"", 
        open_path_name, open_mode);
    pds_dir_fh_local = fopen(open_path_name, open_mode);
    if (pds_dir_fh_local == NULL) {
        logmsg_error("NFSID040E", "mvs_open_pds_dir: fopen for %s failed, error %s", 
            dsname, strerror(errno));
        return -1;
    }

    *pds_dir_fh = pds_dir_fh_local;
    logmsg_debug("NFSID050D", "mvs_open_pds_dir: fopen OK for %s", dsname);
    return 0; // Success
}

/* -------------------------------------------------------------------- */
/* Close the PDS dataset                                                */
/* -------------------------------------------------------------------- */
int mvs_close_pds_dir(FILE *pds_dir_fh) {
    // Close the dataset handle opened for reading directory blocks
    if (fclose(pds_dir_fh) != 0) {
        // fclose failed, errno is set by fclose
        return -1;
    }
    return 0; // Return 0 on success
}

/* -------------------------------------------------------------------- */
/* Member list management                                               */
/*                                                                      */
/* A pds_member_list_t is a growable array of pds_member_entry_t:       */
/*   list_size      = allocated capacity (number of entries)            */
/*   number_in_list = number of entries currently in use               */
/*   list           = the allocated array (NULL when empty)             */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Initialize a new list: allocate the initial array and reset counts.  */
/* Returns 0 on success, -1 (errno=ENOMEM) on allocation failure.       */
/* -------------------------------------------------------------------- */
int mvspdir_mlist_init(pds_member_list_t *mlist)
{
    pds_member_entry_t *new_list;

    mlist->list           = NULL;
    mlist->list_size      = 0;
    mlist->number_in_list = 0;

    new_list = (pds_member_entry_t *)malloc(
        (size_t)MVSPDIR_MLIST_INITIAL_SIZE * sizeof(pds_member_entry_t));
    if (new_list == NULL) {
        logmsg_error("NFSID060E", "mvspdir_mlist_init: malloc of %d entries failed",
                  MVSPDIR_MLIST_INITIAL_SIZE);
        errno = ENOMEM;
        return -1;
    }

    mlist->list      = new_list;
    mlist->list_size = MVSPDIR_MLIST_INITIAL_SIZE;
    return 0;
}

/* -------------------------------------------------------------------- */
/* Expand the list capacity by MVSPDIR_MLIST_INCREMENT_SIZE entries.    */
/* On failure the original list is left intact.                         */
/* -------------------------------------------------------------------- */
int mvspdir_mlist_expand(pds_member_list_t *mlist)
{
    int32_t             new_size;
    pds_member_entry_t *new_list;

    new_size = mlist->list_size + MVSPDIR_MLIST_INCREMENT_SIZE;
    new_list = (pds_member_entry_t *)realloc(mlist->list,
        (size_t)new_size * sizeof(pds_member_entry_t));
    if (new_list == NULL) {
        logmsg_error("NFSID070E", "mvspdir_mlist_expand: realloc to %d entries failed",
                  new_size);
        errno = ENOMEM;
        return -1;
    }

    mlist->list      = new_list;
    mlist->list_size = new_size;
    return 0;
}

/* -------------------------------------------------------------------- */
/* Reserve and return the next free entry, expanding the list if it is  */
/* full.  The slot is zero-initialised and number_in_list is bumped;    */
/* the caller fills the returned entry in place.  NULL on failure.      */
/* -------------------------------------------------------------------- */
pds_member_entry_t *mvspdir_mlist_getfree(pds_member_list_t *mlist)
{
    pds_member_entry_t *entry;

    if (mlist->number_in_list >= mlist->list_size) {
        if (mvspdir_mlist_expand(mlist) != 0)
            return NULL;
    }

    entry = &mlist->list[mlist->number_in_list];
    mlist->number_in_list++;
    mvs_pds_member_entry_init(entry);
    return entry;
}

/* -------------------------------------------------------------------- */
/* Append a copy of entry as the last element of the list.              */
/* Returns 0 on success, -1 on allocation failure.                      */
/* -------------------------------------------------------------------- */
int mvspdir_mlist_setaslast(pds_member_list_t *mlist, pds_member_entry_t *entry)
{
    pds_member_entry_t *slot;

    slot = mvspdir_mlist_getfree(mlist);
    if (slot == NULL)
        return -1;

    *slot = *entry;   /* copy caller's entry into the reserved slot */
    return 0;
}

/* -------------------------------------------------------------------- */
/* Shrink the allocated capacity to the number of entries in use.       */
/* If the list is empty it is freed and reset to NULL/0.  On realloc    */
/* failure the original (larger) list is left intact.                   */
/* -------------------------------------------------------------------- */
int mvspdir_mlist_shrink(pds_member_list_t *mlist)
{
    pds_member_entry_t *new_list;

    if (mlist->number_in_list <= 0) {
        free(mlist->list);
        mlist->list           = NULL;
        mlist->list_size      = 0;
        mlist->number_in_list = 0;
        return 0;
    }

    new_list = (pds_member_entry_t *)realloc(mlist->list,
        (size_t)mlist->number_in_list * sizeof(pds_member_entry_t));
    if (new_list == NULL) {
        logmsg_error("NFSID080E", "mvspdir_mlist_shrink: realloc to %d entries failed",
                  mlist->number_in_list);
        errno = ENOMEM;
        return -1;
    }

    mlist->list      = new_list;
    mlist->list_size = mlist->number_in_list;
    return 0;
}

/* -------------------------------------------------------------------- */
/* Free the list's storage and reset it to an empty state.  Safe to     */
/* call on an already-empty list (list == NULL); free(NULL) is a no-op. */
/* After this call the structure may be reused via mvspdir_mlist_init   */
/* or mvspdir_mlist_getfree.                                            */
/* -------------------------------------------------------------------- */
void mvspdir_mlist_free(pds_member_list_t *mlist)
{
    free(mlist->list);
    mlist->list           = NULL;
    mlist->list_size      = 0;
    mlist->number_in_list = 0;
}

/* -------------------------------------------------------------------- */
/* ISPF stats helper routines                                           */
/* -------------------------------------------------------------------- */

/*
 * ispf_tm_to_timet: convert a broken-down UTC time to a Unix time_t
 * (seconds since 1970-01-01 00:00:00 UTC) without using mktime().
 *
 * Avoids all JCC timezone/mktime runtime issues.
 * Valid for years 1970 and later.
 */
static time_t ispf_tm_to_timet(int year, int mon, int mday,
                                int hour, int min,  int sec)
{
    /* Cumulative days to start of each month (non-leap year) */
    static int mdays[12] = { 0, 31, 59, 90, 120, 151,
                              181, 212, 243, 273, 304, 334 };
    int  is_leap;
    int  y;
    long days;
    long leaps;

    is_leap = (year % 4 == 0) &&
              (year % 100 != 0 || year % 400 == 0);

    /*
     * Count leap years between 1970 and (year-1) inclusive.
     * Formula: floor(y/4) - floor(y/100) + floor(y/400)
     */
    y     = year - 1;
    leaps = (long)(y/4 - y/100 + y/400) -
            (long)(1969/4 - 1969/100 + 1969/400);

    /* Days from epoch to start of this year */
    days  = (long)(year - 1970) * 365L + leaps;

    /* Days within this year up to start of this month */
    days += (long)mdays[mon];            /* mon is 0-based */

    /* Add leap day if we are past February in a leap year */
    if (is_leap && mon > 1) days++;

    /* Days within this month (mday is 1-based) */
    days += (long)(mday - 1);

    return (time_t)(days * 86400L +
                    (long)hour * 3600L +
                    (long)min  *   60L +
                    (long)sec);
}

static void julian_to_md(int year, int yday, int *mon, int *mday)
{
    static int dim[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int is_leap = (year % 4 == 0) &&
                  (year % 100 != 0 || year % 400 == 0);
    int m;

    dim[1] = is_leap ? 29 : 28;

    for (m = 0; m < 12; m++) {
        if (yday <= dim[m]) {
            *mon  = m;
            *mday = yday;
            return;
        }
        yday -= dim[m];
    }
    /* Invalid input fallback */
    *mon  = 11;
    *mday = 31;
}

time_t convert_ispf_datetime(
    uint8_t              *dateptr,
    uint8_t              *hhmmf,
    uint8_t               seconds)
{

    int year;
    int day_of_year;
    int month, day_of_mon;

    int hour = 0;
    int mins = 0;
    time_t retval;

    year = 1900 + 100*bcd_byte_to_int(dateptr[0]) +
        bcd_byte_to_int(dateptr[1]);
    day_of_year = 10 * bcd_byte_to_int(dateptr[2]) +
        ((dateptr[3] >> 4) & 0x0f);
    julian_to_md(year, day_of_year, &month, &day_of_mon);

    if ( hhmmf ) {
        hour = bcd_byte_to_int(hhmmf[0]);
        mins = bcd_byte_to_int(hhmmf[1]);
    }

    // Usage of the following function is an alternate to using the
    // library function mktime, which is giving an OC4 error.
    retval = ispf_tm_to_timet(
                 year, month, day_of_mon,
                 hour, mins, (int) seconds);

    /* The ISPF fields are LOCAL wall-clock; NFS wants UTC epoch. */
    return mvs_local_epoch_to_utc(retval);
}


/* -------------------------------------------------------------------- */
/* Process an entry within a directory block                            */
/* -------------------------------------------------------------------- */
void mvs_extract_ispf_stats(
    pds_member_entry_t   *entry,
    unsigned char        *userdata,
    int                   user_data_len)
{
    time_t    temptime = 0;
    int       seconds;

    /* Mark this entry as being based around ISPF stats */
    entry->info_flags = MVS_PDSDIR_IFLG_ISPFSTATS;

    entry->ver = userdata[0];
    entry->mod = userdata[1];
    entry->ispf_flags = userdata[2];

    userdata += 3;
    seconds = bcd_byte_to_int(userdata[0]);
    userdata++;

    temptime = convert_ispf_datetime(userdata, NULL, 0);
    entry->crdate = temptime;
    userdata += 4;

    temptime =
        convert_ispf_datetime(userdata, &userdata[4], seconds);
    entry->chgdate = temptime;
    userdata += 6;

    // If the extended stats indicator is set then use those
    if ( entry->ispf_flags & MVS_PDSDIR_ISPF_EXT_STATS ) {
        entry->size = *(int *)userdata;
        userdata += 4;
        entry->initSize = *(int *)userdata;
        userdata += 4;
        entry->modCount = *(int *)userdata;
        userdata += 4;
    } else {
        // Non extended stats ... so size fields are two bytes. 
        entry->size = *(unsigned short *)userdata;
        userdata += 2;
        entry->initSize = *(unsigned short *)userdata;
        userdata += 2;
        entry->modCount = *(unsigned short *)userdata;
        userdata += 2;
    }

    bytes_to_string(entry->user, userdata, 8);

    // There will either be 2 bytes remaining (unused) for non-extended stats,
    // or 6 bytes remaining (unused) for extended stats. We don't need to do anything with these.
}

void mvs_set_no_ispf_stats(pds_member_entry_t *entry) {
    struct timeval tv;

    entry->ver = 0;
    entry->mod = 0;
    entry->ispf_flags = 0;
    entry->crdate = 0;
    entry->chgdate = 0;
    entry->size = 999;
    entry->initSize = 999;
    entry->modCount = 0;
    memset(entry->user, 0, sizeof(entry->user));

    // Now the accessed/modified/created date/times.
    // For simplicity, we'll set these all to the same value based on the current time.
    
    gettimeofday(&tv, NULL);
    entry->crdate = (int32_t)tv.tv_sec;
    entry->chgdate = (int32_t)tv.tv_sec;
}

/* -------------------------------------------------------------------- */
/* ISPF stats ENCODING (inverse of mvs_extract_ispf_stats)              */
/* -------------------------------------------------------------------- */

/* One byte of packed decimal from a value 0..99. */
static uint8_t int_to_bcd(int v)
{
    return (uint8_t)((((v / 10) % 10) << 4) | (v % 10));
}

/* Pack a UTC time_t's DATE into 4 bytes as 0C YY DD DF (see the decoder in
   convert_ispf_datetime()).  The low nibble of byte 3 is the packed-decimal
   sign; the decoder ignores it, we write 0x0F. */
static void ispf_pack_date(time_t t, uint8_t *out)
{
    struct tm *tm_p;
    int        year;
    int        ddd;

    tm_p = gmtime(&t);
    year = tm_p->tm_year + 1900;
    ddd  = tm_p->tm_yday + 1;               /* tm_yday is 0-based */

    out[0] = int_to_bcd((year - 1900) / 100);   /* centuries since 1900 */
    out[1] = int_to_bcd((year - 1900) % 100);   /* year within century  */
    out[2] = int_to_bcd(ddd / 10);              /* first digits of ddd  */
    out[3] = (uint8_t)(((ddd % 10) << 4) | 0x0F);
}

int32_t mvs_ispf_count_lines(const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    int32_t  lines = 0;

    if (buf == NULL || len == 0)
        return 0;

    for (i = 0; i < len; i++) {
        if (buf[i] == 0x0A)       /* ASCII LF -- buffer is ASCII here */
            lines++;
    }
    /* A trailing partial line (no final LF) is still a record. */
    if (buf[len - 1] != 0x0A)
        lines++;

    return lines;
}

int mvs_encode_ispf_stats(const pds_member_entry_t *entry, uint8_t *userdata)
{
    struct tm *tm_p;
    time_t     t;
    int        chg_hour;
    int        chg_min;
    int        chg_sec;
    uint16_t   v16;
    int        i;
    int        ulen;

    memset(userdata, 0, MVS_ISPF_STATS_LEN);

    /* Version and modification level: 1-byte binary each. */
    userdata[0] = (uint8_t)entry->ver;
    userdata[1] = (uint8_t)entry->mod;

    /* Flags: non-extended stats (clear the extended-stats bit). */
    userdata[2] = (uint8_t)(entry->ispf_flags & ~MVS_PDSDIR_ISPF_EXT_STATS);

    /* crdate/chgdate are UTC epoch; ISPF stats store LOCAL wall-clock, so
       shift to the local-time domain before breaking either one down. */
    /* Decompose the change time once (need seconds [3] and time [12..13]). */
    t    = mvs_utc_to_local_epoch((time_t)entry->chgdate);
    tm_p = gmtime(&t);
    chg_hour = tm_p->tm_hour;
    chg_min  = tm_p->tm_min;
    chg_sec  = tm_p->tm_sec;
    userdata[3] = int_to_bcd(chg_sec);

    /* Creation date [4..7] (date only) -- calls gmtime() again. */
    ispf_pack_date(mvs_utc_to_local_epoch((time_t)entry->crdate), &userdata[4]);

    /* Change date [8..11] and time [12..13]. */
    ispf_pack_date(mvs_utc_to_local_epoch((time_t)entry->chgdate), &userdata[8]);
    userdata[12] = int_to_bcd(chg_hour);
    userdata[13] = int_to_bcd(chg_min);

    /* Size fields: non-extended -> native uint16 (mirrors the decoder's
       *(unsigned short *) read).  Clamp to 0xFFFF. */
    v16 = (entry->size     > 0xFFFF) ? (uint16_t)0xFFFF : (uint16_t)entry->size;
    memcpy(&userdata[14], &v16, sizeof(v16));
    v16 = (entry->initSize > 0xFFFF) ? (uint16_t)0xFFFF : (uint16_t)entry->initSize;
    memcpy(&userdata[16], &v16, sizeof(v16));
    v16 = (entry->modCount > 0xFFFF) ? (uint16_t)0xFFFF : (uint16_t)entry->modCount;
    memcpy(&userdata[18], &v16, sizeof(v16));

    /* Userid [20..27]: left-justified, blank-padded, native charset. */
    ulen = (int)strlen(entry->user);
    if (ulen > 8) ulen = 8;
    for (i = 0; i < 8; i++)
        userdata[20 + i] = (uint8_t)((i < ulen) ? entry->user[i] : ' ');

    /* [28..29] reserved -- already zero from the memset. */
    return MVS_ISPF_STATS_LEN;
}

int mvs_build_write_stats(
    pds_member_entry_t        *entry,
    const pds_member_entry_t  *existing,
    int32_t                    line_count,
    time_t                     now)
{
    if (existing == NULL) {
        /* Brand-new member: a fresh set of stats. */
        mvs_pds_member_entry_init(entry);
        entry->info_flags = MVS_PDSDIR_IFLG_ISPFSTATS;
        entry->ispf_flags = 0;
        entry->ver        = 1;
        entry->mod        = 1;
        entry->crdate     = (int32_t)now;
        entry->chgdate    = (int32_t)now;
        entry->size       = line_count;
        entry->initSize   = line_count;
        entry->modCount   = 0;
        strcpy(entry->user, "NFSD");
        return 1;
    }

    if (!(existing->info_flags & MVS_PDSDIR_IFLG_ISPFSTATS)) {
        /* Existing member that never had ISPF stats: leave it that way. */
        mvs_pds_member_entry_init(entry);
        return 0;
    }

    /* Existing member with stats: keep version, creation date, initial size,
       modification count and id; refresh the changed date and current size,
       and bump the modification level (capped at 99). */
    *entry         = *existing;
    entry->chgdate = (int32_t)now;
    entry->size    = line_count;
    entry->mod     = (uint8_t)((existing->mod < 99) ? (existing->mod + 1) : 99);
    return 1;
}

void mvs_pds_member_entry_init(
    pds_member_entry_t *entry)
{
    memset(entry, 0, sizeof(pds_member_entry_t));
}


int mvs_pds_member_entry_set(
    pds_member_entry_t      *entry, 
    const uint8_t           *start_blockptr)
{
    short       user_data_count_hw;
    uint8_t    *blockptr = (uint8_t *)start_blockptr;


    mvs_pds_member_entry_init(entry);

    // Copy member name, trim trailing blanks and null terminate.
    bytes_to_string((unsigned char *)entry->name, blockptr, 8);
    blockptr += 8;
    // logmsg_debug("NFSID090D", "mvs_pds_member_entry_set:   Processing member %s", entry->name);

    // Copy TTR - relative track number and record number
    entry->first_block_tt = *( (uint16_t *)blockptr );
    blockptr += 2;
    entry->first_block_rec = *blockptr;
    blockptr++;

    // Get entry type (0 = member name, 0x80 = alias)
    entry->entry_type = (*blockptr) & MVS_PDSDIR_ALIAS_MASK;
    // Get halfword count of user data
    user_data_count_hw =
        (*blockptr) & MVS_PDSDIR_USERDATA_COUNT_MASK;
    blockptr++;

    // If we have user data, and the length is the expected
    // length for ISPF stats, then extract them. Otherwise, 
    // we'll need to populate the entry with default values.
    if (user_data_count_hw * 2 >= 30) {
        mvs_extract_ispf_stats(entry, blockptr, user_data_count_hw * 2);
    } else {
        mvs_set_no_ispf_stats(entry);
    }


    // Point past user data and return the address ...
    // which will be the next member entry, or beyond the end
    // block.
    blockptr += user_data_count_hw * 2;
    return blockptr - start_blockptr;
}

int mvs_skip_dir_entry(
    const uint8_t           *start_blockptr)
{
    short       user_data_count_hw;
    uint8_t    *blockptr = (uint8_t *)start_blockptr;

    blockptr += 11;
    user_data_count_hw =
        (*blockptr) & MVS_PDSDIR_USERDATA_COUNT_MASK;
    blockptr++;

    // Point past user data
    // which will be the next member entry, or beyond the end
    // block.
    blockptr += user_data_count_hw * 2;
    return blockptr - start_blockptr; // Return length of entry
}

int mvs_extract_dir_entry(
    const uint8_t      *blockptr,
    pds_member_list_t  *mlist)
{
    pds_member_entry_t *entry;

    /* Reserve the next free slot in the list (expands the list if full). */
    entry = mvspdir_mlist_getfree(mlist);
    if (entry == NULL) {
        /* Allocation failure -- errno already set by mvspdir_mlist_getfree. */
        return -1;
    }

    /* Populate the reserved entry and return its byte length in the block. */
    return mvs_pds_member_entry_set(entry, blockptr);
}



/* -------------------------------------------------------------------- */
/* Read the directory blocks until we find the member >= the            */
/* specified starting member name.                                      */
/* Once we've reached the starting member, process the directory block  */
/* extracting the member (multiple) info in the blocks. Continue this   */
/* until we reach the end of the directory or have read the requested   */
/* number of members.                                                   */
/* -------------------------------------------------------------------- */
static void read_and_skip_block_length(FILE *pds_dir_fh) {
    uint16_t block_len;
    fread(&block_len, 1, sizeof(block_len), pds_dir_fh); // Read the block length, which we won't use.
}


int mvs_process_dir_block(
    const uint8_t      *block_data,
    const char         *start_member,
    pds_member_list_t  *mlist,
    int                *end_of_dir)
{
    const uint8_t      *dir_block_end;
    uint8_t            *blockptr = (uint8_t *)block_data; // current position in block
    int                 len_entry;

    dir_block_end = blockptr + *(uint16_t *)blockptr;
    blockptr += 2;

    //logmsg_debug("NFSID100D", "mvs_process_dir_block: Looking for members starting at %s", start_member);

    while (blockptr < dir_block_end) {
        if ( memcmp(blockptr, MVS_PDSDIR_ENDMARK, 8) == 0) {
            // We've reached the end of the directory
            //logmsg_debug("NFSID110D", "mvs_process_dir_block: Found PDS directory end mark - set end_of_dir = 1");
            *end_of_dir = 1;
            break;
        }
    //  logmsg_debug("NFSID120D", "mvs_process_dir_block:   Looking at member name %-8.8s", blockptr);
        if (memcmp(blockptr, start_member, 8) >= 0) {
            len_entry = mvs_extract_dir_entry(blockptr, mlist);
            if (len_entry < 0) {
                // List expansion failed -- errno set by mvs_extract_dir_entry.
                return -1;
            }
            blockptr += len_entry;
        } else {
            blockptr += mvs_skip_dir_entry(blockptr);
        }
    }
    return 0; // Success
}

int mvs_read_pds_dir(
    FILE *pds_dir_fh,
    const char *start_member,
    pds_member_list_t *mlist,
    int *end_of_dir)
{
    uint8_t block[256];
    int     rc;

    *end_of_dir = 0;

    logmsg_debug("NFSID130D", "mvs_read_pds_dir: Start reading at member %s", start_member);

    read_and_skip_block_length(pds_dir_fh);
    while (fread(block, 1, sizeof(block), pds_dir_fh) == 256) {
        //logmsg_debug("NFSID140D", "mvs_read_pds_dir: Read directory block ... first mem %-8.8s", &block[2]);
        // Append every member >= start_member from this block into the list.
        rc = mvs_process_dir_block(block, start_member, mlist, end_of_dir);
        if (rc < 0) {
            return -1;  // List expansion failed -- errno already set.
        }
        if ( *end_of_dir ) {
            //logmsg_debug("NFSID150D", "mvs_read_pds_dir: Reached end of directory");
            break;
        }

        read_and_skip_block_length(pds_dir_fh);
    }

    return 0; // Success
}

/* -------------------------------------------------------------------- */
/* Retrieve member list for a PDS                                       */
/*     Starting with (>=) specified member                              */
/*     For a maximum number of members                                  */
/*     Return the number of members read and saved                      */
/*     Indicate if the end of the directory has been reached            */
/* Returns --                                                           */
/*     0 on success (also applies to no members found)                  */
/*    -1 on error (errno set)                                           */
/* -------------------------------------------------------------------- */
int mvs_pds_member_list(
    const char *dsname,
    int export_idx,
    const char *start_member,
    pds_member_list_t *mlist,
    int *end_of_dir)
{
    int retcode = 0;
    FILE *pds_dir_fh;

    // Open the dataset ... recfm=U and read directory blocks
    retcode = mvs_open_pds_dir(dsname, export_idx, &pds_dir_fh);
    if (retcode < 0) {
        return -1; // Error opening dataset, errno set by mvs_open_pds_dir
    }

    // Append every member >= start_member into the caller's list.
    retcode = mvs_read_pds_dir(pds_dir_fh, start_member, mlist, end_of_dir);

    // Always close the dataset, whether the read succeeded or not.
    mvs_close_pds_dir(pds_dir_fh);

    if (retcode < 0) {
        return -1; // Error reading directory, errno set by mvs_read_pds_dir
    }

    return 0;
}


/* -------------------------------------------------------------------- */
/* Retrieve a PDS member entry for specified DSN and member             */
/* -------------------------------------------------------------------- */
pds_member_entry_t *mvs_pds_get_member_entry(
    const char *dsname,
    const char *member,
    int export_idx,
    pds_member_entry_t *entry)
{
    int                rc;
    int                end_of_dir = 0;
    pds_member_list_t  mlist;

    logmsg_debug("NFSID160D", "mvs_pds_get_member_entry: Getting member info for '%s(%s)'",
        dsname, member);

    if (mvspdir_mlist_init(&mlist) != 0) {
        errno = ENOMEM;
        return NULL;
    }

    /* Read from 'member' to the end of the directory into the list.  The
       first entry (if any) is the smallest member name >= 'member'. */
    rc = mvs_pds_member_list(dsname, export_idx, member, &mlist, &end_of_dir);
    logmsg_debug("NFSID170D", "mvs_pds_get_member_entry: mvs_pds_member_list rc = %d, members = %d",
        rc, mlist.number_in_list);
    if (rc < 0) {
        mvspdir_mlist_free(&mlist);
        errno = EINVAL; // Other error
        return NULL;
    }

    if (mlist.number_in_list == 0) {
        mvspdir_mlist_free(&mlist);
        errno = ENOENT; // Member not found
        return NULL;
    }

    // The list is sorted; the first entry may merely be the next member
    // greater than the one we asked for, so confirm an exact match.
    logmsg_debug("NFSID180D", "mvs_pds_get_member_entry: first entry name = %s", mlist.list[0].name);
    if (strcmp(mlist.list[0].name, member) != 0) {
        mvspdir_mlist_free(&mlist);
        errno = ENOENT; // Member not found
        return NULL;
    }

    // Copy the matched entry into the caller's buffer before freeing.
    *entry = mlist.list[0];
    mvspdir_mlist_free(&mlist);
    return entry;
}


/* -------------------------------------------------------------------- */
/* Fold a member list into a 32-bit signature (pure; no I/O).           */
/*                                                                      */
/* FNV-1a over each entry's 8-char name and its TTR (first_block_tt +   */
/* first_block_rec), then the member count.  Deterministic and stable   */
/* for an unchanged, same-order list (a PDS directory is name-sorted);  */
/* changes on any add / remove / replace (new TTR).  Blind to an        */
/* in-place update that preserves name, length, and TTR (a "zap").      */
/* Split out from mvs_pds_dir_signature so it can be unit-tested with an */
/* in-memory list.                                                       */
/* -------------------------------------------------------------------- */
uint32_t mvs_pds_dir_sig_calc(const pds_member_entry_t *list, int count)
{
    uint32_t hash;
    int      i;
    int      j;

    hash = 2166136261u;   /* FNV-1a offset basis */
    for (i = 0; i < count; i++) {
        const pds_member_entry_t *e = &list[i];
        for (j = 0; j < 8; j++) {
            hash ^= (uint32_t)(unsigned char)e->name[j];
            hash *= 16777619u;   /* FNV-1a prime */
        }
        hash ^= (uint32_t)(e->first_block_tt & 0xFFu);
        hash *= 16777619u;
        hash ^= (uint32_t)((e->first_block_tt >> 8) & 0xFFu);
        hash *= 16777619u;
        hash ^= (uint32_t)e->first_block_rec;
        hash *= 16777619u;
    }
    /* Mix in the count so an add+remove that happens to preserve the fold
       still changes the signature. */
    hash ^= (uint32_t)count;
    hash *= 16777619u;

    return hash;
}

/* -------------------------------------------------------------------- */
/* Compute a signature of a PDS directory's current contents.           */
/*                                                                      */
/* Reads the whole directory and folds it via mvs_pds_dir_sig_calc.     */
/* -------------------------------------------------------------------- */
int mvs_pds_dir_signature(
    const char *dsname,
    int         export_idx,
    uint32_t   *sig_out)
{
    pds_member_list_t  mlist;
    int                end_of_dir = 0;
    int                rc;
    /* 8 low bytes so every real (EBCDIC) member name compares >= it, and a
       valid empty C string for the %s log in mvs_read_pds_dir. */
    char               from_start[9];

    if (sig_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(from_start, 0, sizeof(from_start));

    if (mvspdir_mlist_init(&mlist) != 0) {
        errno = ENOMEM;
        return -1;
    }

    rc = mvs_pds_member_list(dsname, export_idx, from_start, &mlist, &end_of_dir);
    if (rc < 0) {
        mvspdir_mlist_free(&mlist);
        return -1;   /* errno set by mvs_pds_member_list */
    }

    *sig_out = mvs_pds_dir_sig_calc(mlist.list, mlist.number_in_list);

    mvspdir_mlist_free(&mlist);
    return 0;
}

