//NFSD    PROC DSNPREF='SYSS.NFSD.V0R1M0',                               0000100
//             CONFIG='SYS1.PARMLIB(NFSDCFG0)'                           0000110
//********************************************************************   0000200
//*                                                                      0000300
//* NAME: RUNNFSD                                                        0000400
//*                                                                      0000500
//* DESC: RUN THE NFSD PROCESS                                           0000601
//* COPY THIS JCL TO A SUITABLE PROCLIB PDS TO RUN AS STC.               0000700
//********************************************************************   0000800
//RESSOCK EXEC PGM=RESSOCK,REGION=1M,                                    0000903
//          PARM='111 20048 2049'                                        0001000
//STEPLIB   DD DISP=SHR,DSN=&DSNPREF..LOAD                               0001100
//STDOUT    DD SYSOUT=X,DCB=(RECFM=V,BLKSIZE=250)                        0001202
//STDERR    DD SYSOUT=X,DCB=(RECFM=V,BLKSIZE=250)                        0001302
//STDIN     DD DUMMY                                                     0001400
//*                                                                      0001500
//NFSD    EXEC PGM=NFSD,REGION=4M,                                       0001603
//          PARM='-p 111 -m 20048 -n 2049 NFSDCONF'                      0001704
//STEPLIB   DD DISP=SHR,DSN=&DSNPREF..LOAD                               0001100
//STDOUT    DD SYSOUT=X,DCB=(RECFM=V,BLKSIZE=250)                        0001902
//STDERR    DD SYSOUT=X,DCB=(RECFM=V,BLKSIZE=250)                        0002002
//STDIN     DD DUMMY                                                     0002100
//NFSDCONF  DD DISP=SHR,DSN=&CONFIG                                      0002200
//*SYSUDUMP  DD SYSOUT=X                                                 0002407