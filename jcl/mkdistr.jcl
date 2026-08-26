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
//* 1. Delete the program distribution datasets.
//*    SYSS.NFSD.VnRnMn.LOAD
//*    SYSS.NFSD.VnRnMn.DISTRIB
//*
//*    Source materials
//*    SYSS.NFSD.VnRnMn.CNTL
//*    SYSS.NFSD.VnRnMn.ASM
//*    SYSS.NFSD.VnRnMn.C
//*    SYSS.NFSD.VnRnMn.H
//*
//*    Final distribution XMIT file
//*    SYSS.NFSD.VnRnMn.XMI
//*
//* 2. Create the distribution datasets.
//*    SYSS.NFSD.VnRnMn.LOAD
//*    SYSS.NFSD.VnRnMn.DISTRIB
//*    SYSS.NFSD.VnRnMn.XMI
//*    SYSS.NFSD.VnRnMn.CNTL
//*    SYSS.NFSD.VnRnMn.ASM
//*    SYSS.NFSD.VnRnMn.C
//*    SYSS.NFSD.VnRnMn.H
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
//*    dataset and source datasets.
//*
//* 5. Delete the source and load datasets, leaving only the
//*    DISTRIB PDS and the sequential XMIT file for download.
//*
//* You should only need to change the parameters on the MKDISTR
//* procedure.
//*
//********************************************************************
//*
//* CUSTOMIZE BELOW SRCPREF, DSNPREF and VOLSER parameters
//*
//*
//MKDISTR PROC SRCPREF='TONYW.NFSD',
//             DSNPREF='SYSS.NFSD.V0R2M0',
//             VOLSER=TSO003
//*
//*-------------------------------------------------------------------
//* Step 1 - Delete any existing distribution datasets
//*-------------------------------------------------------------------
//*
//S1DEL   EXEC PGM=IEFBR14
//DSNLOAD  DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..LOAD
//DSNDIST  DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..DISTRIB
//DSNASM   DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..ASM
//DSNCNTL  DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..CNTL
//DSNC     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..C
//DSNH     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..H
//DSNXMI   DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..XMI
//*
//*-------------------------------------------------------------------
//* Step 2 - Allocate new distribution datasets
//*-------------------------------------------------------------------
//*
//S2ALOC  EXEC PGM=IEFBR14
//DSNLOAD  DD  DISP=(NEW,CATLG),SPACE=(CYL,(5,3,5)),UNIT=SYSDA,
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=U,LRECL=0,BLKSIZE=19069),
//             DSN=&DSNPREF..LOAD
//DSNDIST  DD  DISP=(NEW,CATLG),SPACE=(CYL,(7,5,5)),UNIT=SYSDA,
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=3120),
//             DSN=&DSNPREF..DISTRIB
//DSNASM   DD  DISP=(NEW,CATLG),SPACE=(CYL,(2,1,3)),UNIT=SYSDA,
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=3120),
//             DSN=&DSNPREF..ASM
//DSNCNTL  DD  DISP=(NEW,CATLG),SPACE=(CYL,(5,3,5)),UNIT=SYSDA,
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=3120),
//             DSN=&DSNPREF..CNTL
//DSNC     DD  DISP=(NEW,CATLG),SPACE=(CYL,(5,3,5)),UNIT=SYSDA,
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=VB,LRECL=255,BLKSIZE=3120),
//             DSN=&DSNPREF..C
//DSNH     DD  DISP=(NEW,CATLG),SPACE=(CYL,(5,3,5)),UNIT=SYSDA,
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=VB,LRECL=255,BLKSIZE=3120),
//             DSN=&DSNPREF..H
//DSNXMI   DD  DISP=(NEW,CATLG),SPACE=(CYL,(7,5)),UNIT=SYSDA,
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PS,RECFM=FB,LRECL=80,BLKSIZE=3120),
//             DSN=&DSNPREF..XMI
//*
//*-------------------------------------------------------------------
//* Step 3 - Copy members to distribution datasets
//*-------------------------------------------------------------------
//*
//S3COPY  EXEC PGM=IEBCOPY
//SYSPRINT DD  SYSOUT=*
//*
//LOADIN    DD  DISP=SHR,DSN=&SRCPREF..LOAD
//CNTLIN    DD  DISP=SHR,DSN=&SRCPREF..CNTL
//ASMIN     DD  DISP=SHR,DSN=&SRCPREF..ASM
//CIN       DD  DISP=SHR,DSN=&SRCPREF..C
//HIN       DD  DISP=SHR,DSN=&SRCPREF..H
//*
//LOADOUT   DD  DISP=OLD,DSN=&DSNPREF..LOAD
//DISTOUT   DD  DISP=OLD,DSN=&DSNPREF..DISTRIB
//ASMOUT    DD  DISP=OLD,DSN=&DSNPREF..ASM
//CNTLOUT   DD  DISP=OLD,DSN=&DSNPREF..CNTL
//COUT      DD  DISP=OLD,DSN=&DSNPREF..C
//HOUT      DD  DISP=OLD,DSN=&DSNPREF..H
//*
//*-------------------------------------------------------------------
//* Step 4.1 - Create XMILOAD member in distribution
//*-------------------------------------------------------------------
//*
//S41LOAD EXEC PGM=XMIT370
//SYSPRINT DD  SYSOUT=*
//XMITPRT  DD  SYSOUT=*
//XMITLOG  DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
//SYSUT1   DD  DSN=&DSNPREF..LOAD,DISP=SHR
//SYSUT2   DD  DISP=(,DELETE,DELETE),DSN=&&SYSUT2,
//             UNIT=SYSDA,
//             SPACE=(CYL,50)
//XMITOUT  DD  DISP=OLD,DSN=&DSNPREF..DISTRIB(XMILOAD)
//SYSIN    DD  DUMMY
//*
//*-------------------------------------------------------------------
//* Step 4.2 - Create XMICNTL member in distribution
//*-------------------------------------------------------------------
//*
//S42CNTL EXEC PGM=XMIT370
//SYSPRINT DD  SYSOUT=*
//XMITPRT  DD  SYSOUT=*
//XMITLOG  DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
//SYSUT1   DD  DSN=&DSNPREF..CNTL,DISP=SHR
//SYSUT2   DD  DISP=(,DELETE,DELETE),DSN=&&SYSUT2,
//             UNIT=SYSDA,
//             SPACE=(CYL,50)
//XMITOUT  DD  DISP=OLD,DSN=&DSNPREF..DISTRIB(XMICNTL)
//SYSIN    DD  DUMMY
//*
//*-------------------------------------------------------------------
//* Step 4.3 - Create XMIASM member in distribution
//*-------------------------------------------------------------------
//*
//S43ASM  EXEC PGM=XMIT370
//SYSPRINT DD  SYSOUT=*
//XMITPRT  DD  SYSOUT=*
//XMITLOG  DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
//SYSUT1   DD  DSN=&DSNPREF..ASM,DISP=SHR
//SYSUT2   DD  DISP=(,DELETE,DELETE),DSN=&&SYSUT2,
//             UNIT=SYSDA,
//             SPACE=(CYL,50)
//XMITOUT  DD  DISP=OLD,DSN=&DSNPREF..DISTRIB(XMIASM)
//SYSIN    DD  DUMMY
//*
//*-------------------------------------------------------------------
//* Step 4.4 - Create XMIC member in distribution
//*-------------------------------------------------------------------
//*
//S44C    EXEC PGM=XMIT370
//SYSPRINT DD  SYSOUT=*
//XMITPRT  DD  SYSOUT=*
//XMITLOG  DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
//SYSUT1   DD  DSN=&DSNPREF..C,DISP=SHR
//SYSUT2   DD  DISP=(,DELETE,DELETE),DSN=&&SYSUT2,
//             UNIT=SYSDA,
//             SPACE=(CYL,50)
//XMITOUT  DD  DISP=OLD,DSN=&DSNPREF..DISTRIB(XMIC)
//SYSIN    DD  DUMMY
//*
//*-------------------------------------------------------------------
//* Step 4.5 - Create XMIH member in distribution
//*-------------------------------------------------------------------
//*
//S45H    EXEC PGM=XMIT370
//SYSPRINT DD  SYSOUT=*
//XMITPRT  DD  SYSOUT=*
//XMITLOG  DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
//SYSUT1   DD  DSN=&DSNPREF..H,DISP=SHR
//SYSUT2   DD  DISP=(,DELETE,DELETE),DSN=&&SYSUT2,
//             UNIT=SYSDA,
//             SPACE=(CYL,50)
//XMITOUT  DD  DISP=OLD,DSN=&DSNPREF..DISTRIB(XMIH)
//SYSIN    DD  DUMMY
//*
//*-------------------------------------------------------------------
//* Step 5 - Create the final sequential XMI dataset
//*-------------------------------------------------------------------
//*
//S5DXMI  EXEC PGM=XMIT370
//SYSPRINT DD  SYSOUT=*
//XMITPRT  DD  SYSOUT=*
//XMITLOG  DD  SYSOUT=*
//SYSUDUMP DD  SYSOUT=*
//SYSUT1   DD  DSN=&DSNPREF..DISTRIB,DISP=SHR
//SYSUT2   DD  DISP=(,DELETE,DELETE),DSN=&&SYSUT2,
//             UNIT=SYSDA,
//             SPACE=(CYL,50)
//XMITOUT  DD  DISP=OLD,DSN=&DSNPREF..XMI
//SYSIN    DD  DUMMY
//*
//*-------------------------------------------------------------------
//* Step 6 - Clean up the temporary distribution datasets
//*-------------------------------------------------------------------
//*
//S6DEL   EXEC PGM=IEFBR14
//DSNLOAD  DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..LOAD
//DSNASM   DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..ASM
//DSNCNTL  DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..CNTL
//DSNC     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..C
//DSNH     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=&DSNPREF..H
//*
//         PEND
//*
//MKDISTR EXEC MKDISTR
//S3COPY.SYSIN  DD  *
    COPY OUTDD=LOADOUT,INDD=LOADIN
    SELECT MEMBER=(RESSOCK,NFSD)
    COPY OUTDD=DISTOUT,INDD=CNTLIN
    SELECT MEMBER=($README,CONFIG,INSTALL,NFSD)
    COPY OUTDD=ASMOUT,INDD=ASMIN
    COPY OUTDD=CNTLOUT,INDD=CNTLIN
    COPY OUTDD=COUT,INDD=CIN
    COPY OUTDD=HOUT,INDD=HIN
//*
//