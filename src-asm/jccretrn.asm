*---------------------------------------------------------------------*
* Return to JCC caller                                                *
*---------------------------------------------------------------------*
         MACRO
&LABEL   JCCRETRN
.* Note that JCC stack is different from standard MVS save area
.* conventions in that the second word of the stack frame
.* is the pointer back to the previous stack frame. 
.* Standard MVS save area format would have the third word
.* pointing backwards. 
&LABEL   L     R13,4(0,R13)          Get previous stack frame
         L     R14,12(0,R13)         Restore R14
         LM    R1,R12,24(R13)        Restore R1-R12
         BR    R14                   Return to caller
         MEND 