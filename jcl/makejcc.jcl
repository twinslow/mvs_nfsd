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
//********************************************************************
//*
//* ASSEMBLE MODULES
//*
//********************************************************************
//GETCIB EXEC ASMFCL,MAC1='SYS1.AMODGEN',MAC2='SYS2.MACLIB', 
//        PARM.ASM=(OBJ,NODECK)                              
//ASM.SYSPUNCH DD SYSOUT=*                                   
//ASM.SYSIN    DD DISP=SHR,DSN=TONYW.DINONFS.ASM(GETCIB)     
//LKED.SYSLMOD DD DISP=SHR,DSN=TONYW.DINONFS.LOAD(GETCIB)
//*
//********************************************************************
//*
//* COMPILE C MODULES
//*
//********************************************************************
//EBCDIC   EXEC JCCCMOD,MODNAME=EBCDIC                                
//LOGGER   EXEC JCCCMOD,MODNAME=LOGGER                               
//EXPORTS  EXEC JCCCMOD,MODNAME=EXPORTS                               
//FHANDLE  EXEC JCCCMOD,MODNAME=FHANDLE                               
//MOUNT3   EXEC JCCCMOD,MODNAME=MOUNT3                                
//MVSFID   EXEC JCCCMOD,MODNAME=MVSFID                                
//MVSDOL   EXEC JCCCMOD,MODNAME=MVSDOL                                 
//MVSIO    EXEC JCCCMOD,MODNAME=MVSIO                                 
//MVSPDIR  EXEC JCCCMOD,MODNAME=MVSPDIR                               
//MVSPRW   EXEC JCCCMOD,MODNAME=MVSPRW                                
//NFS3     EXEC JCCCMOD,MODNAME=NFS3                                  
//PORTMAP  EXEC JCCCMOD,MODNAME=PORTMAP                               
//RPC      EXEC JCCCMOD,MODNAME=RPC                                   
//XDR      EXEC JCCCMOD,MODNAME=XDR                                   
//HEXDUMP  EXEC JCCCMOD,MODNAME=HEXDUMP                               
//*                                                                   
//* This is a mock VFS module that returns fixed files and contents.  
//MOCKVFS  EXEC JCCCMOD,MODNAME=MOCKVFS                               
//*                                                                   
//* This is the real VFS module that is reading PDS dir and content   
//MVSVFS   EXEC JCCCMOD,MODNAME=MVSVFS                                
//*                                                                   
//********************************************************************
//*                                                                   
//* COMPILE AND LINK MAIN NFSD                                        
//*                                                                   
//********************************************************************
//NFSD    EXEC JCCCL,INFILE='TONYW.DINONFS.C(NFSD)',                  
//        PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',                  
//        OUTFILE='TONYW.DINONFS.LOAD(NFSD)',                         
//        JOPTS='-o -LIST=//DDN:SYSPRINT -D__MVS__'                   
//COMPILE.JCCINCS DD DISP=SHR,DSN=TONYW.DINONFS.H                     
//* Below prelink was to merge in ASM modules as used in              
//* FTPD build.                                                       
//* PRELINK.L DD DSN=&&ALLOBJ,DISP=(OLD,DELETE)                       
//PRELINK.I DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(EBCDIC)              
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(LOGGER)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(EXPORTS)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(FHANDLE)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MOUNT3)              
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSFID)              
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSDOL) 
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSIO)               
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSPDIR)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSPRW)              
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(NFS3)                
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(PORTMAP)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(RPC)                 
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(XDR)                 
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(HEXDUMP)             
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MVSVFS)              
//*         DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(MOCKVFS)             
//          DD DSN=&&OBJ,DISP=(OLD,DELETE)                            
//LKED.SYSLIN DD DSN=&&OBJMOD,DISP=(OLD,DELETE)                       
//          DD  DDNAME=SYSIN                                         
//LKED.SYSLMOD DD DISP=SHR,DSN=TONYW.DINONFS.LOAD                    
//LKED.SYSIN DD *                                                    
    INCLUDE SYSLMOD(GETCIB)                                          
    NAME NFSD(R)                                                     
//                                                                    