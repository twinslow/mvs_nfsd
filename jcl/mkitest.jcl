//ITESTDS  JOB (ITEST),'MAKE ITEST DSETS',
//             CLASS=A,MSGCLASS=X,MSGLEVEL=(1,1),
//             NOTIFY=&SYSUID
//*
//********************************************************************
//*
//* Name: MKITEST
//*
//* Description:
//*
//* Allocate the PDS datasets used by the dino-nfs automated
//* integration tests (integration-test/).  Run this ONCE on MVS
//* before running the tests, and again whenever you want a clean
//* slate.
//*
//* Datasets (adjust the dataset name prefix) for DS names to match
//* your config.json "dsname" values and export configuration:
//*
//*    TEMP.ITEST.FB       PO RECFM=FB LRECL=80   general FB tests
//*    TEMP.ITEST.VB       PO RECFM=VB LRECL=255  general VB tests
//*    TEMP.ITEST.FB2      PO RECFM=FB LRECL=80   rename/copy target
//*    TEMP.ITEST.VB2      PO RECFM=VB LRECL=255  rename/copy target
//*    TEMP.ITEST.FBSMALL  PO RECFM=FB LRECL=80   tiny -> "full ds"
//*
//* These datasets must also be EXPORTED by the dino-nfs server
//* (nfsd.conf) so their members appear under the NFS mount.  Give
//* them a matching file extension (e.g. fileext=txt) so members
//* show up as <name>.txt to the NFS client.
//*
//********************************************************************
//*
//MKDSETS PROC PREFIX='TEMP.ITEST',VOLSER=WORK04,IN=CYL,PRI=10,SEC=10
//*
//DELETE  EXEC PGM=IEFBR14
//*
//DEL1     DD  UNIT=SYSDA,SPACE=(TRK,1),DISP=(MOD,DELETE),
//             DSN=&PREFIX..FB
//DEL2     DD  UNIT=SYSDA,SPACE=(TRK,1),DISP=(MOD,DELETE),
//             DSN=&PREFIX..VB
//DEL3     DD  UNIT=SYSDA,SPACE=(TRK,1),DISP=(MOD,DELETE),
//             DSN=&PREFIX..FB2
//DEL4     DD  UNIT=SYSDA,SPACE=(TRK,1),DISP=(MOD,DELETE),
//             DSN=&PREFIX..VB2
//DEL5     DD  UNIT=SYSDA,SPACE=(TRK,1),DISP=(MOD,DELETE),
//             DSN=&PREFIX..FBSMALL
//*
//ALLOC   EXEC PGM=IEFBR14
//*
//FB       DD  DSN=&PREFIX..FB,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(&IN,(&PRI,&SEC,25)),
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=8000)
//VB       DD  DSN=&PREFIX..VB,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(&IN,(&PRI,&SEC,25)),
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=VB,LRECL=255,BLKSIZE=6144)
//FB2      DD  DSN=&PREFIX..FB2,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(&IN,(&PRI,&SEC,25)),
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=8000)
//VB2      DD  DSN=&PREFIX..VB2,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(&IN,(&PRI,&SEC,25)),
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=VB,LRECL=255,BLKSIZE=6144)
//*
//* FBSMALL is intentionally tiny (1 track, 3 directory blocks) so
//* the "upload to full dataset" test fills it quickly.
//*
//FBSMALL  DD  DSN=&PREFIX..FBSMALL,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(TRK,(1,1,3)),
//             VOL=SER=&VOLSER,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=8000)
//*
//MKDSETS PEND
//*
//********************************************************************
//*
//S1 EXEC MKDSETS
//