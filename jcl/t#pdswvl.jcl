//TONYWZ1  JOB (MVSNFSD),
//             'Test membr write vb',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Writing to a RECFM-VB PDS, with long records
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
    char      *out_file_name = "//DSN:TEMP.TESTPROJ.VBOUT(TEST1)";
    char       out_buff[4001];
    int        i, len;

    for (i=0; i<sizeof(out_buff) - 1; i++)
        out_buff[i] = 'X';
    out_buff[i] = '\0';

    ofh = fopen(out_file_name, "wt");
    len = 80;

    fprintf(ofh,"\n");

    for (i=0; i<100; i++) {
        fprintf(ofh, "%05d %05d %s\n", i+1, len, &out_buff[4001-len]);
        fprintf(stderr, "Write record %d of length %d\n",
            i+1, len);
        len += 10;
    }

     fclose(ofh);

    return rc;
}

@@
//GO.STDERR DD  SYSOUT=*,DCB=(RECFM=F,BLKSIZE=133)
//
