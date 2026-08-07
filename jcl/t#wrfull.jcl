//TONYWZ1  JOB (DINO),
//             'Test membr write vb',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Writing to a full PDS -- does it cause a hang in JCC IO
//* when abend is caught using STAE.
//*
//********************************************************************
//TESTENQ EXEC JCCCL,
//        PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',
//        OUTFILE='TONYW.NFSD.LOAD',
//        JOPTS='-s -o -LIST=//DDN:SYSPRINT -D__MVS__'
//COMPILE.JCCINCS DD DISP=SHR,DSN=TONYW.NFSD.H
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <mvsutils.h>
#include <setjmp.h>

#include "asmutils.h"

#define write_to_member W2MEMBR
#define protected_write PRTWRITE


static char g_wto[121];

void write_to_member(const char *out_file_name, int line_cnt) {

    FILE      *ofh;
    int        i;

    sprintf(g_wto, "About to open %s and write %d lines",
        out_file_name, line_cnt);
    _write2op(g_wto);

    ofh = fopen(out_file_name, "wt");

    sprintf(g_wto, "Returned from open ofh = 0x%08x", ofh);
    _write2op(g_wto);

    for (i = 0; i < line_cnt; i++)
        fprintf(ofh, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n");

    fclose(ofh);
}

int dir_read(const char *pds_name) {

    /* Read the PDS directory by DSN before writing the member.     */
    /* pdsflush_slot() does exactly this, via                       */
    /* mvs_pds_get_member_entry(), between the SVC 99 allocation    */
    /* and the //DDN: open for output.                              */
    /*                                                              */
    /* It matters because it leaves the dataset allocated TWICE at  */
    /* the same time: our own SVC 99 allocation, plus the one the   */
    /* C library makes for this //DSN: open.  That is the state the */
    /* server is always in at the moment of the abend, and the one  */
    /* this program has never been in.                              */
    /*                                                              */
    /* OUTSIDE the STAE, because the server's directory read is     */
    /* outside its own -- only the write is guarded.                */
    /*                                                              */
    /* SUCCESS SIGNAL: one IEC032I E37-04, not two.  Every hang so  */
    /* far has had exactly one; this program has always had two.    */
    {
        FILE *dfh;
        char  dsn[60];
        char  blk[256];
        int   len;

        sprintf(dsn, "//DSN:%s", pds_name);
        dfh = fopen(dsn,
            "rb,klen=0,lrecl=256,blksize=256,recfm=u,force");
        sprintf(g_wto, "dir read %s, dfh = 0x%08x", dsn, dfh);
        _write2op(g_wto);
        if (dfh != NULL) {
            /* Walk it, as mvs_read_pds_dir does -- opening alone   */
            /* may not put the dataset in the same state.           */
            while (len = fread(blk, 1, sizeof(blk), dfh) == sizeof(blk)) {
                sprintf(g_wto, "   Dir read ... len = %d", len);
                _write2op(g_wto);
            }
            fclose(dfh);
            _write2op("dir read closed");
        }
    }


}

int protected_write (const char *pds_name, const char *member_name,
                     int line_cnt)
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
    char ddname[9];
    char fn_buff[60];
    char wto[120];

    rc = mvs_dynalloc(MVS_DYNALLOC_REQ_ALLOC, 0,
                      pds_name, member_name, ddname);
    sprintf(wto, "mvs_dynalloc returned rc = %d", rc);
    _write2op(wto);
    if ( rc != 0 )
        return -16;

    ddname[8] = '\0';
    for (i=7; ddname[i] == ' '; i--) ddname[i] = '\0';

    sprintf(fn_buff, "//DDN:%s", ddname);
    _write2op(fn_buff);

    func_rc = -3;
    rc = _setjmp_stae (b, sdwa104); // sdwa104 will contain abend info

    sprintf(g_wto, "_setjmp_stae() rc = %d", rc);
    _write2op(g_wto);

    dir_read(pds_name);

    switch(rc) {
        case 0:
            sprintf(wto,
                "About to call write_to_member() - %s DD DSN=%s(%s)",
                ddname, pds_name, member_name);
            _write2op(wto);
            write_to_member(fn_buff, line_cnt);
            sprintf(wto,
                "Returned from write_to_member()"
                " no abend - %s DD DSN=%s(%s)",
                ddname, pds_name, member_name);
            _write2op(wto);
            rc = _setjmp_canc (); // Cancel the last STAE in the OS
            if ( rc != 0 )
                printf ("setjmp_canc rc=%d\n", rc);
            break;

        case 1:
            sprintf(wto,
                "Abend caught - %s DD DSN=%s(%s)",
                ddname, pds_name, member_name);
            _write2op(wto);
            sprintf(abend_code, "%03X",
                (sdwa[1] & 0x00FFF000) >> 12);
            sprintf(g_wto,
                "ERROR: The write_to_member(%s) abended - %s\n",
                fn_buff, abend_code);
            _write2op(g_wto);

            func_rc = -2;
            break;

        default:
            _write2op("ERROR: setjmp couldn't create STAE");
            func_rc = -3;
            break;
    }

    rc = mvs_dynalloc(MVS_DYNALLOC_REQ_UNALLOC, 0,
                      pds_name, member_name, NULL);
    sprintf(wto,
        "mvs_dynalloc UNALLOC %s, rc = %d",
        ddname, rc);
    _write2op(wto);

    return func_rc;
}

int main(int argc, char **argv) {

    int        rc = 0;
    FILE      *ofh;
    char      *out_file_name = "//DSN:TEMP.ITEST.FBSMALL(XXX)";
    char       buff[120];
    int        i, len;

    _write2op("About to call #1 protected_write() to FBSMALL");
    rc = protected_write("TEMP.ITEST.FBSMALL", "XXX", 1);
    sprintf(buff, "Return from protected_write() rc = %d", rc);
    _write2op(buff);

    dir_read("TEMP.ITEST.FBSMALL");

    _write2op("About to call #2 protected_write(YYY) to FBSMALL");
    protected_write("TEMP.ITEST.FBSMALL", "YYY", 50);
    _write2op("Returned from protected_write() #2");

    dir_read("TEMP.ITEST.FBSMALL");

    _write2op("About to call #3 write_to_member(ZZZ) to FB");
    write_to_member("//DSN:TEMP.ITEST.FB(ZZZ)", 1);
    _write2op("Returned from write_to_member() #3");

    dir_read("TEMP.ITEST.FB");
    return 100 + rc;
}

@@
//LKED.SYSLIN DD DSN=&&OBJMOD,DISP=(OLD,DELETE)
//          DD  DDNAME=SYSIN
//LKED.SYSLIB DD DISP=SHR,DSN=TONYW.NFSD.LOAD
//LKED.SYSIN DD *
    INCLUDE SYSLMOD(MVSDALC)
    NAME T#WRFULL(R)
//*
//*--------------------------------------------------------
//RUN      EXEC PGM=T#WRFULL
//STEPLIB   DD  DISP=SHR,DSN=TONYW.NFSD.LOAD
//STDOUT    DD  SYSOUT=*,DCB=(RECFM=F,BLKSIZE=133)
//STDERR    DD  SYSOUT=*,DCB=(RECFM=F,BLKSIZE=133)
//STDIN     DD  DUMMY
//