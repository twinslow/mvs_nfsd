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
//             DSN=SYSS.NFSD.ASM
//DEL4     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.CNTL
//DEL5     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.C
//DEL6     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.H
//DEL7     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
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
//ALLOC3   DD  DISP=(NEW,CATLG),SPACE=(CYL,(2,1,3)),UNIT=SYSDA,
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=3120),
//             DSN=SYSS.NFSD.ASM
//ALLOC4   DD  DISP=(NEW,CATLG),SPACE=(CYL,(5,3,5)),UNIT=SYSDA,
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=3120),
//             DSN=SYSS.NFSD.CNTL
//ALLOC5   DD  DISP=(NEW,CATLG),SPACE=(CYL,(5,3,5)),UNIT=SYSDA,
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=VB,LRECL=255,BLKSIZE=3120),
//             DSN=SYSS.NFSD.C
//ALLOC6   DD  DISP=(NEW,CATLG),SPACE=(CYL,(5,3,5)),UNIT=SYSDA,
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=VB,LRECL=255,BLKSIZE=3120),
//             DSN=SYSS.NFSD.H
//ALLOC7   DD  DISP=(NEW,CATLG),SPACE=(CYL,(7,5)),UNIT=SYSDA,
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
    COPY OUTDD=ASMOUT,INDD=ASMIN
    COPY OUTDD=CNTLOUT,INDD=CNTLIN
    COPY OUTDD=COUT,INDD=CIN
    COPY OUTDD=HOUT,INDD=HIN
//*
//LOADIN    DD  DISP=SHR,DSN=TONYW.NFSD.LOAD
//CNTLIN    DD  DISP=SHR,DSN=TONYW.NFSD.CNTL
//ASMIN     DD  DISP=SHR,DSN=TONYW.NFSD.ASM
//CIN       DD  DISP=SHR,DSN=TONYW.NFSD.C
//HIN       DD  DISP=SHR,DSN=TONYW.NFSD.H
//*
//LOADOUT   DD  DISP=OLD,DSN=SYSS.NFSD.LOAD
//DISTOUT   DD  DISP=OLD,DSN=SYSS.NFSD.DISTRIB
//ASMOUT    DD  DISP=OLD,DSN=SYSS.NFSD.ASM
//CNTLOUT   DD  DISP=OLD,DSN=SYSS.NFSD.CNTL
//COUT      DD  DISP=OLD,DSN=SYSS.NFSD.C
//HOUT      DD  DISP=OLD,DSN=SYSS.NFSD.H
//*
//XMILOAD  EXEC XMIT370,PDS='SYSS.NFSD.LOAD',
//             XMI='SYSS.NFSD.DISTRIB(XMILOAD)'
//XMICNTL  EXEC XMIT370,PDS='SYSS.NFSD.CNTL',
//             XMI='SYSS.NFSD.DISTRIB(XMICNTL)'
//XMIASM   EXEC XMIT370,PDS='SYSS.NFSD.ASM',
//             XMI='SYSS.NFSD.DISTRIB(XMIASM)'
//XMIC     EXEC XMIT370,PDS='SYSS.NFSD.C',
//             XMI='SYSS.NFSD.DISTRIB(XMIC)'
//XMIH     EXEC XMIT370,PDS='SYSS.NFSD.H',
//             XMI='SYSS.NFSD.DISTRIB(XMIH)'
//*
//XMIDIST  EXEC XMIT370,PDS='SYSS.NFSD.DISTRIB',
//             XMI='SYSS.NFSD.XMI'
//*
//CLEANUP  EXEC PGM=IEFBR14
//DEL1     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.LOAD
//DEL3     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.ASM
//DEL4     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.CNTL
//DEL5     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.C
//DEL6     DD  DISP=(MOD,DELETE),SPACE=(TRK,1),UNIT=SYSDA,
//             DSN=SYSS.NFSD.H
//*
//