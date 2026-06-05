//TONYWC1 JOB (NFSD),'MAKE TEST STC GETCIB',                          
//      CLASS=A,MSGCLASS=X,MSGLEVEL=1,REGION=8M,                      
//      NOTIFY=TONYW                                                  
//*                                                                   
//********************************************************************
//*                                                                   
//* NAME: MKRESSCK                                                    
//*                                                                   
//* DESC: COMPILE AND LINK PROGRAM TO RESET SOCKETS
//*                                                                   
//********************************************************************
//*                                                                   
//RESSOCK EXEC JCCCL,INFILE='TONYW.DINONFS.C(RESSOCK)',         
//        PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',                  
//        OUTFILE='TONYW.DINONFS.LOAD(RESSOCK)',                      
//        JOPTS='-s -o -LIST=//DDN:SYSPRINT -D__MVS__'                
//COMPILE.JCCINCS DD DISP=SHR,DSN=TONYW.DINONFS.H                     
//                                                                    