         TITLE 'GETCIB - MVS STOP Command Checker'
         PRINT GEN
GETCIB   CSECT
***********************************************************************
*                                                                     *
* GETCIB - Inspect the MVS CIB queue for a STOP (P) command           *
*                                                                     *
* JCC C prototype:  extern int getcib(void);                          *
*                                                                     *
* Return codes                                                        *
*   -1  This address space is not a started task (STC)                *
*    0  No STOP/MODIFY command is currently queued                    *
*    1  MODIFY command received; CIB has been freed via QEDIT         *
*    2  STOP command received; CIB has been freed via QEDIT           *
*                                                                     *
* Calling convention: Modified MVS linage as per JCC                  *
*   Entry  R13 -> Stack frame from JCC runtime                        *
*          R14 =  return address                                      *
*          R15 =  entry point address                                 *
*          R1  =  Address of parameter list                           *
*                 Parm 1 - Address of callers command save buffer     *
*                 Parm 2 - Length of callers command save buffer      *  
*                 Parm 3 - An address of an int32 to receive length   *
*                          of CIB data                                *
*   Exit   R15 =  integer return code                                 *
*          R2-R12 preserved (restored from caller save area)          *
*                                                                     *
* Restrictions                                                        *
*   NOT re-entrant: uses static storage                               *
*                                                                     *
* Control block offsets                                               *
*   The STC indicator is checked via PSA -> TCB -> JSCB (IEZJSCB).    *
*   Offset JSCBSFL (X'33'), bit JSCBSTCF (X'10') below are based on   *
*   OS/VS2 MVS 3.8J.  Verify against the IEZJSCB macro listing for    *
*   your specific MVS level if the STC check behaves unexpectedly.    *
*                                                                     *
***********************************************************************
         YREGS
***********************************************************************
*  Entry and save-area linkage                                        *
***********************************************************************
*
* This code was copied from FTPLOGIN by Juergen Winkelmann, ETH Zuerich
*
         STM   R14,R12,12(R13)  save registers                        
         L     R2,8(,R13)       \                                     
         LA    R14,96(,R2)       \                                    
         L     R12,0(,R13)        \                                   
         CL    R14,4(,R12)         \                                  
         BL    F1-GETCIB+4(,R15)    \                                 
         L     R10,0(,R12)           \ save area chaining             
         BALR  R11,R10               / and JCC prologue               
         CNOP  0,4                  /                                 
F1       DS    0H                  /                                  
         DC    F'96'              /                                   
         STM   R12,R14,0(R2)     /                                    
         LR    R13,R2           /                                     
         LR    R12,R15          establish module addressability       
         USING GETCIB,R12       tell assembler of base                
         ST    R1,PARMLIST      Save the callers parameter list addr 
*
***********************************************************************
*  Verify that this address space is a started task.                  *
*                                                                     *
*  Navigation: absolute PSA+X'21C' (PSATOLD)                          *
*           -> current TCB                                            *
*           -> TCB+X'B4'  (TCBJSCB) -> JSCB                           *
*           -> JSCB+X'33' (JSCBSFL) -> inspect JSCBSTCF bit           *
***********************************************************************
* This code is not working on MVS 3.8J. The macro does not define
* JSCBSFLG, although it does not define JSCBSWT1. The bit masks
* in the macro do not list a bit for STC (they are all "reserved"). 
*
* TODO: More investigation needed.
*
*        LA    R2,16                 Fixed address for CVT
*        USING CVT,R2
*        L     R3,CVTTCBP            Load CVT TCB pointer
*        L     R4,0(R3)              Load addr of TCB
*        DROP  R2                    No longer needed
*        USING TCB,R4
*        L     R5,TCBJSCB            Load JSCB addr from TCB
*        USING IEZJSCB,R5
*        DROP  R4
*        TM    JSCBSWT1,X'01'        Is the STC bit set?
*        BZ    RETMIN1               No -> not a started task
*        DROP  R5
*
***********************************************************************
*  Retrieve the CIB chain pointer for this address space.             *
*                                                                     *
*  EXTRACT fills CSCLADDR with the address of CSCL. The CSCL contains *
*  the ECB address and the CIB address.                               *
***********************************************************************
*
CIBACCES DS    0H
*        WTO   'CIBGET: About to execute extract'
         EXTRACT CSCLADDR,FIELDS=COMM
*
         L     R10,CSCLADDR
         USING CSCL,R10
         CLC   COMCIBPT,=F'0'        Is the CIB chain pointer null?
         BE    RETZ                  No command queued
*
         L     R11,COMCIBPT          Load addr of CIB
         USING CIB,R11
*CIBSTART EQU   X'04' -  COMMAND CODE FOR START  
*CIBMODFY EQU   X'44' -  COMMAND CODE FOR MODIFY 
*CIBSTOP  EQU   X'40' -  COMMAND CODE FOR STOP            
         CLI   CIBVERB,CIBSTART      Is this for the start command?
         BNE   NOTSTART              No, not the start command CIB
*        WTO   'CIBGET: Found CIB for START command'
         BAL   R6,DOSTART            Yes, we'll free it. 
         BAL   R6,SETCIBN            Set CIB limit
         B     CIBACCES              Check for other verbs
*
NOTSTART DS    0H
         CLI   CIBVERB,CIBMODFY      Is this a MODIFY command?
         BNE   NOTMODFY              No, not the modify command CIB
*        WTO   'CIBGET: Found CIB for MODIFY command'
         BAL   R6,DOMODIFY           Yes ... process
         B     RETONE                Return 1 - MODIFY cmd processed   
