//TONYWC1 JOB (NFSD),'MAKE TEST STC GETCIB',                          
//      CLASS=A,MSGCLASS=X,MSGLEVEL=1,REGION=8M,                      
//      NOTIFY=TONYW                                                  
//*                                                                   
//********************************************************************
//*                                                                   
//* NAME: MKRUNCIB                                                    
//*                                                                   
//* DESC: COMPILE AND LINK TEST STC that calls getcib(...)            
//*                                                                   
//********************************************************************
//*                                                                   
//GETCIB EXEC ASMFCL,MAC1='SYS1.AMODGEN',MAC2='SYS2.MACLIB',          
//        PARM.ASM=(OBJ,NODECK)                                       
//ASM.SYSPUNCH DD SYSOUT=*                                            
//ASM.SYSIN    DD DISP=SHR,DSN=TONYW.DINONFS.ASM(GETCIB)              
//*SM.SYSGO    DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(GETCIB)           
//LKED.SYSLMOD DD DISP=SHR,DSN=TONYW.DINONFS.LOAD                     
//LKED.SYSIN   DD *                                                   
    NAME GETCIB(R)                                                    
//*                                                                   
//TESTCIB EXEC JCCCL,INFILE='TONYW.DINONFS.TESTS.C(TESTCIB)',         
//        PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',                  
//        OUTFILE='TONYW.DINONFS.LOAD(TESTCIB)',                      
//        JOPTS='-s -o -LIST=//DDN:SYSPRINT -D__MVS__'                
//COMPILE.JCCINCS DD DISP=SHR,DSN=TONYW.DINONFS.H                     
//LKED.SYSLIN DD DSN=&&OBJMOD,DISP=(OLD,DELETE)                       
//          DD  DDNAME=SYSIN                                          
//LKED.SYSLIB DD DISP=SHR,DSN=TONYW.DINONFS.LOAD                      
//LKED.SYSIN DD *                                                     
    INCLUDE SYSLMOD(GETCIB)                                           
    NAME TESTCIB(R)                                                   
//                                                                    