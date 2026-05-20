//TONYWZ1  JOB (DINO),
//             'Test _getdcb',
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
//TEST EXEC JCCCG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdio.h>
#include <time.h>

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

time_t alt_mkt(struct tm *time_in) {

    time_t tval;
    tval = ispf_tm_to_timet(
                time_in->tm_year + 1900,
                time_in->tm_mon,
                time_in->tm_mday,
                time_in->tm_hour,
                time_in->tm_min,
                time_in->tm_sec
            );
    return tval;
}

int main(int argc, char **argv)
{
    struct tm target_time;
    time_t   tval;
    int rc = 0;
    unsigned char dt[30];

    fprintf(stderr, "sizeof(time_t) = %d\n", sizeof(time_t));

    memset(&target_time, 0, sizeof(target_time));
    target_time.tm_year = 126;
    target_time.tm_mday = 25;
    target_time.tm_mon = 4;
    target_time.tm_hour = 13;
    target_time.tm_min  = 14;
    target_time.tm_sec  = 15;
    target_time.tm_isdst = 0;
    target_time.tm_zone = "UTC";

    fprintf(stderr,
        "tm_year=%d, tm_mon=%d, tm_mday=%d, "
        "tm_hour=%d, tm_min=%d, tm_sec=%d \n",
        target_time.tm_year,
        target_time.tm_mon,
        target_time.tm_mday,
        target_time.tm_hour,
        target_time.tm_min,
        target_time.tm_sec
    );

    tval = alt_mkt(&target_time);
    //tval = ispf_tm_to_timet(2026, 4, 25, 13, 14, 15);

    strftime(dt, sizeof(dt),
        "%Y-%m-%d %H:%M:%S",
        gmtime(&tval));

    fprintf(stderr, "Formatted time : %s\n", dt);

    return rc;
}
@@
//
