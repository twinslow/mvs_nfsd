//TONYWDIS JOB (NFSD),'Make distribution',
//             CLASS=A,MSGCLASS=X,MSGLEVEL=(1,1),
//             NOTIFY=&SYSUID
//*
//********************************************************************
//*
//* Name: MKDISTR
//*
//* Description:
//*
//* 1. Delete the distribution datasets.
//*    SYSS.NFSD.LOAD
//*    SYSS.NFSD.DISTRIB
//*    SYSS.NFSD.XMI
//*
//* 2. Create the distribution datasets.
//*    SYSS.NFSD.LOAD
//*    SYSS.NFSD.DISTRIB
//*    SYSS.NFSD.XMI
//*
//* 3. Copy the load modules from the orig load lib to the
//*    distribution load dataset
//*    NFSD        - The main NFSD server program
//*    RESSOCK     - A utility program to reset the sockets
//*                  on the specified ports.
//*
//*    Copy the started task JCL and sample config to the
//*    distribution CNTL dataset and other members...
//*    $README     - Install instructions
//*    INSTALL     - Sample install JCL to recv load DS
//*    NFSD        - Stored procedure JCL
//*    CONFIG      - Sample configuration dataset
//*
//* 4. Use XMIT370 to create XMI files for LOAD library
//*    dataset
//*
//********************************************************************
//*
//XMIT370 PROC XMI='NULLFILE',      XMI - OUTput xmit data set
//             PDS='NULLFILE'       PDS - INput pds data set
//XMIT370  EXEC PGM=XMIT370
//SYSPRINT DD  SYSOUT=*
//XMITPRT  DD  SYSOUT=*
//XMITLOG  DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
//SYSUT1   DD  DSN=&PDS,DISP=SHR
//SYSUT2   DD  DISP=(,DELETE,DELETE),DSN=&&SYSUT2,
//             UNIT=SYSDA,
//             SPACE=(CYL,50)
//XMITOUT  DD  DISP=OLD,DSN=&XMI
//SYSIN    DD  DUMMY
//         PEND
//*
//********************************************************************
//*
//S1      EXEC PGM=IEFBR14
//DEL1     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.LOAD
//DEL2     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.DISTRIB
//DEL3     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.XMI
//*
//S2      EXEC PGM=IEFBR14
//ALLOC1   DD  DISP=(NEW,CATLG),SPACE=(CYL,(5,3,5)),UNIT=SYSDA,
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=U,LRECL=0,BLKSIZE=19069),
//             DSN=SYSS.NFSD.LOAD
//ALLOC2   DD  DISP=(NEW,CATLG),SPACE=(CYL,(7,5,5)),UNIT=SYSDA,
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=3120),
//             DSN=SYSS.NFSD.DISTRIB
//ALLOC3   DD  DISP=(NEW,CATLG),SPACE=(CYL,(7,5)),UNIT=SYSDA,
//             VOL=SER=TSO003,
//             DCB=(DSORG=PS,RECFM=FB,LRECL=80,BLKSIZE=3120),
//             DSN=SYSS.NFSD.XMI
//*
//S3      EXEC PGM=IEBCOPY
//SYSPRINT DD  SYSOUT=*
//SYSIN    DD  *
    COPY OUTDD=LOADOUT,INDD=LOADIN
    SELECT MEMBER=(RESSOCK,NFSD)
    COPY OUTDD=DISTOUT,INDD=CNTLIN
    SELECT MEMBER=($README,CONFIG,INSTALL,NFSD)
//*
//LOADIN    DD  DISP=SHR,DSN=TONYW.NFSD.LOAD
//CNTLIN    DD  DISP=SHR,DSN=TONYW.NFSD.CNTL
//*
//LOADOUT   DD  DISP=OLD,DSN=SYSS.NFSD.LOAD
//DISTOUT   DD  DISP=OLD,DSN=SYSS.NFSD.DISTRIB
//*
//XMILOAD  EXEC XMIT370,PDS='SYSS.NFSD.LOAD',
//             XMI='SYSS.NFSD.DISTRIB(XMILOAD)'
//*
//XMIDIST  EXEC XMIT370,PDS='SYSS.NFSD.DISTRIB',
//             XMI='SYSS.NFSD.XMI'
//*
//