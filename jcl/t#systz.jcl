//TONYWZ1  JOB (MVSNFSD),
//             'Test w+b open',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Determine system TZ
//*
//********************************************************************
//TEST EXEC JCCCLG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdlib.h>
#include <stdio.h>


int get_int_cvt_val(int cvt_offset)
{
    void **pcvt = (void **)0x00000010;
    unsigned char *cvt = *(unsigned char **)pcvt;
    int val;

    //val = *(int *)( cvt + 0x0130 );
    val = *(int *)( cvt + cvt_offset );
    fprintf(stderr, "cvt at 0x%08X, offset 0x%08X = %d\n",
        cvt, cvt_offset, val);
    return val;
}

/*
 * This functions rounds to the qtr hour, if it is just 1 second off
 */
static int round_to_qtr_hour(int n) {
    int r = n % 900;
    if (r < 0) r += 900;   /* C's % is truncating, not floor-mod */
    if (r == 1)    return n - 1;
    if (r == 899) return n + 1;
    return n;
}

int get_tz_offset()
{
    int seconds;
    long long offset;

    offset = get_int_cvt_val(0x0130);

    fprintf(stderr, "offset (tod ticks) %d\n",offset);

    seconds = (int) (1048567LL * offset / 1000000LL);

    fprintf(stderr, "seconds=%d\n", seconds);
    // Round to half hour if it within 1 second
    return round_to_qtr_hour(seconds);
}


int main(int argc, char **argv) {
    int offset;
    int tests[] = { 3598, 3599, 3600, 3601, 3602, 3623,
                   -3598,-3599,-3600,-3601,-3602,-3623,
                    7198, 7199, 7200, 7201, 7202, 7211,
                   -7198,-7199,-7200,-7201,-7202,-7211,
                    19798, 19799, 19800, 19801, 19802, 19823,
                    20698, 20699, 20700, 20701, 20702, 20723,
                   -19798,-19799,-19800,-19801,-19802,-19823,
                    86398, 86399, 86400, 86401, 86402, 86411,
                   -86398,-86399,-86400,-86401,-86402,-86411,
                   0};
    int i;

    offset = get_tz_offset();
    fprintf(stderr,
        "System TZ offset in seconds = %d\n", offset);

    i = 0;
    while (tests[i] != 0) {
        fprintf(stderr, "Test rounding of %d = %d\n",
            tests[i], round_to_qtr_hour(tests[i]));
        i++;
    }

    return 0;
}

@@
//GO.STDERR DD  SYSOUT=*,DCB=(RECFM=F,BLKSIZE=133)
//
