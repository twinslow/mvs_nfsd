//ITESTDS  JOB (ITEST),'MAKE ITEST DSETS',
//             CLASS=A,MSGCLASS=X,MSGLEVEL=(1,1),
//             NOTIFY=&SYSUID
//*
//********************************************************************
//*
//* Name: MKDSETS
//*
//* Description:
//*
//* Allocate the PDS datasets used by the MVS NFSD automated
//* integration tests (integration-test/).  Run this ONCE on MVS
//* before running the tests, and again whenever you want a clean
//* slate.
//*
//* Datasets (adjust the TEMP.ITEST.* names to match your
//* config.json "dsname" values and export configuration):
//*
//*    TEMP.ITEST.FB       PO RECFM=FB LRECL=80   general FB tests
//*    TEMP.ITEST.VB       PO RECFM=VB LRECL=255  general VB tests
//*    TEMP.ITEST.FB2      PO RECFM=FB LRECL=80   rename/copy target
//*    TEMP.ITEST.VB2      PO RECFM=VB LRECL=255  rename/copy target
//*    TEMP.ITEST.FBSMALL  PO RECFM=FB LRECL=80   tiny -> "full ds"
//*
//* STEP1 deletes any existing copies (ok to fail the first time);
//* STEP2 allocates them fresh and empty.
//*
//* These datasets must also be EXPORTED by the MVS NFSD server
//* (nfsd.conf) so their members appear under the NFS mount.  Give
//* them a matching file extension (e.g. fileext=txt) so members
//* show up as <name>.txt to the NFS client.
//*
//********************************************************************
//*
//DELETE   EXEC PGM=IDCAMS
//SYSPRINT DD   SYSOUT=*
//SYSIN    DD   *
  DELETE TEMP.ITEST.FB
  DELETE TEMP.ITEST.VB
  DELETE TEMP.ITEST.FB2
  DELETE TEMP.ITEST.VB2
  DELETE TEMP.ITEST.FBSMALL
  SET MAXCC = 0
/*
//*
//ALLOC   EXEC PGM=IEFBR14
//FB       DD  DSN=TEMP.ITEST.FB,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(TRK,(30,15,25)),
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=8000)
//VB       DD  DSN=TEMP.ITEST.VB,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(TRK,(30,15,25)),
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=VB,LRECL=255,BLKSIZE=6144)
//FB2      DD  DSN=TEMP.ITEST.FB2,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(TRK,(15,10,15)),
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=8000)
//VB2      DD  DSN=TEMP.ITEST.VB2,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(TRK,(15,10,15)),
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=VB,LRECL=255,BLKSIZE=6144)
//*
//* FBSMALL is intentionally tiny (1 track, 3 directory blocks) so
//* the "upload to full dataset" test fills it quickly.
//*
//FBSMALL  DD  DSN=TEMP.ITEST.FBSMALL,DISP=(NEW,CATLG,DELETE),
//             UNIT=SYSDA,SPACE=(TRK,(1,0,3)),
//             VOL=SER=TSO003,
//             DCB=(DSORG=PO,RECFM=FB,LRECL=80,BLKSIZE=8000)
//
