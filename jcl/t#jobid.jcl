//TONYWZ1  JOB (MVSNFSD),
//             'Get JES2 jobid',
//             CLASS=A,
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Get JES2 job id
//*
//********************************************************************
//TEST EXEC JCCCG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdio.h>
#include <string.h>

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
 * 'out' must point to a buffer of at least 9 bytes.
 * Returns 0 on success, -1 if any pointer in the chain is null.
 */
int get_jes2_jobid(char *out)
{
    BYTE *tcb, *jscb, *ssib;

    tcb = fetch_ptr(540);
    if (tcb == NULL) return -1;

    jscb = fetch_ptr((ADDR31)(tcb + 0xB4));
    if (jscb == NULL) return -1;

    ssib = fetch_ptr((ADDR31)(jscb + 0x13C));
    if (ssib == NULL) return -1;

    memcpy(out, ssib + 0x0C, 8);
    out[8] = '\0';

    return 0;
}

int main(int argc, char **argv) {

    char       jobid[9];

    get_jes2_jobid(jobid);
    jobid[8] = '\0';
    printf("This job has %s\n", jobid);
    return 0;

}
@@
//