*
NOTMODFY DS    0H
         CLI   CIBVERB,CIBSTOP       Is this a STOP command?
         BNE   NOTSTOP               No, not the stop command CIB
*        WTO   'CIBGET: Found CIB for STOP command'
         BAL   R6,DOSTOP             Yes, we'll process it.
         B     RETTWO                Return 2 - STOP cmd processed
*
* It is something else -- we will ignore
* 
NOTSTOP  DS    0H
         B     RETZ                  Return 0 - no STOP/MODIFY pending
*
***********************************************************************
*  Retrieve the CIB and free it                                       *
***********************************************************************
*
DOSTART  DS    0H
*        WTO   'CIBGET: Found start command CIB - freeing it'
         QEDIT ORIGIN=COMCIBPT,BLOCK=(R11)  Free processed CIB
         BR    R6                    Return to caller
*
***********************************************************************
*  Set the CIB limit                                                  *
***********************************************************************
*
SETCIBN  DS    0H
*        WTO   'CIBGET: Setting CIB limit to 4'
         QEDIT ORIGIN=COMCIBPT,CIBCTR=4
         BR    R6                    Return to caller
*
***********************************************************************
*  Retrieve the CIB, save the modify command and free the CIB         *
***********************************************************************
*
DOMODIFY DS    0H
*        WTO   'CIBGET: Found modify command CIB'
         L     R2,PARMLIST           Load address of caller's parm list
         L     R3,0(R2)              Load caller's save buffer addr
         L     R4,4(R2)              Load caller's save buffer length
*
* Test if callers buffer is > 0 or save area is NULL
*
         SR    R5,R5                 Clear R5 for test
         CR    R3,R5                 Is caller's save buffer addr null?
         BE    DOMFREE               Yes -> skip command save
         CR    R4,R5                 Is length > 0?
         BE    DOMFREE               No -> skip command save
*
         LH    R5,CIBDATLN           Load length of data field in CIB
         CR    R4,R5                 Is caller's buffer big enough?
         BNL   DMSAVE                Yes         
         LR    R5,R4                 No, we'll truncate the data
* 
DMSAVE   DS    0H
         L     R7,8(R2)              Load address of caller's int32 for
*                                    length of data 
         ST    R5,0(R7)              Return length of data being copied
*        WTO   'CIBGET: Copying modify command data'
         BCTR  R5,0                  Decrement length for EX/MVC
         EX    R5,MVCINSTR           Copy the CIB data
*
DOMFREE  DS    0H
*        WTO   'CIBGET: Freeing modify command CIB'
         QEDIT ORIGIN=COMCIBPT,BLOCK=(R11)  Free processed CIB
*
         BR    R6                    Return to caller
*
***********************************************************************
*  Have stop command ... free the CIB                                 *
***********************************************************************
*
DOSTOP   DS    0H 
*        WTO   'CIBGET: Found stop command CIB - freeing it'
         QEDIT ORIGIN=COMCIBPT,BLOCK=(R11)  Free processed CIB
         BR    R6                    Return to caller
*
***********************************************************************
*  End of CIB processing code                                         *
***********************************************************************
*
MVCINSTR MVC   0(0,R3),CIBDATA       Copy the CIB data (via EX)
*
         DROP  R11
         DROP  R10
*
***********************************************************************
*  Return paths                                                       *
***********************************************************************
*
*  Return 1 - STOP command was received
*
RETTWO   DS    0H
*        WTO   'CIBGET: STOP command received - RC=1'
         L     R13,4(,R13)           Restore caller's save area ptr
         LM    R14,R12,12(R13)       Restore caller's R14 through R12
         LA    R15,2                 Return code = 2
         BR    R14                   Return to caller
*
*  Return 1 - STOP command was received
*
RETONE   DS    0H
*        WTO   'CIBGET: MODIFY command received - RC=2'
         L     R13,4(,R13)           Restore caller's save area ptr
         LM    R14,R12,12(R13)       Restore caller's R14 through R12
         LA    R15,1                 Return code = 1
         BR    R14                   Return to caller
*
*  Return 0 - no STOP command pending
*
RETZ     DS    0H
*        WTO   'CIBGET: No STOP command pending - RC=0'
         L     R13,4(,R13)
         LM    R14,R12,12(R13)
         SLR   R15,R15               Return code = 0
         BR    R14
*
*  Return -1 - not a started task
*    LA can only load non-negative values; negate 1 to obtain -1.
*
RETMIN1  DS    0H
*        WTO   'CIBGET: Not a STC - RC=-1'
         L     R13,4(,R13)
         LM    R14,R12,12(R13)
         L     R15,=F'-1'            Load R15 with -1
         BR    R14
*
***********************************************************************
*  Static working storage and macros                                  *
***********************************************************************
*
*SAVE     DS    18F                   Standard 72-byte save area
PARMLIST DS    F                     Address of caller's parameter list
CSCLADDR DS    F                     CSCL addr from EXTRACT
*CIBADDR  DS    F                     CIB chain head ptr (EXTRACT o/p)
*
         LTORG                       Assemble any deferred literals
CSCL     DSECT                       
         IEZCOM                      Macro for CSCL DSECT
CIB      DSECT
         IEZCIB                      Macro for CIB DSECT
*
         CVT DSECT=YES               Mapping for CVT
         IHAPSA                      Macro for PSA
         IEZJSCB                     Macro for JSCB 
         IKJTCB                      Macro for TCB 
         END   GETCIB
