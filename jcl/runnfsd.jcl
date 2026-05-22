//TONYWN1 JOB (NFSD),'MAKE DINO-NFS',                                 
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
//NFSD    EXEC PGM=NFSD,                                              
//          PARM='-p 11111 -m 12048 -n 12049 NFSDCONF'                
//STEPLIB   DD DISP=SHR,DSN=TONYW.DINONFS.LOAD                        
//SYSPRINT  DD SYSOUT=*                                               
//STDOUT    DD SYSOUT=*,DCB=(RECFM=F,LRECL=250,BLKSIZE=250)           
//STDERR    DD SYSOUT=*,DCB=(RECFM=F,LRECL=250,BLKSIZE=250)           
//STDIN     DD DUMMY                                                  
//NFSDCONF  DD DISP=SHR,DSN=TONYW.DINONFS.CONF                        
//                                                                    