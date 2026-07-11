*
*---------------------------------------------------------------------*
* JCC standard entry to establish stack frame                         *
*---------------------------------------------------------------------*
         MACRO
&LABEL   JCCPROLG &FRAME=84
&LABEL   DS    0H
* R15: Entry addr, R14: Return addr, R13: Caller provided stack frame
* 0(R13) address of stack control block
* 4(R13) address of prev stack frame
* 8(R13) address of next stack frame
* 12(R13) -> 68(R13) Save area for R14 to R12.
         STM   R14,R12,12(R13)       Save registers R14,R15,R0-R12
         L     R2,8(,R13)            Get the storage addr offered
         LA    R14,&FRAME.(,R2)      Calc end address of that storage
         L     R12,0(,R13)           Get addr of stack control block
         CL    R14,4(,R12)           Fits in current segment?
         BL    JPX&SYSNDX-&SYSECT+4(,R15)   yes - skip the extend call
         L     R10,0(,R12)           Stack-extension routine from SCB
         BALR  R11,R10                  call it
         CNOP  0,4
JPX&SYSNDX DS  0H
         DC    A(&FRAME)             Frame size read by the extender
         STM   R12,R14,0(R2)         Init new stack frame, ...
*                                       with SCB, prev, next 
         LR    R13,R2                Set new frame base in R13
         MEND