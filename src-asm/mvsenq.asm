         TITLE 'MVSENQ - MVS ENQ/DEQ resources'
*
***********************************************************************
*  Setup                                                              *
***********************************************************************
         PRINT NOGEN
         YREGS
         PRINT GEN
*
***********************************************************************
*  Entry and stack frame linkage                                      *
***********************************************************************
MVSENQ   CSECT
         JCCPROLG
         LR    R12,R15          establish module addressability
         USING MVSENQ,R12
*
         ST    R1,PARMLIST      Save the callers parameter list addr
***********************************************************************
* MVSENQ processing                                                   *
*                                                                     *
* The parameters passed are --                                        *
*    uint8_t   request_type                                           *
*                 1 - ENQ                                             *
*                 2 - DEQ                                             *
*                 3 - TEST                                            *
*    uint8_t   options                                                *
*                 0x01 - Exclusive (SHR if bit not set)               *
*    char     *qname, null terminated, max 8 + \0 chars               *
*    char     *rname, null terminated, max 255 + \0 chars             *
*                                                                     *
* Note that when JCC passes an unsigned char value as a parameter it  *
* is in the parm list as a 32 bit value. Thus, loading it as a 32 bit *
* value is correct. Loading a character (IC/ICM) is wrong as it would *
* load the high order byte of the parameter, which would be zero.     *
***********************************************************************
         L     R1,PARMLIST
*
*---------------------------------------------------------------------*
* Copy QNAME, RNAME and get length of RNAME - used for all req types  *
*---------------------------------------------------------------------*
         L     R2,8(R1)     3rd parm - qname
         LA    R3,QNAME     Destination
         LA    R4,L'QNAME   Length of destination field
         BAL   R6,STRCOPY   Copy string
*
         L     R2,12(R1)    4th parm - rname
         LA    R3,RNAME     Destination
         LA    R4,L'RNAME   Length of destination field
         BAL   R6,STRCOPY   Copy string
         ST    R0,RNAMELEN  Save length of the rname value
*
*---------------------------------------------------------------------*
* Process request type                                                *
*---------------------------------------------------------------------*
         L     R2,0(R1)      Load request type. Even though it
*                            is defined as an unsigned char, JCC
*                            will pass the value as a 32 bit value in
*                            the parm list.
         C     R2,=F'1'
         BE    DO@ENQ        Request is allocate
         C     R2,=F'2'
         BE    DO@DEQ        Request is unallocate
         C     R2,=F'3'
         BE    DO@TEST       Request is unallocate
         B     RETC@M1       Request type is invalid
*
***********************************************************************
***********************************************************************
* ENQ TEST request                                                    *
***********************************************************************
***********************************************************************
DO@TEST  DS    0H
*
*        WTO   'MVSENQ: Starting ENQ/TEST request'
         L     R1,PARMLIST
*
         L     R4,RNAMELEN   Load length of RNAME
         L     R2,4(R1)      Get options from 2nd parm
         N     R2,=XL4'00000001' AND for low bit
         BNZ   DOTSTEXC      It was set ... we want exclusive control
*
*---------------------------------------------------------------------*
* Execute a shared ENQ                                                *
*---------------------------------------------------------------------*
*
DOTSTSHR DS    0H
         ENQ   (QNAME,RNAME,S,(R4),SYSTEMS),RET=TEST
         B     DOTSTRC
*
*---------------------------------------------------------------------*
* Execute an exclusive ENQ                                            *
*---------------------------------------------------------------------*
*
* EXCLUSIVE
*
DOTSTEXC DS    0H
         ENQ   (QNAME,RNAME,E,(R4),SYSTEMS),RET=TEST
*
DOTSTRC  DS    0H           Test return code
         LTR   R15,R15      Was RC = 0
         BZ    RETC@0       Yes ... exit
*---------------------------------------------------------------------*
* Output a WTO with the RC from ENQ                                   *
*---------------------------------------------------------------------*
*
         L     R3,0(R15)    Load the return code for the 1st resource
         N     R3,=XL4'000000FF' - mask out other bytes
         MVC   WTOMSG,=CL45'Test ENQ failed'
         MVC   WTOREG,=CL3'RC'     ENQ RC value is now in R3
*        BAL   R7,DEBUGWTO
*
         LR    R15,R3
         B     RETURN       Return error code to caller
*
***********************************************************************
***********************************************************************
* ENQ request                                                         *
***********************************************************************
***********************************************************************
DO@ENQ   DS    0H
*
*        WTO   'MVSENQ: Starting ENQ request'
*
         L     R4,RNAMELEN   Load length of RNAME
         L     R1,PARMLIST
         L     R2,4(R1)      Get options
         N     R2,=XL4'00000001' And for low bit
         BNZ   DOENQEXC      It was set ... we want exclusive control
*
*---------------------------------------------------------------------*
* Execute a shared ENQ                                                *
*---------------------------------------------------------------------*
*
DOENQSHR DS    0H
*        WTO   'MVSENQ: Issuing ENQ/SHR'
         ENQ   (QNAME,RNAME,S,(R4),SYSTEMS),RET=USE
         B     DOENQRC
*
*---------------------------------------------------------------------*
* Execute an exclusive ENQ                                            *
*---------------------------------------------------------------------*
*
* EXCLUSIVE
*
DOENQEXC DS    0H
*        WTO   'MVSENQ: Issuing ENQ/EXCL'
         ENQ   (QNAME,RNAME,E,(R4),SYSTEMS),RET=USE
