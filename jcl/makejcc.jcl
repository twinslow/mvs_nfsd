//TONYWN1 JOB (NFSD),'MAKE DINO-NFS',
//      CLASS=A,MSGCLASS=X,MSGLEVEL=1,REGION=8M,
//      COND=(0,LT),
//      NOTIFY=TONYW
//*
//********************************************************************
//*
//* NAME: MAKENFSD
//*
//* DESC: COMPILE AND LINK DINO NFSD, USING JCC COMPILER
//*
//* NOTE the lower case -o in the JCC compiler options. If this
//* is upper case -O, then compiler outputs ASM source and not object
//* code.
//********************************************************************
//*-------------------------------------------------------------------
//* Proc to compile a C module using JCC into object code
//*-------------------------------------------------------------------
//JCCCMOD PROC SOUT='*',JCC='JCC',
//          INFILE='TONYW.NFSD.C',
//          OBJFILE='TONYW.NFSD.OBJLIB',
//          HDRFILE='TONYW.NFSD.H',
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
//          MAC2='TONYW.NFSD.ASM',
//          MAC3='SYS2.MACLIB',
//          SRCLIB='TONYW.NFSD.ASM',
//          LOADLIB='TONYW.NFSD.LOAD',
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
//* COMPRESS OBJECT LIB
//*
//********************************************************************
//COMPOBJ  EXEC COMPRESS,LIB='TONYW.NFSD.OBJLIB'
//*
//********************************************************************
//*
//* ASSEMBLE MODULES
//*
//********************************************************************
//*
//MVSDSCB  EXEC ASMMOD,MODULE=MVSDSCB
//GETCIB   EXEC ASMMOD,MODULE=GETCIB
//MVSDALC  EXEC ASMMOD,MODULE=MVSDALC
//MVSSTOW  EXEC ASMMOD,MODULE=MVSSTOW
//MVSENQ   EXEC ASMMOD,MODULE=MVSENQ
//*
//********************************************************************
//*
//* COMPILE C MODULES
//*
//********************************************************************
//EBCDIC   EXEC JCCCMOD,MODNAME=EBCDIC
//LOGGER   EXEC JCCCMOD,MODNAME=LOGGER
//EXPORTS  EXEC JCCCMOD,MODNAME=EXPORTS
//CFGOPTS  EXEC JCCCMOD,MODNAME=CFGOPTS
//FHANDLE  EXEC JCCCMOD,MODNAME=FHANDLE
//MOUNT3   EXEC JCCCMOD,MODNAME=MOUNT3
//NFS3     EXEC JCCCMOD,MODNAME=NFS3
//PORTMAP  EXEC JCCCMOD,MODNAME=PORTMAP
//RPC      EXEC JCCCMOD,MODNAME=RPC
//XDR      EXEC JCCCMOD,MODNAME=XDR
//HEXDUMP  EXEC JCCCMOD,MODNAME=HEXDUMP
//NFSERR   EXEC JCCCMOD,MODNAME=NFSERR
//*
//MVSUTL   EXEC JCCCMOD,MODNAME=MVSUTL
//MVSFID   EXEC JCCCMOD,MODNAME=MVSFID
//MVSBLKC  EXEC JCCCMOD,MODNAME=MVSBLKC
//MVSDOL   EXEC JCCCMOD,MODNAME=MVSDOL
//MVSFSZ   EXEC JCCCMOD,MODNAME=MVSFSZ
//MVSIO    EXEC JCCCMOD,MODNAME=MVSIO
//MVSPDIR  EXEC JCCCMOD,MODNAME=MVSPDIR
//MVSPRF   EXEC JCCCMOD,MODNAME=MVSPRF
//MVSPRW   EXEC JCCCMOD,MODNAME=MVSPRW
//MVSPWFL  EXEC JCCCMOD,MODNAME=MVSPWFL
//MVSPWW   EXEC JCCCMOD,MODNAME=MVSPWW
//MVSSPL   EXEC JCCCMOD,MODNAME=MVSSPL
//*
//* This is a mock VFS module that returns fixed files and contents.
//*
//MOCKVFS  EXEC JCCCMOD,MODNAME=MOCKVFS
//*
//* This is the real VFS module that is reading/writing PDS dirs
//* and content
//*
//MVSVFS   EXEC JCCCMOD,MODNAME=MVSVFS
//*
//********************************************************************
//*
//* COMPILE AND LINK MAIN NFSD
//*
//********************************************************************
//NFSD    EXEC JCCCL,INFILE='TONYW.NFSD.C(NFSD)',
//        PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',
//        OUTFILE='TONYW.NFSD.LOAD(NFSD)',
//        JOPTS='-o -LIST=//DDN:SYSPRINT -D__MVS__'
//*
//COMPILE.JCCINCS DD DISP=SHR,DSN=TONYW.NFSD.H
//*
//PRELINK.I DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(EBCDIC)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(LOGGER)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(CFGOPTS)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(EXPORTS)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(FHANDLE)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MOUNT3)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(NFSERR)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(NFS3)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(PORTMAP)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(RPC)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(XDR)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(HEXDUMP)
//*
//* MOCKVFS was a way to implement a set of VFS functions that
//* would operate on MVS, just returning dummy data in
//* directory content and file content. Its exported symbols
//* match what is also in MVSVFS, so it is commented out.
//*
//*         DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MOCKVFS)
//*
//* The real VFS functions for execution on MVS
/*
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSVFS)
//*
//* Everything that MVSVFS will pull in for the platform.
//*
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSUTL)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSFID)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSBLKC)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSDOL)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSFSZ)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSIO)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSPDIR)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSPRF)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSPRW)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSPWFL)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSPWW)
//          DD DISP=SHR,DSN=TONYW.NFSD.OBJLIB(MVSSPL)
//*
//* And lastly, the compiled object from NFSD compile above.
//*
//          DD DSN=&&OBJ,DISP=(OLD,DELETE)
//*
//LKED.SYSLIN DD DSN=&&OBJMOD,DISP=(OLD,DELETE)
//          DD  DDNAME=SYSIN
//LKED.SYSLMOD DD DISP=SHR,DSN=TONYW.NFSD.LOAD
//LKED.SYSIN DD *
    INCLUDE SYSLMOD(GETCIB)
    INCLUDE SYSLMOD(MVSDALC)
    INCLUDE SYSLMOD(MVSSTOW)
    INCLUDE SYSLMOD(MVSENQ)
    INCLUDE SYSLMOD(MVSDSCB)
    NAME NFSD(R)
//