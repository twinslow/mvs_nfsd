//NFSDINST JOB (NFSD),'NFSD RECV .LOAD',
//             CLASS=A,MSGCLASS=X,MSGLEVEL=(1,1),
//             NOTIFY=&SYSUID
//*
//********************************************************************
//*
//RECV370 PROC XMI=FORGITTEN, XMI - INput xmit data set
//             PDS=FORGOTTEN, PDS - OUTput pds data set
//             SPA='600,150', Primary and secondary SPACE (in trk)
//             BLK='3120',    Blocksize
//             DIR=40,        Number of directory blocks for PDS
//             VOL=WORK03     Target dasd volume
//RECV370  EXEC PGM=RECV370
//RECVLOG  DD  SYSOUT=*
//SYSTSPRT DD  SYSOUT=*
//SYSPRINT DD  SYSOUT=*,DCB=(RECFM=FB,LRECL=121,BLKSIZE=12100)
//SYSTERM  DD  SYSOUT=*
//SYSABEND DD  DUMMY
//*
//XMITIN   DD  DISP=SHR,DSN=&XMI
//*
//* SYSUT1   = PDS unloaded (Sequential unloaded) temporary
//SYSUT1   DD  DISP=(,DELETE,DELETE),DSN=&&SYSUT1,
//             UNIT=SYSALLDA,VOL=SER=&VOL,
//             SPACE=(TRK,(&SPA))
//*
//* SYSUT2   = PDS output data set
//SYSUT2   DD  DISP=(,CATLG),DSN=&PDS,
//             UNIT=SYSALLDA,VOL=SER=&VOL,
//             DCB=(LRECL=0,BLKSIZE=&BLK,DSORG=PO,RECFM=U),
//             SPACE=(TRK,(&SPA,&DIR))
//SYSIN    DD  DUMMY
//         PEND
//*
//********************************************************************
//* call the procedure
//********************************************************************
//*
//RECEIVE  EXEC RECV370,
//             XMI='SYSS.NFSD.DISTRIB(XMILOAD)',
//             PDS='SYSS.NFSD.LOAD',
//             SPA='040,0', Primary and secondary SPACE (in trk)
//             BLK='19069',   Blocksize
//             DIR=1,         Number of directory blocks for PDS
//             VOL=TSO003     Target dasd volume
//*
//