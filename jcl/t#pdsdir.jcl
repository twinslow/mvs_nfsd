//TONYWZ1  JOB (MVSNFSD),
//             'Test directory read',
//             CLASS=A,
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Test read of PDS directory
//*
//********************************************************************
//TEST EXEC JCCCLG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdio.h>
#include <time.h>

#define MVS_PDSDIR_ENDMARK          "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
#define MVS_PDSDIR_USERDATA_COUNT_MASK ((unsigned char) 0x1F)
#define MVS_PDSDIR_ALIAS_MASK   ((unsigned char) 0x80)
#define MVS_PDSDIR_ISPF_EXT_STATS ((unsigned char) 0x04)

typedef struct {
    long             crdate;
    long             chgdate;
    int              size;
    int              initSize;
    int              modCount;
    unsigned char    name[9];
    unsigned char    entry_type;
    char             user[9];
    unsigned char    ver;
    unsigned char    mod;
    unsigned short   first_block_tt;
    unsigned char    first_block_rec;
    unsigned char    ispf_flags;
} pds_member_entry_t;

//  ----+----1----+----2----+----3--
//  --+----4----+----5----+----7----+----8
static unsigned char *out_title =
   "Member   Track  Recd  Created    "
   "Modified            Size   Init   Mod    VV.MM  User\n";

static unsigned char *out_fmt =
 "%8s 0x%04X 0x%02X  %-10s %-19s %-6d %-6d %-6d %02d.%02d  %s\n";


void dump(pds_member_entry_t *entry) {

    static int out_header = 0;
    unsigned char       created_date[11];
    unsigned char       modified_datetime[21];

    if (!out_header) {
        fprintf(stdout,out_title);
        out_header = 1;
    }

    strftime(created_date, sizeof(created_date),
       "%Y-%m-%d",
       gmtime(&entry->crdate));

    strftime(modified_datetime, sizeof(modified_datetime),
       "%Y-%m-%d %H:%M:%S",
       gmtime(&entry->chgdate));

    fprintf(stdout, out_fmt,
       entry->name,
       entry->first_block_tt,
       entry->first_block_rec,
       created_date,
       modified_datetime,
       entry->size,
       entry->initSize,
       entry->modCount,
       entry->ver,
       entry->mod,
       entry->user );
}

void bytes_to_string(
    unsigned char *dest,
    unsigned char *source,
    int            source_len)
{
    int x = source_len;
    memcpy(dest, source, source_len);
    dest[x--] = '\0';
    while ( dest[x] == ' ' && x >= 0 )
        dest[x--] = '\0';
}

void pds_member_entry_init(pds_member_entry_t *entry) {
    memset(entry, 0, sizeof(pds_member_entry_t));
}

int bcd_byte_to_int(unsigned char bcdval_in)
{
    int bcdval = bcdval_in;
    return 10 * ((bcdval >> 4) & 0x0F) + (bcdval & 0x0F);
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
    unsigned char        *dateptr,
    unsigned char        *hhmmf,
    unsigned char        seconds)
{

    int year = 2026;
    int day_of_year = 0;
    int month, day_of_mon;

    int hour = 0;
    int mins = 0;
    time_t retval;

//  struct tm target_time;

    // Format of the ISPF member stats is
    // +0x00     1 byte century, 0=1900, 1=2000 etc.
    // +0x01     3 bytes packed decimal julian date YYDDDF
//  /* Print raw bytes safely - avoid unaligned fullword load */
//  fprintf(stderr,
//      "Date-input = 0x%02X%02X%02X%02X  ",
//      dateptr[0], dateptr[1], dateptr[2], dateptr[3]);

    year = 1900 + 100*bcd_byte_to_int(dateptr[0]) +
        bcd_byte_to_int(dateptr[1]);
    day_of_year = 10 * bcd_byte_to_int(dateptr[2]) +
        ((dateptr[3] >> 4) & 0x0f);
    julian_to_md(year, day_of_year, &month, &day_of_mon);

    if ( hhmmf ) {
        hour = bcd_byte_to_int(hhmmf[0]);
        mins = bcd_byte_to_int(hhmmf[1]);
    }

//  memset(&target_time, 0, sizeof(target_time));
//  target_time.tm_year = year - 1900;
//  target_time.tm_mday = day_of_mon;
//  target_time.tm_mon = month;
//  target_time.tm_hour = hour;
//  target_time.tm_min  = mins;
//  target_time.tm_sec  = seconds;
//  target_time.tm_isdst = 0;

//  fprintf(stderr,
//      "year=%d, tm_year=%d, tm_mon=%d, tm_mday=%d, "
//      "tm_hour=%d, tm_min=%d, tm_sec=%d tz=%s\n",
//      year,
//      target_time.tm_year,
//      target_time.tm_mon,
//      target_time.tm_mday,
//      target_time.tm_hour,
//      target_time.tm_min,
//      target_time.tm_sec,
//      target_time.tm_zone
//  );

    retval = ispf_tm_to_timet(
                 year, month, day_of_mon,
                 hour, mins, (int) seconds);
    return retval;
}

void pds_member_dump_userdata(
    pds_member_entry_t   *entry,
    unsigned char        *userdata,
    int                   user_data_len)
{
    int i;
    fprintf(stderr, "     User data ");
    for (i=0; i<user_data_len; i += 2) {
        fprintf(stderr, "%04X ", *((unsigned short *) &userdata[i]) );
    }
    fprintf(stderr, "\n");
}

