//TONYWUX1 JOB (NFSD),
//            'Test exports parser',
//            CLASS=A,NOTIFY=TONYW,REGION=8M,
//            MSGCLASS=X,MSGLEVEL=(1,1),
//            COND=(0,LT)
//*
//********************************************************************
//*
//* Name: TESTEXP
//*
//* Desc: Build and run the mUnit tests for exports.c (the NFSDCONF
//*       parser + export table).
//*
//*       This is a SEPARATE job from testrun.jcl on purpose: the
//*       exports tests link the REAL exports.c, whereas every other
//*       test links tests/tstubs.c, which REPLACES exports.c and
//*       defines the same symbols.  The two cannot coexist in one
//*       load module, so the exports suite is its own program with
//*       its own main() (tests/texports.c).
//*
//*       Linked: EXPORTS + CFGOPTS + EBCDIC + LOGGER + MUNIT + the
//*       test module TEXPORTS.  mvs_get_dcb_info_dsn() is stubbed
//*       inside TEXPORTS, so MVSIO is NOT linked.
//*
//********************************************************************
//*
//JCCCMOD PROC SOUT='*',JCC='JCC',
//          INFILE='TONYW.DINONFS.C',
//          OBJFILE='TONYW.DINONFS.OBJLIB',
//          JOPTS='-o',
//          MODNAME='NOT-SPECIFIED'
//*
//COMPILE  EXEC PGM=JCC,
//         PARM='-I//DDN:JCCINCL //DDN:SYSIN &JOPTS -D__MVS__'
//STEPLIB  DD   DSN=&JCC..LINKLIB,DISP=SHR
//SYSPRINT DD   SYSOUT=&SOUT
//JCCINCL  DD   DSN=&JCC..INCLUDE,DISP=SHR
//JCCINCS  DD   DISP=SHR,DSN=TONYW.DINONFS.H
//JCCOUTPT DD   UNIT=SYSDA,SPACE=(TRK,(50,20)),DISP=(,DELETE),
//         DSN=&&OUTPT
//STDOUT   DD   SYSOUT=&SOUT
//JCCOASM  DD   DISP=SHR,DSN=&OBJFILE(&MODNAME)
//SYSIN    DD   DSN=&INFILE(&MODNAME),DISP=SHR
//*
//JCCCMOD  PEND
//*
//JCCCTST PROC SOUT='*',JCC='JCC',
//          INFILE='TONYW.DINONFS.TESTS.C',
//          OBJFILE='TONYW.DINONFS.OBJLIB',
//          JOPTS='-o -D_MVS -D__MVS__',
//          MODNAME='NOT-SPECIFIED'
//*
//COMPILE  EXEC PGM=JCC,
//         PARM='-I//DDN:JCCINCL //DDN:SYSIN &JOPTS'
//STEPLIB  DD   DSN=&JCC..LINKLIB,DISP=SHR
//SYSPRINT DD   SYSOUT=&SOUT
//JCCINCL  DD   DSN=&JCC..INCLUDE,DISP=SHR
//JCCINCS  DD   DISP=SHR,DSN=SYSD.MUNIT.H
//         DD   DISP=SHR,DSN=TONYW.DINONFS.H
//         DD   DISP=SHR,DSN=TONYW.DINONFS.TESTS.H
//JCCOUTPT DD   UNIT=SYSDA,SPACE=(TRK,(50,20)),DISP=(,DELETE),
//         DSN=&&OUTPT
//STDOUT   DD   SYSOUT=&SOUT
//JCCOASM  DD   DISP=SHR,DSN=&OBJFILE(&MODNAME)
//SYSIN    DD   DSN=&INFILE(&MODNAME),DISP=SHR
//*
//JCCCTST  PEND
//*
//********************************************************************
//*
//* Compile the production modules under test + their deps
//*
//********************************************************************
//EXPORTS  EXEC JCCCMOD,MODNAME=EXPORTS
//CFGOPTS  EXEC JCCCMOD,MODNAME=CFGOPTS
//EBCDIC   EXEC JCCCMOD,MODNAME=EBCDIC
//LOGGER   EXEC JCCCMOD,MODNAME=LOGGER
//*
//********************************************************************
//*
//* Compile the test module (has its own main)
//*
//********************************************************************
//TEXPORTS EXEC JCCCTST,MODNAME=TEXPORTS
//*
//********************************************************************
//*
//* Compile TEXPORTS's main, then link + run the standalone program
//*
//********************************************************************
//JCCCLG  EXEC JCCCLG,INFILE='TONYW.DINONFS.TESTS.C(TEXPORTS)',
//             PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',
//             JOPTS='-D_MVS -D__MVS__ -o -list=//DDN:SYSPRINT'
//COMPILE.JCCINCS DD DISP=SHR,DSN=SYSD.MUNIT.H
//          DD DISP=SHR,DSN=TONYW.DINONFS.H
//          DD DISP=SHR,DSN=TONYW.DINONFS.TESTS.H
//COMPILE.SYSPRINT DD SYSOUT=*
//PRELINK.I DD DISP=SHR,DSN=SYSD.MUNIT.OBJLIB(MUNIT)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(LOGGER)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(CFGOPTS)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(EBCDIC)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(EXPORTS)
//          DD DSN=&&OBJ,DISP=(OLD,DELETE)
//LKED.SYSLIN DD
//          DD DDNAME=SYSIN
//LKED.SYSIN DD *
//LKED.SYSLIB DD DISP=SHR,DSN=TONYW.DINONFS.LOAD
//
