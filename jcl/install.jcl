//NFSDINST JOB (NFSD),'NFSD RECV .LOAD',
//             CLASS=A,MSGCLASS=X,MSGLEVEL=(1,1),
//             NOTIFY=&SYSUID
//*
//********************************************************************
//* Restore the distribution datasets from XMIT data
//*
//* SYSS.NFSD.VnRnMn.LOAD      - Executables required
//*
//* SYSS.NFSD.VnRnMn.ASM       - Assembler Source modules
//* SYSS.NFSD.VnRnMn.C         - C Source modules
//* SYSS.NFSD.VnRnMn.H         - C header source files
//* SYSS.NFSD.VnRnMn.CNTL      - Misc jobs for build and maintenance.
//*                              There are also various test programs.
//*
//* The datasets containing the source are for reference, it is not
//* intented to be a build environment. However, the member
//* 'SYSS.NFSD.CNTL(MAKEJCC)' is a job that will assemble,
//* compile and link everything for the NFSD executable.
//* You will need to modify it for your own environment.
//*
//********************************************************************
//*
//RECV370 PROC VOL=TSO003,                  Target dasd volume
//         XMI='SYSS.NFSD.V0R1M0.DISTRIB',  XMI - Input xmit data set
//         INMEM='NOT-SPECIFIED',           XMI - Input member
//         OUTPREF='SYSS.NFSD.V0R1M0',      Output dataset prefix
//         OUTTYPE='FORGOTTEN',             Output dataset last qual
//         SPA='15,5',                      Pri and sec SPACE (in trk)
//         BLK='3200',                      Blocksize
//         RECFM=FB,                        Record format
//         LRECL=80,                        Logical record length
//         DIR=5                            Number of dir blks for PDS
//*
//RECV370  EXEC PGM=RECV370
//RECVLOG  DD  SYSOUT=*
//SYSTSPRT DD  SYSOUT=*
//SYSPRINT DD  SYSOUT=*,DCB=(RECFM=FB,LRECL=121,BLKSIZE=12100)
//SYSTERM  DD  SYSOUT=*
//SYSABEND DD  DUMMY
//*
//XMITIN   DD  DISP=SHR,DSN=&XMI(&INMEM)
//*
//* SYSUT1   = PDS unloaded (Sequential unloaded) temporary
//SYSUT1   DD  DISP=(,DELETE,DELETE),DSN=&&SYSUT1,
//             UNIT=SYSALLDA,VOL=SER=&VOL,
//             SPACE=(TRK,(&SPA))
//*
//* SYSUT2   = PDS output data set
//SYSUT2   DD  DISP=(,CATLG),DSN=&OUTPREF..&OUTTYPE,
//             UNIT=SYSALLDA,VOL=SER=&VOL,
//             DCB=(LRECL=&LRECL,BLKSIZE=&BLK,DSORG=PO,RECFM=&RECFM),
//             SPACE=(TRK,(&SPA,&DIR))
//SYSIN    DD  DUMMY
//         PEND
//*
//********************************************************************
//* call the procedure
//********************************************************************
//*
//RECLOAD EXEC RECV370,OUTTYPE='LOAD',INMEM='XMILOAD',
//             RECFM=U,BLK=19069,LRECL=0,
//             DIR=1,SPA='45,0'
//*
//RECASM  EXEC RECV370,OUTTYPE=ASM,INMEM=XMIASM
//RECCNTL EXEC RECV370,OUTTYPE=CNTL,INMEM=XMICNTL
//RECC    EXEC RECV370,OUTTYPE=C,INMEM=XMIC,RECFM=VB,LRECL=255
//RECH    EXEC RECV370,OUTTYPE=H,INMEM=XMIH,RECFM=VB,LRECL=255
//*
//