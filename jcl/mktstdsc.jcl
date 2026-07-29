//TONYWD1 JOB (NFSD),'MAKE TEST DSCB',
//      CLASS=A,MSGCLASS=X,MSGLEVEL=1,REGION=8M,
//      COND=(0,LT),
//      NOTIFY=TONYW
//*
//********************************************************************
//*
//* NAME: MKTSTDSC
//*
//* DESC: COMPILE, LINK AND GO test that calls MVSDSCB
//*
//*       Assembles MVSDSCB, compiles TESTDSCB, links them together
//*       and runs the result.
//*
//*       Dataset names to look up are given on the RUN step PARM as
//*       a comma separated list, up to 16 of them.  Leave the PARM
//*       off to use the driver's own built-in list.
//*
//*       Suggested first run, in increasing order of difficulty:
//*         a single extent dataset  - proves the field mapping
//*         a multi extent PDS       - proves the extent walk
//*         one with MORE than three extents - proves the format 3
//*                                    path, which uses CAMLST SEEK
//*                                    and so returns the DSCB key as
//*                                    well as the data
//*         a name that does not exist - must come back as status 8
//*                                    with the loop still continuing
//*
//*       Cross check the answers with ISPF 3.2 / 3.4, or LISTVTOC.
//*
//********************************************************************
//*-------------------------------------------------------------------
//* Proc to compile a C module using JCC into object code
//*-------------------------------------------------------------------
//JCCCMOD PROC SOUT='*',JCC='JCC',
//          INFILE='TONYW.DINONFS.C',
//          OBJFILE='TONYW.DINONFS.OBJLIB',
//          HDRFILE='TONYW.DINONFS.H',
//          JOPTS='-o -LIST=//DDN:SYSPRINT -D__MVS__',
//          MODNAME='NOT-SPECIFIED'
//*
//COMPILE  EXEC PGM=JCC,
//         PARM='-I//DDN:JCCINCL //DDN:SYSIN &JOPTS'
//STEPLIB  DD   DSN=&JCC..LINKLIB,DISP=SHR
//SYSPRINT DD   SYSOUT=&SOUT
//JCCINCL  DD   DSN=&JCC..INCLUDE,DISP=SHR
//JCCINCS  DD   DISP=SHR,DSN=&HDRFILE
//JCCOUTPT DD   UNIT=SYSDA,SPACE=(TRK,(50,20)),DISP=(,DELETE),
//         DSN=&&OUTPT
//STDOUT   DD   SYSOUT=&SOUT
//JCCOASM  DD   DISP=SHR,DSN=&OBJFILE(&MODNAME)
//SYSIN    DD   DSN=&INFILE(&MODNAME),DISP=SHR
//*
//JCCCMOD  PEND
//*
//*-------------------------------------------------------------------
//* Proc to assemble a module using ASMF into the load library
//*-------------------------------------------------------------------
//ASMMOD   PROC SOUT='*',
//          ASMOPTS='OBJ,NODECK',
//          MAC='SYS1.MACLIB',
//          MAC1='SYS1.AMODGEN',
//          MAC2='TONYW.DINONFS.ASM',
//          MAC3='SYS2.MACLIB',
//          SRCLIB='TONYW.DINONFS.ASM',
//          LOADLIB='TONYW.DINONFS.LOAD',
//          MODULE='XXXXX'
//*
//ASM      EXEC PGM=IFOX00,PARM=(&ASMOPTS),REGION=128K
//SYSLIB   DD   DSN=&MAC,DISP=SHR
//         DD   DSN=&MAC1,DISP=SHR
//         DD   DSN=&MAC2,DISP=SHR
//         DD   DSN=&MAC3,DISP=SHR
//SYSUT1   DD   DSN=&&SYSUT1,UNIT=SYSSQ,SPACE=(1700,(600,100)),
//             SEP=(SYSLIB)
//SYSUT2   DD   DSN=&&SYSUT2,UNIT=SYSSQ,SPACE=(1700,(300,50)),
//             SEP=(SYSLIB,SYSUT1)
//SYSUT3   DD   DSN=&&SYSUT3,UNIT=SYSSQ,SPACE=(1700,(300,50))
//SYSPRINT DD   SYSOUT=&SOUT,DCB=BLKSIZE=1089
//SYSPUNCH DD   SYSOUT=&SOUT
//SYSGO    DD   DSN=&&OBJSET,UNIT=SYSSQ,SPACE=(80,(200,50)),
//             DISP=(MOD,PASS)
//SYSIN    DD   DISP=SHR,DSN=&SRCLIB(&MODULE)
//*
//LKED     EXEC PGM=IEWL,PARM=(XREF,LET,LIST,NCAL),REGION=128K,
//             COND=(8,LT,ASM)
//SYSLIN   DD   DSN=&&OBJSET,DISP=(OLD,DELETE)
//*         DD   DDNAME=SYSIN
//SYSUT1   DD   DSN=&&SYSUT1,UNIT=(SYSDA,SEP=(SYSLIN,SYSLMOD)),
//             SPACE=(1024,(50,20))
//SYSPRINT DD   SYSOUT=&SOUT
//SYSLMOD  DD   DISP=SHR,DSN=&LOADLIB(&MODULE)
//*
//ASMMOD   PEND
//*
//*-------------------------------------------------------------------
//* Assemble/compile the modules we need
//*-------------------------------------------------------------------
//*
//MVSDSCB  EXEC ASMMOD,MODULE=MVSDSCB
//*
//MVSUTL   EXEC JCCCMOD,MODNAME=MVSUTL
//*
//*-------------------------------------------------------------------
//* Now build the driver and link in what we need
//*-------------------------------------------------------------------
//TESTDSCB EXEC JCCCL,INFILE='TONYW.DINONFS.TESTS.C(TESTDSCB)',
//        PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',
//        OUTFILE='TONYW.DINONFS.LOAD',
//        JOPTS='-s -o -LIST=//DDN:SYSPRINT -D__MVS__'
//COMPILE.JCCINCS DD DISP=SHR,DSN=TONYW.DINONFS.H
//PRELINK.I DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSUTL)
//*         DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(LOGGER)
//          DD DSN=&&OBJ,DISP=(OLD,DELETE)
//LKED.SYSLIN DD DSN=&&OBJMOD,DISP=(OLD,DELETE)
//          DD  DDNAME=SYSIN
//LKED.SYSLIB DD DISP=SHR,DSN=TONYW.DINONFS.LOAD
//LKED.SYSIN DD *
    INCLUDE SYSLMOD(MVSDSCB)
    NAME TESTDSCB(R)
//*
//*--------------------------------------------------------
//* Adjust the PARM to name datasets that exist on this system.
//* An empty PARM makes the driver use its built-in list.
//*--------------------------------------------------------
//RUN      EXEC PGM=TESTDSCB,
//         PARM=('SYS1.MACLIB','TEMP.ITEST.FB',
//             'TEMP.ITEST.FBSMALL','INT.SYMLIB',
//             'CBTCOV.FILE186')
//STEPLIB   DD  DISP=SHR,DSN=TONYW.DINONFS.LOAD
//STDERR    DD  SYSOUT=*
//STDOUT    DD  SYSOUT=*
//STDIN     DD  DUMMY
//
