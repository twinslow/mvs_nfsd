//TONYWZ1  JOB (MVSNFSD),
//             'Test _getdcb',
//             CLASS=A,
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Test the __getdcb and __get_ddndsnmemb in JCC library
//*
//********************************************************************
//TEST EXEC JCCCG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdio.h>
#include <stdlib.h>
#include <io.h>

char *decode_dsorg(unsigned char dsorg) {
    unsigned char *printable;
    switch(dsorg) {
        case 0x40: printable = "PS";
            break;
        case 0x02: printable = "PO";
            break;
        default: printable = "?";
            break;
    }
    return printable;
}

char *decode_recfm(unsigned char recfm) {
    unsigned char *printable;
    switch(recfm) {
        case 0x40: printable = "V";
            break;
        case 0x50: printable = "VB";
            break;
        case 0x80: printable = "F";
            break;
        case 0x90: printable = "FB";
            break;
        case 0xC0: printable = "U";
            break;
        default: printable = "?";
            break;
    }
    return printable;
}

int main(int argc, char **argv) {

    unsigned char *dsname = "TEMP.TESTPROJ.C(HELLOW)";
    unsigned char dsorg, recfm, keylen;
    unsigned short lrecl, blksize;
    unsigned char dsname_buff[100];
    unsigned char ddname[9];
    unsigned char member[9];
    unsigned char volser[7];
    unsigned char dsn[45];
    unsigned char flags[11];

    int fhandle = -1;
    int rc = 0;

    strncpy(dsname_buff, "//DSN:", sizeof(dsname_buff));
    strncat(dsname_buff, dsname, sizeof(dsname_buff));

    fhandle = open(dsname_buff, O_RDONLY|O_BINARY, 0, "");
    if ( fhandle < 0 ) {
        perror("Open failed");
        rc = 8;
        goto exit_func;
    }

    rc = __getdcb(fhandle,
             &dsorg, &recfm, &keylen, &lrecl, &blksize);
    if ( rc < 0 ) {
        perror("__getdcb failed");
        rc = 8;
        goto exit_func;
    }

    fprintf(stdout, "DSNAME: %s -- "
         "DSORG=0x%02x/%s, RECFM=0x%02x/%s "
         "KEYLEN=%d, LRECL=%d, BLKSIZE=%d\n",
         dsname,
         dsorg, decode_dsorg(dsorg),
         recfm, decode_recfm(recfm),
         keylen, lrecl, blksize);

    /* Get info from the JFCB */
    rc = __get_ddndsnmemb(fhandle, ddname, dsn, member, volser, flags);
    if ( rc < 0 ) {
        perror("__get_ddndsnmemb failed");
        rc = 8;
        goto exit_func;
    }

    fprintf(stdout,
         "DSN=%s, MEMBER=%s, VOL=SER=%s, DDNAME=%s\n",
         dsn, member, volser, ddname);

exit_func:

    if ( fhandle >= 0 )
        close(fhandle);

    return rc;
}
@@
//
