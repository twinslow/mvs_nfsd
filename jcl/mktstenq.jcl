//TONYWE1 JOB (NFSD),'MAKE TEST ENQ',                          
//      CLASS=A,MSGCLASS=X,MSGLEVEL=1,REGION=8M,                      
//      COND=(0,LT),
//      NOTIFY=TONYW                                                  
//*                                                                   
//********************************************************************
//*                                                                   
//* NAME: MKTSTENQ                                                    
//*                                                                   
//* DESC: COMPILE, LINK AND GO test calls MVSENQ
//*                                                                   
//********************************************************************
//*                                                                   
//MVSENQ EXEC ASMFCL,MAC1='SYS1.AMODGEN',MAC2='SYS2.MACLIB',
//        MAC3='TONYW.DINONFS.ASM',          
//        PARM.ASM=(OBJ,NODECK)                                       
//ASM.SYSPUNCH DD SYSOUT=*                                            
//ASM.SYSIN    DD DISP=SHR,DSN=TONYW.DINONFS.ASM(MVSENQ)              
//LKED.SYSLMOD DD DISP=SHR,DSN=TONYW.DINONFS.LOAD                     
//LKED.SYSIN   DD *                                                   
    NAME MVSENQ(R)                                                    
//*                                                                   
//TESTENQ EXEC JCCCL,INFILE='TONYW.DINONFS.TESTS.C(TESTENQ)',         
//        PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',                  
//        OUTFILE='TONYW.DINONFS.LOAD',                      
//        JOPTS='-s -o -LIST=//DDN:SYSPRINT -D__MVS__'                
//COMPILE.JCCINCS DD DISP=SHR,DSN=TONYW.DINONFS.H                     
//LKED.SYSLIN DD DSN=&&OBJMOD,DISP=(OLD,DELETE)                       
//          DD  DDNAME=SYSIN                                          
//LKED.SYSLIB DD DISP=SHR,DSN=TONYW.DINONFS.LOAD                      
//LKED.SYSIN DD *                                                     
    INCLUDE SYSLMOD(MVSENQ)                                           
    NAME TESTENQ(R)                                                   
//*
//*--------------------------------------------------------  
//RUN      EXEC PGM=TESTENQ                         
//STEPLIB   DD  DISP=SHR,DSN=TONYW.DINONFS.LOAD     
//STDERR    DD  SYSOUT=*                            
//STDOUT    DD  SYSOUT=*                            
//STDIN     DD  DUMMY                               
//                                                  