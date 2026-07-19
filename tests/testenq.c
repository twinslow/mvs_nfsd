#include <stdio.h>
#include <process.h>   /* for Sleep() */

#include "types.h"
#include "asmutils.h"




int test(int enqcnt) {
    char    *qname = "TESTENQ";
    char     rname[55]; /* DSN + (member) + \0 */
    int      rc;

    sprintf(rname, "TESTING.MCTEST(N%03d)", enqcnt);

    rc = mvs_enq(
        MVS_ENQ_REQ_ENQTEST, MVS_ENQ_OPT_EXC, 
        qname, rname);

    fprintf(stderr, "TESTENQ-SHR Q=%-8.8s R=%-55.55s RC=%d\n",
        qname, rname, rc);

    return rc;
}

int enq(int enqcnt) {
    char    *qname = "TESTENQ";
    char     rname[55]; /* DSN + (member) + \0 */
    int      rc;
    
    sprintf(rname, "TESTING.MCTEST(N%03d)", enqcnt);

    rc = mvs_enq(
        MVS_ENQ_REQ_ENQ, MVS_ENQ_OPT_EXC, 
        qname, rname);

    fprintf(stderr, "ENQ-SHR     Q=%-8.8s R=%-55.55s RC=%d\n",
        qname, rname, rc);

    return rc;
}

int deq(int deqcnt) {
    char    *qname = "TESTENQ";
    char     rname[55]; /* DSN + (member) + \0 */
    int      rc;

    sprintf(rname, "TESTING.MCTEST(N%03d)", deqcnt);
    
    rc = mvs_enq(
        MVS_ENQ_REQ_DEQ, 0,
        qname, rname);

    fprintf(stderr, "DEQ         Q=%-8.8s R=%-55.55s RC=%d\n",
        qname, rname, rc);

    return rc;
}


int main(argc, argv) {
    int rc;


    int      enqcnt = 1;
    int      deqcnt = -4;
    int      tstcnt = -4;

    while (deqcnt <= 20) {
        if ( tstcnt > 0 )
            test(tstcnt);

        if ( tstcnt > 3 )
            test(tstcnt - 2);

        if ( enqcnt > 0 ) 
            enq(enqcnt);

        if ( deqcnt > 0 )
            deq(deqcnt);

        Sleep(2000);  /* Sleep for a bit */
        enqcnt++;
        deqcnt++;
        tstcnt++;
    }

    return 0;
}