*
DOENQRC  DS    0H           Test return code
         LTR   R15,R15      Was RC = 0
         BZ    RETC@0       Yes ... exit
*---------------------------------------------------------------------*
* Output a WTO with the RC from ENQ                                   *
*---------------------------------------------------------------------*
*
         L     R3,0(R15)    Load the return code for the 1st resource
*        WTO   'MVSENQ: ENQ error...'
*        N     R3,=XL4'000000FF' - mask out other bytes
*        MVC   WTOMSG,=CL45'ENQ failed'
*        MVC   WTOREG,=CL3'RC'     ENQ RC value is now in R3
*        BAL   R7,DEBUGWTO
*
         LR    R15,R3
         B     RETURN       Return error code to caller
*
***********************************************************************
***********************************************************************
* DEQ request                                                         *
***********************************************************************
***********************************************************************
DO@DEQ   DS    0H
*
*        WTO   'MVSENQ: Starting DEQ request'
*---------------------------------------------------------------------*
* Execute DEQ                                                         *
*---------------------------------------------------------------------*
         L     R4,RNAMELEN   Load length of RNAME
         DEQ   (QNAME,RNAME,(R4),SYSTEMS),RET=HAVE
         LTR   R15,R15       Was RC = 0
         BZ    RETC@0        Yes ... exit
*---------------------------------------------------------------------*
* Output a WTO with the RC from ENQ                                   *
*---------------------------------------------------------------------*
*
         L     R3,0(R15)    Load the return code for the 1st resource
*        N     R3,=XL4'000000FF' - mask out other bytes
*        MVC   WTOMSG,=CL45'DEQ failed'
*        MVC   WTOREG,=CL3'RC'     ENQ RC value is now in R3
*        BAL   R7,DEBUGWTO
*
         LR    R15,R3
         B     RETURN       Return error code to caller
*
***********************************************************************
***********************************************************************
* COMMON EXIT POINTS                                                  *
***********************************************************************
***********************************************************************
*
*---------------------------------------------------------------------*
* Return codes and restore regs                                       *
*---------------------------------------------------------------------*
*
RETC@M1  DS    0H
         L     R15,=F'-1'
         B     RETURN
*
RETC@0   DS    0H
         SR    R15,R15               R15=0 and fall through
*
RETURN   JCCRETRN                    R15 has return value
*
***********************************************************************
***********************************************************************
* Subroutines                                                         *
***********************************************************************
***********************************************************************
*
         COPY  STRCOPY
*
*---------------------------------------------------------------------*
* Debug output WTO                                                    *
*---------------------------------------------------------------------*
*
DEBUGWTO DS    0H
         CVD   R3,DOUBLE           Convert R3 binary to Packed Decimal
         UNPK  WTOVAL(10),DOUBLE+3(5) Unpack last 5 bytes (9 digs+sign)
         OI    WTOVAL+9,X'F0'     Fix the sign zone to make it prntbl
         WTO   MF=(E,WTOPLIST)     Execute the Write-To-Operator
         BR    R7
*
DBGX4WTO DS    0H
         STCM  R3,B'1111',WORK     Store 4 bytes of R3 into work area
         UNPK  HEXOUT(9),WORK(5)   Unpack 4 bytes into 9 bytes
*                                  (zones/digits)
         TR    HEXOUT(8),HEXTAB-240 Translate the zones into
*                                  EBCDIC hex chars
         MVC   WTOVAL(2),=CL2'0x'
         MVC   WTOVAL+2(8),HEXOUT
         WTO   MF=(E,WTOPLIST)     Execute the Write-To-Operator
         BR    R7
*
CONVFW2X DS    0H
         STCM  R3,B'1111',WORK     Store 4 bytes of R3 into work area
         UNPK  HEXOUT(9),WORK(5)   Unpack 4 bytes into 9 bytes
*                                  (zones/digits)
         TR    HEXOUT(8),HEXTAB-240 Translate the zones into
*                                  EBCDIC hex chars
         BR    R7
*
***********************************************************************
***********************************************************************
* Static working storage and macros                                   *
***********************************************************************
***********************************************************************
*
PARMLIST DS    F           Address of callers parm list (a full word!)
*
QNAME    DS    CL8         QNAME value for ENQ/DEQ/TEST
RNAME    DS    CL255       RNAME value for ENQ/DEQ/TEST
RNAMELEN DS    F           Length of the rname parameter
*
*---------------------------------------------------------------------*
* Debug WTO list form equivalent                                      *
*---------------------------------------------------------------------*
*
* Area for debug WTO message
DOUBLE   DS    D              8-byte doubleword work area for CVD
WORK     DS    CL5            5-byte temporary work area
HEXOUT   DS    CL8            Output buffer for the 8 hex characters
         DS    C              Extra byte needed for UNPK padding logic
HEXTAB   DC    C'0123456789ABCDEF' Translation table for hex digits
*
WTOPLIST DS    0F
         DC    AL2(WTOLEN)         Total length of WTO buffer (Data+4)
         DC    XL2'0000'           MCS flags
         DC    CL8'MVSENQ: '
WTOMSG   DC    CL45'PROMPT TEXT'
WTOREG   DC    CL3'R3 '
         DC    C'='
WTOVAL   DC    C'XXXXXXXXXX'       Message text area (10 bytes for val)
WTOLEN   EQU   *-WTOPLIST
*
*---------------------------------------------------------------------*
         LTORG
         END   MVSENQ