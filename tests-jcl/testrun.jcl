//TONYWUT1 JOB (NFSD),
//            'Test dino-nfs mods',
//            CLASS=A,NOTIFY=TONYW,REGION=8M,
//            MSGCLASS=X,MSGLEVEL=(1,1),
//            COND=(0,LT)
//*
//********************************************************************
//*
//* Name: TESTRUN
//*
//* Desc: Build and run the mUnit tests for NFSD server
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
//********************************************************************
//*
//* Compile application modules
//*
//********************************************************************
//EBCDIC   EXEC JCCCMOD,MODNAME=EBCDIC                                
//MVSDOL   EXEC JCCCMOD,MODNAME=MVSDOL                                
//MVSFSZ   EXEC JCCCMOD,MODNAME=MVSFSZ                                
//MVSIO    EXEC JCCCMOD,MODNAME=MVSIO                                 
//MVSPDIR  EXEC JCCCMOD,MODNAME=MVSPDIR
//MVSPRF   EXEC JCCCMOD,MODNAME=MVSPRF
//MVSPRW   EXEC JCCCMOD,MODNAME=MVSPRW
//*                                                                   
//********************************************************************
//*                                                                   
//* Compile unit test modules                                         
//*                                                                   
//********************************************************************
//TSTUBS   EXEC JCCCTST,MODNAME=TSTUBS                                
//TMVSDOL  EXEC JCCCTST,MODNAME=TMVSDOL                               
//TMVSFSZ  EXEC JCCCTST,MODNAME=TMVSFSZ                               
//TMVSIO   EXEC JCCCTST,MODNAME=TMVSIO                                
//TMVSIO2  EXEC JCCCTST,MODNAME=TMVSIO2                               
//TMVSPDIR EXEC JCCCTST,MODNAME=TMVSPDIR                               
//TMVSPRW  EXEC JCCCTST,MODNAME=TMVSPRW
//TMVSPRF  EXEC JCCCTST,MODNAME=TMVSPRF
//*                                                                   
//********************************************************************
//*                                                                   
//* Compile RUNALL, then link and run tests                           
//*                                                                   
//********************************************************************
//JCCCLG  EXEC JCCCLG,INFILE='TONYW.DINONFS.TESTS.C(RUNALL)',         
//             JOPTS='-D_MVS -D__MVS__ -o -list=//DDN:SYSPRINT'       
//COMPILE.JCCINCS DD DISP=SHR,DSN=SYSD.MUNIT.H                        
//          DD DISP=SHR,DSN=TONYW.DINONFS.H                           
//          DD DISP=SHR,DSN=TONYW.DINONFS.TESTS.H                     
//COMPILE.SYSPRINT DD SYSOUT=*                                        
//PRELINK.I DD DISP=SHR,DSN=SYSD.MUNIT.OBJLIB(MUNIT)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(LOGGER)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSDOL)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSFSZ)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSIO)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSPDIR)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSPRF)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSPRW)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(EBCDIC)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TSTUBS)              
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSDOL)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSFSZ)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSIO)              
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSIO2)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSPDIR)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSPRW)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSPRF)
//          DD DSN=&&OBJ,DISP=(OLD,DELETE)
//GO.TSTFSZ DD DISP=(NEW,DELETE),DSN=&&TESTFSZ,
//          UNIT=SYSDA,VOL=SER=TSO003,SPACE=(TRK,2),
//          DCB=(DSORG=PS,RECFM=VB,LRECL=255,BLKSIZE=27998)                             
//                                                                    