#include <stdio.h>
#include <string.h>

#include "mvsutl.h"

typedef unsigned char  BYTE;
typedef unsigned int   ADDR31;   /* S/370 addresses fit in 4 bytes */

/*
 * Read a 31-bit pointer field stored at absolute address 'addr'.
 * MVS pointer fields sometimes have flag bits in the high byte
 * (e.g. AVT slot in-use bit), so mask to 31 bits to be safe.
 */
static void *fetch_ptr(ADDR31 addr)
{
    ADDR31 raw = *(volatile ADDR31 *)addr;
    return (void *)(raw & 0x7FFFFFFFu);
}

/*
 * get_jes2_jobid()
 *
 * Walks PSA -> TCB -> JSCB -> SSIB for the CURRENTLY EXECUTING TCB
 * and extracts the JES2-assigned job identifier.
 *
 *   PSA + X'21C' (540) -> current TCB     (PSATOLD)
 *   TCB + X'B4'        -> JSCB            (TCBJSCB)
 *   JSCB + X'13C'       -> SSIB            (JSCBSSIB)
 *   SSIB + X'0C', 8 bytes -> job id, e.g. "JOB01234"
 *
 * Returns a static char output buffer pointer, or
 * NULL if any pointer in the chain is null.
 */
char *get_jes2_jobid()
{
    static char out_jobid[9];
    BYTE *tcb, *jscb, *ssib;

    tcb = fetch_ptr(540);
    if (tcb == NULL) return NULL;

    jscb = fetch_ptr((ADDR31)(tcb + 0xB4));
    if (jscb == NULL) return NULL;

    ssib = fetch_ptr((ADDR31)(jscb + 0x13C));
    if (ssib == NULL) return NULL;

    memcpy(out_jobid, ssib + 0x0C, 8);
    out_jobid[8] = '\0';

    return &out_jobid[0];
}

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

