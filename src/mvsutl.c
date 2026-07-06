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

