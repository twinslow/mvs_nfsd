//TONYWZ1  JOB (DINO),
//             'Test w+b open',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* STAE abend trap
//*
//********************************************************************
//TEST EXEC JCCCG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdlib.h>
#include <stdio.h>
#include <mvsutils.h>
#include <setjmp.h>

/* 
   Opens `filename`, writes `num_lines` numbered lines to it, 
   then closes it.
   Returns 0 on success, -1 on failure. 
*/

int write_lines(const char *filename, int num_lines) 
{
    FILE   *fp;
    char    linecontent[256];
    int     i;

    fp = fopen(filename, "w");
    if (fp == NULL) {
        perror("fopen");
        return -1;
    }

    strcpy(linecontent,"AAAA-AAAA-BBBB-BBBB-CCCC-CCCC-DDDD-DDDD"
                       "EEEE-EEEE-FFFF-FFFF|");

    for (i = 1; i <= num_lines; i++) {        
        fprintf(fp, "%d - %s\n", i, linecontent);
    }

    fclose(fp);
    return 0;
}

int protected_write (const char *filename, int num_lines) 
{                                                                   
    long  * a; // To cause an abend      
    long    rc;                   
    int     func_rc;
    jmp_buf b;
    int     i; 
    char    abend_code[4];

    // System Diagnostic Work Area
    // Mapped byt IHASDWA macro
    unsigned int  sdwa[26];
    unsigned char *sdwa104 = (unsigned char *)&sdwa; 

    func_rc = -3;
    rc = _setjmp_stae (b, sdwa104); // sdwa104 will contain abend info
    switch(rc) {
        case 0:
            func_rc = write_lines(filename, num_lines);
            rc = _setjmp_canc (); // Cancel the last STAE in the OS  
            if ( rc != 0 ) 
                printf ("setjmp_canc rc=%d\n", rc);             
            break;

        case 1:
            sprintf(abend_code, "%03X",
                (sdwa[1] & 0x00FFF000) >> 12);
            printf("ERROR: The write_lines function abended - %s\n", 
                abend_code); 

            for (i=0; i<4; i++) {
                printf("SDWA[%d] 0x%08X\n", i, sdwa[i]);
            }

            func_rc = -2;
            break;

        default:
            printf ("ERROR: setjmp couldn't create STAE\n");                       
            break;
    }

    return func_rc;                            
}                          

int main(int argc, char **argv) {
    int i;
    int rc;
    char filename[100];

    for (i=0; i<10; i++) {
        sprintf(filename, "//DSN:TEMP.TESTPROJ.OUTPDS(M%05d)", i);
        rc = protected_write(filename, 400);
        if ( rc < 0 ) {
            break;
        }
    }
    return 0;
}

@@
//GO.STDERR DD  SYSOUT=*,DCB=(RECFM=F,BLKSIZE=133)
//
