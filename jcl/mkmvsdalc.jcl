//TONYWA2 JOB (NFSD),'MAKE MVSDALC',
//      CLASS=A,MSGCLASS=X,MSGLEVEL=1,REGION=8M,
//      NOTIFY=TONYW
//*
//* Assemble + link-edit the SVC99 dynamic-allocation helper (mvs_dynalloc)
//* into the NFSD load library, the same way GETCIB is built (mkgetcib.jcl).
//*
//MVSDALC EXEC ASMFCL,MAC1='SYS1.AMODGEN',MAC2='SYS2.MACLIB',
//        PARM.ASM=(OBJ,NODECK)
//ASM.SYSPUNCH DD SYSOUT=*
//ASM.SYSIN    DD DISP=SHR,DSN=TONYW.NFSD.ASM(MVSDALC)
//LKED.SYSLMOD DD DISP=SHR,DSN=TONYW.NFSD.LOAD
//LKED.SYSIN   DD *
    NAME MVSDALC(R)
//
