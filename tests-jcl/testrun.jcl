//TONYWUT1 JOB (NFSD),
//            'Test dino-nfs mods',
//            CLASS=A,NOTIFY=TONYW,
//            MSGCLASS=X,
//            REGION=8M,
//            MSGLEVEL=(1,1)
//********************************************************************
//*
//* Name: TESTRUN
//*
//* Desc: Build and run the mUnit tests for NFSD server
//*
//********************************************************************
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
//********************************************************************
//*
//* Compile application modules
//*
//********************************************************************
//MVSIO    EXEC JCCCMOD,MODNAME=MVSIO
//*
//********************************************************************
//*
//* Compile unit test modules
//*
//********************************************************************
//TSTUBS   EXEC JCCCTST,MODNAME=TSTUBS
//TMVSIO   EXEC JCCCTST,MODNAME=TMVSIO
//TMVSIO2  EXEC JCCCTST,MODNAME=TMVSIO2
//*
//********************************************************************
//*
//* Compile, link and run tests
//*
//********************************************************************
//JCCCLG  EXEC JCCCLG,INFILE='TONYW.DINONFS.TESTS.C(RUNALL)',
//             JOPTS='-D_MVS -D__MVS__ -o -list=//DDN:SYSPRINT'
//COMPILE.JCCINCS DD DISP=SHR,DSN=SYSD.MUNIT.H
//          DD DISP=SHR,DSN=TONYW.DINONFS.H
//          DD DISP=SHR,DSN=TONYW.DINONFS.TESTS.H
//COMPILE.SYSPRINT DD SYSOUT=*
//PRELINK.I DD DISP=SHR,DSN=SYSD.MUNIT.OBJLIB(MUNIT)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSIO)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TSTUBS)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSIO)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSIO2)
//          DD DSN=&&OBJ,DISP=(OLD,DELETE)
//
