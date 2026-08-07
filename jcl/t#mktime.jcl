//TONYWZ1  JOB (MVSNFSD),
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

int main(int argc, char **argv)
{
    struct tm target_time;
    time_t   tval;
    int rc = 0;

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

    tval = mktime(&target_time);

    return rc;
}
@@
//
