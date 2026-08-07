//TONYWZ1  JOB (MVSNFSD),
//             'Test membr write fb1',
//             CLASS=A,
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Testing file write to PDS members
//*
//********************************************************************
//TEST EXEC JCCCG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

int main(int argc, char **argv) {

    int        rc = 0;
    FILE      *ofh;
    char      *out_file_name = "//DSN:TEMP.TESTPROJ.OUTFB(TEST1)";
    size_t     bwritten;
    char       wbuff[256];
    int        i;

    ofh = fopen(out_file_name, "wt");
    if ( !ofh ) {
        perror("Open failed");
        return 8;
    }

    for ( i = 1; i <= 30; i++) {
        sprintf(wbuff, "This is line %04d\n", i);
        bwritten = fwrite(wbuff, strlen(wbuff), 1, ofh);
    }

    fclose(ofh);

    return rc;
}
@@
//
