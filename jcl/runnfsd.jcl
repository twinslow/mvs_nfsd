//TONYWN1 JOB (NFSD),'MAKE MVS NFSD',
//      CLASS=B,MSGCLASS=X,MSGLEVEL=1,REGION=8M,
//      NOTIFY=TONYW
//*
//********************************************************************
//*
//* NAME: RUNNFSD
//*
//* DESC: Run the NFSD process as a batch job.
//*
//********************************************************************
//RESSOCK EXEC PGM=RESSOCK,
//          PARM='111 2048 2049'
//STEPLIB   DD DISP=SHR,DSN=TONYW.NFSD.LOAD
//SYSPRINT  DD SYSOUT=*
//STDOUT    DD SYSOUT=*,DCB=(RECFM=V,BLKSIZE=250)
//STDERR    DD SYSOUT=*,DCB=(RECFM=V,BLKSIZE=250)
//STDIN     DD DUMMY
//*
//NFSD    EXEC PGM=NFSD,
//          PARM='-p 111 -m 2048 -n 2049 NFSDCONF'
//STEPLIB   DD DISP=SHR,DSN=TONYW.NFSD.LOAD
//SYSPRINT  DD SYSOUT=*
//STDOUT    DD SYSOUT=*,DCB=(RECFM=V,BLKSIZE=250)
//STDERR    DD SYSOUT=*,DCB=(RECFM=V,BLKSIZE=250)
//STDIN     DD DUMMY
//NFSDCONF  DD DISP=SHR,DSN=TONYW.NFSD.CONF
//