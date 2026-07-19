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
//*
//*-------------------------------------------------------------------
//* Proc to assemble a module using ASMF into the load library
//*-------------------------------------------------------------------
//ASMMOD   PROC SOUT='*',
//          ASMOPTS='OBJ,NODECK,NOLIST',
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
//********************************************************************
//*
//* ASSEMBLE MODULES
//*
//********************************************************************
//*
//GETCIB   EXEC ASMMOD,MODULE=GETCIB
//MVSDALC  EXEC ASMMOD,MODULE=MVSDALC
//MVSSTOW  EXEC ASMMOD,MODULE=MVSSTOW
//MVSENQ   EXEC ASMMOD,MODULE=MVSENQ 
//*
//********************************************************************
//*
//* Compile application modules
//*
//********************************************************************
//EBCDIC   EXEC JCCCMOD,MODNAME=EBCDIC
//CFGOPTS  EXEC JCCCMOD,MODNAME=CFGOPTS
//LOGGER   EXEC JCCCMOD,MODNAME=LOGGER
//MVSDOL   EXEC JCCCMOD,MODNAME=MVSDOL
//MVSFSZ   EXEC JCCCMOD,MODNAME=MVSFSZ                                
//MVSIO    EXEC JCCCMOD,MODNAME=MVSIO                                 
//MVSPDIR  EXEC JCCCMOD,MODNAME=MVSPDIR
//MVSPRF   EXEC JCCCMOD,MODNAME=MVSPRF
//MVSPRW   EXEC JCCCMOD,MODNAME=MVSPRW
//MVSPWW   EXEC JCCCMOD,MODNAME=MVSPWW
//*                                                                   
//********************************************************************
//*                                                                   
//* Compile unit test modules                                         
//*                                                                   
//********************************************************************
//TSTUBS   EXEC JCCCTST,MODNAME=TSTUBS
//TLOGGER  EXEC JCCCTST,MODNAME=TLOGGER
//TCFGOPTS EXEC JCCCTST,MODNAME=TCFGOPTS
//TMVSDOL  EXEC JCCCTST,MODNAME=TMVSDOL
//TMVSFSZ  EXEC JCCCTST,MODNAME=TMVSFSZ                               
//TMVSIO   EXEC JCCCTST,MODNAME=TMVSIO                                
//TMVSIO2  EXEC JCCCTST,MODNAME=TMVSIO2                               
//TMVSPDIR EXEC JCCCTST,MODNAME=TMVSPDIR                               
//TMVSPRW  EXEC JCCCTST,MODNAME=TMVSPRW
//TMVSPRF  EXEC JCCCTST,MODNAME=TMVSPRF
//TMVSPWW  EXEC JCCCTST,MODNAME=TMVSPWW
//*                                                                   
//********************************************************************
//*                                                                   
//* Compile RUNALL, then link and run tests                           
//*                                                                   
//********************************************************************
//JCCCLG  EXEC JCCCLG,INFILE='TONYW.DINONFS.TESTS.C(RUNALL)',   
//             PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',       
//             JOPTS='-D_MVS -D__MVS__ -o -list=//DDN:SYSPRINT'       
//COMPILE.JCCINCS DD DISP=SHR,DSN=SYSD.MUNIT.H                        
//          DD DISP=SHR,DSN=TONYW.DINONFS.H                           
//          DD DISP=SHR,DSN=TONYW.DINONFS.TESTS.H                     
//COMPILE.SYSPRINT DD SYSOUT=*                                        
//PRELINK.I DD DISP=SHR,DSN=SYSD.MUNIT.OBJLIB(MUNIT)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(LOGGER)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(CFGOPTS)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSDOL)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSFSZ)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSIO)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSPDIR)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSPRF)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSPRW)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSPWW)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(EBCDIC)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TSTUBS)              
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSDOL)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSFSZ)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSIO)              
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSIO2)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSPDIR)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSPRW)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSPRF)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TMVSPWW)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TLOGGER)
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(TCFGOPTS)
//          DD DSN=&&OBJ,DISP=(OLD,DELETE)
//LKED.SYSLIN DD
//          DD DDNAME=SYSIN
//LKED.SYSIN DD *
    INCLUDE SYSLIB(MVSENQ)
//LKED.SYSLIB DD DISP=SHR,DSN=TONYW.DINONFS.LOAD
//GO.TSTFSZ DD DISP=(NEW,DELETE),DSN=&&TESTFSZ,
//          UNIT=SYSDA,VOL=SER=TSO003,SPACE=(TRK,2),
//          DCB=(DSORG=PS,RECFM=VB,LRECL=255,BLKSIZE=27998)
//                                                                    