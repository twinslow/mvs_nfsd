//TONYWZ1  JOB (DINO),
//             'Test w+b open',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Does a SINGLE "w+b"+DCB fopen create a scratch dataset AND then
//* support random fseek/fwrite/fread on the same handle?  If so the
//* spill design (doc/design_nfs_write.md Sec 8) can use one open
//* instead of the proven create("wb")-then-reopen("r+b") pair.
//*
//* PASS => rc 0, FAIL => rc 8 (see STDERR for the details).
//*
//********************************************************************
//TEST EXEC JCCCG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdlib.h>
#include <stdio.h>
#include <mvsutils.h>

int myfunc2 () {                                                                   
    long  * a; // To cause an abend                                                
    long    i;                                                                     
    jmp_buf b;                                                                     
                                                                                   
    i = _setjmp_stae (b, NULL); // We don't want 104 bytes of abend data           
    if (i == 0) { // Normal return                                                 
                                                                                   
        printf ("1st setjmp return\n");                                            
                                                                                   
        a = NULL;                                                                  
        *a = 0; // Cause an abend!  The following 2 lines will not be executed     
                                                                                   
        i = _setjmp_canc (); // Cancel the last STAE in the OS                     
        printf ("setjmp_canc rc=%d\n", i);                                         
                                                                                   
    } else if (i == 1) { // Something was caught - the STAE has been cleaned up.   
                                                                                   
        printf ("2nd setjmp return, abend caught\n");                              
                                                                                   
        //*a = 0; // Cause an abend! (for real, if you want to test it.)           
                                                                                   
    } else { // can only be -1 = OS failure                                        
                                                                                   
        printf ("setjmp couldn't create STAE\n");                                  
    }                                                                              
    return (0);                                                                    
}                                                                                  

int main(int argc, char **argv) {

    myfunc2();

    return 0;
}
@@
//GO.STDERR DD  SYSOUT=*,DCB=(RECFM=F,BLKSIZE=133)
//
