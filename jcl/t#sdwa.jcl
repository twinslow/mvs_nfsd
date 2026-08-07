//TONYWZ1  JOB (MVSNFSD),
//             'SDWA',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Mapping DSECT IHASDWA
//*
//********************************************************************
//SDWA EXEC ASMFC,PARM.ASM=(OBJ,NODECK),MAC1='SYS2.MACLIB'
//ASM.SYSIN DD *
       IHASDWA DSECT=YES
       END
//