void pds_member_get_ispf_stats(
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

    entry->size = *(unsigned short *)userdata;
    userdata += 2;
    entry->initSize = *(unsigned short *)userdata;
    userdata += 2;
    entry->modCount = *(unsigned short *)userdata;
    userdata += 2;

    bytes_to_string(entry->user, userdata, 8);

/*
    // If the extended stats indicator is set then use those
    if ( entry->ispf_flags & MVS_PDSDIR_ISPF_EXT_STATS ) {
        userdata += 2;
        entry->size = *(int *)userdata;
        userdata += 4;
        entry->initSize = *(int *)userdata;
        userdata += 4;
        entry->modCount = *(int *)userdata;
        userdata += 4;
    }
*/
}

int pds_member_entry_set(
    pds_member_entry_t *entry,
    unsigned char *blockptr)
{
    short      user_data_count_hw;
    unsigned char *save_blockptr;

    save_blockptr = blockptr;

    // Copy member name and null terminate it.
    memcpy(entry->name, blockptr, 8);
    entry->name[8] = '\0';
    blockptr += 8;

    // Copy TTR - relative track number and record number
    entry->first_block_tt = *( (short *)blockptr );
    blockptr += 2;
    entry->first_block_rec = *blockptr;
    blockptr++;

    // Get entry type (0 = member name, 0x80 = alias)
    entry->entry_type = (*blockptr) & MVS_PDSDIR_ALIAS_MASK;
    // Get halfword count of user data
    user_data_count_hw =
        (*blockptr) & MVS_PDSDIR_USERDATA_COUNT_MASK;
    fprintf(stderr,
        "    Processing %s, entry_type = %d, ISPF user data has "
        "length of %d halfwords, byte-val 0x%02X\n",
        entry->name, entry->entry_type, user_data_count_hw,
        (*blockptr));
    blockptr++;

    // Now we get to the user data ... 0 to 62 bytes.
    pds_member_dump_userdata(
        entry, blockptr, user_data_count_hw * 2);
    pds_member_get_ispf_stats(
        entry, blockptr, user_data_count_hw * 2);

    // Point past user data and return the address ...
    // which will be the next member entry, or beyond the end
    // block.
    blockptr += user_data_count_hw * 2;
    return blockptr - save_blockptr;
}

int process_dir_block(unsigned char *dirblock) {

    int                   end_of_dir = 0;
    short                 bytes_used_in_block;
    short                 count;
    unsigned char *       blockptr;
    pds_member_entry_t    member_info;
    pds_member_entry_t *  member = &member_info;
    int                   len_entry;

    // First two bytes of block is the length used
    bytes_used_in_block = *( (short *)dirblock );
    fprintf(stderr,
        "Bytes used in block = %d\n",bytes_used_in_block);

    // Initialize pointer to past the two byte length used
    blockptr = &(dirblock[2]);

    count = 2;
    while ( blockptr < dirblock + bytes_used_in_block ) {

        if ( memcmp(blockptr, MVS_PDSDIR_ENDMARK, 8) == 0) {
            end_of_dir = 1;
            break;
        }

        pds_member_entry_init(member);
        len_entry = pds_member_entry_set(member, blockptr);
        dump(member);
        blockptr += len_entry;
    }

    return end_of_dir;
}

long mvs_pds_dir_read(unsigned char * pdsname) {

    FILE * fh = NULL;
    char   dirblock[256];
    int    quit;
    short  l;
    unsigned char open_dsn[200];

    strcpy(open_dsn, "//DSN:");
    strcat(open_dsn, pdsname);
    fh = fopen (open_dsn,
        "rb,klen=0,lrecl=256,blksize=256,recfm=u,force");
    if (fh == NULL) {
        perror("Open of dataset failed");
        return 12;
    }
    fprintf(stderr, "Dataset opened\n");
    fflush(stderr);

    fread (&l, 1, 2, fh); /* Skip U length */
    fprintf(stderr, "fread skip U length ... l=%d\n",l);
    fflush(stderr);

    quit = 0;
    while (fread (dirblock, 1, 256, fh) == 256) {

        if ( memcmp(dirblock, MVS_PDSDIR_ENDMARK, 8) == 0) {
            break;
        }
        quit = process_dir_block(dirblock);
        if (quit) break;

        fread (&l, 1, 2, fh); /* Skip U length */
        fprintf(stderr, "fread skip U length ... l=%d\n",l);
        fflush(stderr);
    }

exit_func:

    if (fh)
        fclose (fh);
    return 0;
}

int main(int argc, char **argv)
{
    struct tm target_time;
    unsigned char bcdvals[] = {0x01,0x09,0x19,0x29,0x99};
    int i;
    time_t   tval;
    int rc = 0;

    for (i = 0; i<sizeof(bcdvals); i++) {
        fprintf(stderr, ">>> 0x%02X = %d\n",
            bcdvals[i], bcd_byte_to_int(bcdvals[i]) );
    }
    fprintf(stderr, "sizeof(time_t) = %d\n", sizeof(time_t));

    //rc = mvs_pds_dir_read("TEMP.TESTPROJ.CNTL");
    rc = mvs_pds_dir_read("TEMP.ITEST.FB");
    return rc;
}
@@
//
