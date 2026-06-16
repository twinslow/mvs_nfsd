

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

#include "nfsd.h"
#include "mvsio.h"
#include "mvspdir.h"

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

    if (strlen(dsname) > 44) {
        errno = EINVAL; // Dataset name too long
        return -1;
    }

    // Open the dataset ... recfm=U and return a handle for reading directory blocks
    pds_dir_fh_local = fopen(strcat(strcpy(open_path_name, open_prefix), dsname), 
        "rb,klen=0,lrecl=256,blksize=256,recfm=u,force");
    if (pds_dir_fh_local == NULL) {
        // fopen failed, errno is set by fopen
        return -1;
    }

    *pds_dir_fh = pds_dir_fh_local;
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
    return retval;
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
    const uint8_t *blockptr, 
    pds_member_entry_t *member_entries, 
    int max_members, 
    int *num_members_returned) 
{
    pds_member_entry_t *entry;
    int len_entry;

    // Pointer to specific member entry we are going to populate.
    entry = &member_entries[*num_members_returned];
    // Populate the entry
    len_entry = mvs_pds_member_entry_set(entry, blockptr);
    (*num_members_returned)++;

    return len_entry;
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
    int                 max_members,
    pds_member_entry_t *member_entries,
    int                *num_members_returned,
    int                *end_of_dir)
{
    const uint8_t      *dir_block_end;
    uint8_t *blockptr = (uint8_t *)block_data; // Pointer to current position in the block
    pds_member_entry_t entry;

    dir_block_end = blockptr + *(uint16_t *)blockptr;
    blockptr += 2;

    while (blockptr < dir_block_end) {
        if ( memcmp(blockptr, MVS_PDSDIR_ENDMARK, 8) == 0) {
            // We've reached the end of the directory
            *end_of_dir = 1;   
            break;
        }
        if (memcmp(blockptr, start_member, 8) >= 0) {
            blockptr += mvs_extract_dir_entry(blockptr, member_entries, max_members, num_members_returned);
            if (*num_members_returned >= max_members) {
                break;
            }
        } else {
            blockptr += mvs_skip_dir_entry(blockptr); 
        }
    }
    return 0; // Success
}

int mvs_read_pds_dir(
    FILE *pds_dir_fh,
    const char *start_member,
    int max_members,
    pds_member_entry_t *member_entries,
    int *num_members_returned,
    int *end_of_dir)
{
    uint16_t block_len;
    uint8_t block[256];
    size_t bytes_read;

    read_and_skip_block_length(pds_dir_fh); 
    while (bytes_read = fread(block, 1, sizeof(block), pds_dir_fh) == 256) {

        // Read the end mark for the directory?
        // Otherwise, process directory block
        mvs_process_dir_block(block, start_member, max_members, member_entries, num_members_returned, end_of_dir);        
        if ( *num_members_returned >= max_members) {
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
    int max_members,
    pds_member_entry_t *member_entries,
    int *num_members_returned,
    int *end_of_dir)
{
    int retcode = 0;
    FILE *pds_dir_fh;

    // Open the dataset ... recfm=U and read directory blocks
    retcode = mvs_open_pds_dir(dsname, export_idx, &pds_dir_fh);
    if (retcode < 0) {
        return -1; // Error opening dataset, errno set by mvs_open_pds_dir
    }

    // Store up to max_members entries after we've read a member GE start_member
    retcode = mvs_read_pds_dir(
        pds_dir_fh, start_member, 
        max_members, member_entries, 
        /*pointer*/num_members_returned, /*pointer*/end_of_dir);

    if (retcode < 0) {
        mvs_close_pds_dir(pds_dir_fh);
        return -1; // Error reading directory, errno set by mvs_read_pds_dir
    }

    // Close the dataset
    return retcode;
}



/* -------------------------------------------------------------------- */
/* Retrieve a PDS member entry for specified DSN and member             */
/* -------------------------------------------------------------------- */
pds_member_entry_t *mvs_pds_get_member_entry(
    const char *dsname, 
    const char *member, 
    int export_idx)
{
    int rc;
    int num_members_returned;
    int end_of_dir;

    static pds_member_entry_t entry; // Static to allow returning pointer

    rc = mvs_pds_member_list(
        dsname, export_idx, member, 1, 
        &entry, &num_members_returned, &end_of_dir);

    if (rc < 0) {
        errno = EINVAL; // Other error
        return NULL; // Error occurred
    }
    if (num_members_returned == 0) {
        errno = ENOENT; // Member not found
        return NULL;
    }
    // It is possible that the member we got back is just a greater
    // than entry and the one we were looking for. 
    if (strcmp(entry.name, member) != 0) {
        errno = ENOENT; // Member not found
        return NULL;
    }

    return &entry; // Return pointer to the static entry
}

