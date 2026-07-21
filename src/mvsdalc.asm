         TITLE 'MVSDALC - MVS Dynamic Allocation Module'
*
***********************************************************************
*  Setup                                                              *
***********************************************************************
         PRINT NOGEN
         YREGS
         IEFZB4D0               Gen DSECT for req block, TU etc.
         PRINT GEN
*
***********************************************************************
*  Entry and stack frame linkage                                      *
***********************************************************************
MVSDALC  CSECT
         JCCPROLG
         LR    R12,R15          establish module addressability       
         USING MVSDALC,R12
*
         ST    R1,PARMLIST      Save the callers parameter list addr 
***********************************************************************
* MVSDALC processing                                                  *
*                                                                     *
* The parameters passed are --                                        *
*    uint8_t   request_type                                           *
*                 1 - Alloc                                           *
*                 2 - Unalloc                                         *
*    uint8_t   options                                                *
*                 0x01 - Free=close                                   *
*    char     *dsn, null terminated                                   *
*    char     *member, null terminated. Optional and maybe NULL       *
*    char     *ddname, 8 bytes returned ddname, blank padded and      * 
*                  not null terminated. Used for allocate request     *
*                                                                     *
* Note that when JCC passes an unsigned char value as a parameter it  *
* is in the parm list as a 32 bit value. Thus, loading it as a 32 bit *
* value is correct. Loading a character (IC/ICM) is wrong as it would *
* load the high order byte of the parameter, which would be zero.     *
***********************************************************************
         L     R1,PARMLIST
         L     R2,0(R1)      Load request type. Even though it 
*                            is defined as an unsigned char, JCC
*                            will pass the value as a 32 bit value in
*                            the parm list.
         C     R2,=F'1'
         BE    DO@ALLOC      Request is allocate
         C     R2,=F'2'
         BE    DO@UNALC      Request is unallocate
         B     RETC@M1       Request type is invalid
*
***********************************************************************
***********************************************************************
* DYNAMIC ALLOCATION REQUEST                                          *
***********************************************************************
***********************************************************************
DO@ALLOC DS    0H
*---------------------------------------------------------------------*
* Copy dataset name, member name to text units                        *
*---------------------------------------------------------------------*
         L     R1,PARMLIST
*
         L     R2,8(R1)     3rd parm - dataset name
         LA    R3,TUDSN     Destination
         LA    R4,L'TUDSN   Length of destination field
         BAL   R6,STRCOPY   Copy string
*        STH   R0,DSNLEN    DALDSNAM length = actual name length 
*                           (not padded)
*
         L     R2,12(R1)    4th parm - member name
         LTR   R2,R2
         BZ    NOMEMNAM     If 4parm NULL then no member name
         LA    R3,TUMEMBR   Destination
         LA    R4,L'TUMEMBR Length of destination field        
         BAL   R6,STRCOPY   Copy string 
*       
         LA    R2,TUDSMEM   Get address of member name TU
         ST    R2,TUAPTRMN  Store TU address in LIST
         B     DONEMEM      Done with member name processing
*
NOMEMNAM DS    0H           No member name supplied 
         SR   R2,R2
         ST   R2,TUAPTRMN   Zero the address of the TU so that
*                           DYNALLOC ignores.
DONEMEM  DS    0H
*---------------------------------------------------------------------*
* Do we need the FREE=CLOSE text unit?                                *
*---------------------------------------------------------------------*
*
         L    R2,4(R1)      Load the options byte, which will be passed
*                           as a 32 bit value in the parm list.  
         N    R2,=XL4'00000001' Mask for low bit
         BNZ  USEFCLOS      Yes, we are going to use FREE=CLOSE TU   
*
*                           We are not using FREE=CLOSE, so we
*                           are going to turn on the high bit of 
*                           the TUPOLST TU addr, so that's the end of 
*                           the TU list.
         L    R2,TUPOLST
         O    R2,=XL4'80000000'
         ST   R2,TUPOLST
         B    DUNFCLOS
*
USEFCLOS DS   0H            We have to turn off the high bit of
*                           of the TUPOLST TU address, so that
*                           alloc picks up the FREE=CLOSE TU.
         L    R2,TUPOLST
         N    R2,=XL4'7FFFFFFF'    Turn off high bit
         ST   R2,TUPOLST
*  
DUNFCLOS DS   0H  
*     
*---------------------------------------------------------------------*
* Issue the allocation request                                        *
*---------------------------------------------------------------------*
*
         BAL   R6,SETRBALC  Setup the request block for allocate
*
*        LA    R3,REQBLK    Load address of the request block
*        USING S99RB,R3     
*        L     R2,S99TXTPP  Load the address of the TU list 
*        DROP  R3           using the request block 
*        MVC   WTOMSG,=CL45'Allocate TU list at '
*        BAL   R6,DUMPTULS  Dump the list of text units
* 
         LA    R1,REQBLKA   Load ptr location
         DYNALLOC           Do the SVC99
         LTR   R15,R15      Zero RC?
         BZ    ALLOCOK
* SVC99 fail
         LR    R3,R15       Save return code from DYNALLOC
         LA    R2,REQBLK    Load address of the request block
         USING S99RB,R2     
*
         WTO   'Dynamic allocation failed'         
         MVC   WTOMSG,=CL45'Dynamic alloc failed'
         MVC   WTOREG,=CL3'R15'     R15 value is now in R3
         BAL   R7,DEBUGWTO
         MVC   WTOMSG,=CL45'Dynamic alloc reason code'
         MVC   WTOREG,=CL3'RC '
         LH    R3,S99ERROR   Get reason code
         BAL   R7,DEBUGWTO
         MVC   WTOMSG,=CL45'Dynamic alloc info code'
         MVC   WTOREG,=CL3'IC '
         LH    R3,S99INFO   Get info code
         BAL   R7,DEBUGWTO
         MVC   WTOMSG,=CL45' ' 
         MVC   WTOMSG(44),TUDSN
         MVC   WTOREG,=CL3'LEN'
         LH    R3,DSNLEN
         BAL   R7,DEBUGWTO  
         DROP  R2
*
         B     RETC@M1       Return code -1       
*
ALLOCOK  DS    0H
*
*---------------------------------------------------------------------*
* Copy the assigned DDNAME to the caller's output buffer.             *
*                                                                     *
* SVC99 returned the ddname in DDNRET (the DALRTDDN text unit on the  *
* ALLOC request), so it belongs to THIS allocation -- dataset name    *
* AND member.  A separate info retrieval keyed on dataset name alone  *
* is ambiguous when the same dataset is allocated more than once at   *
* the same time (e.g. a memberless read allocation held concurrently) *
* and could hand back a foreign, memberless ddname.                   *
*---------------------------------------------------------------------*
*
         L     R1,PARMLIST
         L     R2,16(R1)     5th param is buff for ddname
         MVC   0(L'DDNRET,R2),DDNRET
         B     RETC@0       All good... RC = 0
*
***********************************************************************
***********************************************************************
* DYNAMIC UNALLOCATION REQUEST                                        *
***********************************************************************
***********************************************************************
DO@UNALC DS    0H
*---------------------------------------------------------------------*
* Copy dataset name, member name to text units                        *
*---------------------------------------------------------------------*
*
         LA    R2,TUUDSNA1    Get address of dsname TU
         ST    R2,TUUPTRDS    Store it in the list of TUs 
         LA    R2,TUUDSMEM    Get address of member TU
         O     R2,=XL4'80000000' Set high bit for end of list
         ST    R2,TUUPTRMN    Store it in the list of TUs  
*   
         L     R1,PARMLIST
*
         L     R2,8(R1)       3rd parm - dataset name
         LA    R3,TUUDSN      Destination
         LA    R4,L'TUUDSN    Length of destination field        
         BAL   R6,STRCOPY     Copy string 
*
         L     R2,12(R1)      4th parm - member name
         LTR   R2,R2
         BZ    NOMEMNA2       If 4parm NULL then no member name 
         LA    R3,TUUMEMBR    Destination
         LA    R4,L'TUUMEMBR  Length of destination field        
         BAL   R6,STRCOPY     Copy string 
*       
         B     DONEMEM2       Done with member name processing
*
NOMEMNA2 DS    0H             No member name supplied 
         LA    R2,TUUDSNA1    Get addr of dataset TU
         O     R2,=XL8'80000000' Set high bit on for end of list
         ST    R2,TUUPTRDS    Save it back in list 
*
DONEMEM2 DS    0H
*
* Unallocate by dataset name and possibly member name if specified
*
         BAL   R6,SETRBUNA  Setup the request block for unallocate
*
*        LA    R3,REQBLK    Load address of the request block
*        USING S99RB,R3     
*        L     R2,S99TXTPP  Load the address of the TU list 
*        DROP  R3           using the request block 
*        MVC   WTOMSG,=CL45'Unallocate TU list at '
*        BAL   R6,DUMPTULS  Dump the list of text units
*
         LA    R1,REQBLKA   Load ptr location
         DYNALLOC           Do the SVC99
         LTR   R15,R15
         BNZ   RETC@M1
         B     RETC@0         
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
*---------------------------------------------------------------------*
* Setup request block for allocate                                    *
*---------------------------------------------------------------------*
SETRBALC LA    R2,REQBLK
         USING S99RB,R2 
         XC    REQBLK,REQBLK           Zero the request block      
         MVI   S99RBLN,REQBLKLN        Set reqblk length
         MVI   S99VERB,S99VRBAL        Set verb for allocate
         LA    R3,TUPTRALC             TU list for alloc
         ST    R3,S99TXTPP             Store text pointer list
         DROP  R2 
         BR    R6 
*          
*---------------------------------------------------------------------*
* Setup request block for unallocate, used when info failed           *
*---------------------------------------------------------------------*
SETRBUNA LA    R2,REQBLK
         USING S99RB,R2 
         XC    REQBLK,REQBLK           Zero the request block      
         MVI   S99RBLN,REQBLKLN        Set reqblk length
         MVI   S99VERB,S99VRBUN        Set verb for unallocate
         LA    R3,TUPTRUNA             TU list for info
         ST    R3,S99TXTPP             Store text pointer list
         DROP  R2 
         BR    R6 
*
*---------------------------------------------------------------------*
* Copy a null terminated string to destination and pad with blanks    *
* On entry ...                                                        *
*    R2 - Source address                                              *
*    R3 - Destination address                                         *
*    R4 - Length of destination field                                 *
*    R6 - Return address                                              *
*---------------------------------------------------------------------*
STRCOPY  LR    R5,R4          R5 = remaining destination length
         SR    R8,R8          R0 = count of chars copied (return value)
         LTR   R5,R5          destination length zero?
         BZ    STRCEND        yes - nothing to do
*
STRCLP   CLI   0(R2),X'00'    end of source string?
         BE    STRCPAD        yes - go pad remainder
         MVC   0(1,R3),0(R2)  copy one byte
         LA    R2,1(R2)       bump source pointer
         LA    R3,1(R3)       bump destination pointer
         LA    R8,1(R8)       bump count of chars copied
         BCT   R5,STRCLP      decrement remaining, loop if > 0
         B     STRCEND        dest full - truncated, done
*
STRCPAD  LTR   R5,R5          any destination space left?
         BZ    STRCEND        no - done
         MVI   0(R3),C' '     blank fill
         LA    R3,1(R3)       bump destination pointer
         BCT   R5,STRCPAD     decrement remaining, loop if > 0
*
STRCEND  DS    0H
         LR    R0,R8          return copied (real) len in R0  
         BR    R6             return to caller (R0 = chars copied)
*
*---------------------------------------------------------------------*
* Dump the text unit list, which is at the address in R2              *
*---------------------------------------------------------------------*
DUMPTULS DS    0H
         
         LR    R3,R2
         MVC   WTOREG,=CL3'R2'     Address of TU list
         BAL   R7,DBGX4WTO
*
DUMPTU   DS    0H       
         L     R4,0(R2)            Get address of text unit
         LTR   R4,R4               Is the address zero?
         BNZ   DUMPTUNK            No, normal key     
         MVC   WTOMSG,=CL45'Key N/A (addr zero)'
         B     DUMPTUMS         
DUMPTUNK LH    R3,0(R4)            Get key
         BAL   R7,CONVFW2X         Convert to hexadecimal
         MVC   WTOMSG,=CL45'Key 0x0000 at '
         MVC   WTOMSG+6(4),HEXOUT+4    Add key to msg    
DUMPTUMS DS    0H
         LR    R3,R4               Output address of TU in msg
         BAL   R7,DBGX4WTO         Issue WTO 
* Check for last TU pointer
         L     R4,0(R2)            Get TU addr from list again
         N     R4,=XL4'80000000'   And with high bit 
         BNZ   DUMPTUXX            End of list if it was set
         LA    R2,4(R2)            Next TU pointer
         B     DUMPTU              And again
*
DUMPTUXX DS    0H
         MVC   WTOMSG,=CL45' ' 
         MVC   WTOMSG(44),TUDSN
         MVC   WTOREG,=CL3'LEN'
         LH    R3,DSNLEN
         BAL   R7,DEBUGWTO  
*
         MVC   WTOREG,=CL3'R3'
         WTO   'End of text units'
         BR    R6 
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
REQBLKA  DC    A(REQBLK+X'80000000')   The request block pointer
*
CALCRBL  EQU   S99RBEND-S99RB
REQBLK   DS    CL(CALCRBL)       The request block    
REQBLKLN EQU   L'REQBLK          Request block length 
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
         DC    CL10'MVSDALC: '
WTOMSG   DC    CL45'PROMPT TEXT'
WTOREG   DC    CL3'R3 '
         DC    C'='
WTOVAL   DC    C'XXXXXXXXXX'       Message text area (10 bytes for val)
WTOLEN   EQU   *-WTOPLIST
*
*---------------------------------------------------------------------*
* List of text unit pointers and the text units                       *
*---------------------------------------------------------------------*
*
* This is the TU list for the allocate
*
*
TUPTRALC DS    0F               Addr of TU for ...
         DC    A(TUDSNA1)          DSNAME
         DC    A(TURTDDN)      Return assigned ddname (always present)
TUAPTRMN DC    A(TUDSMEM)          Dataset member ... we zap to zero
*                                  if we are not specifying a member
*                                  name, or store the addr if we are.
*                                  (for reuse).
TUPOLST  DC    A(TUDSSA1)          Dataset status
         DC    A(TUDCLOSE+X'80000000') Free=close
*
* Now the text units for allocate
*
         DS    0F
TUDSNA1  DC    AL2(DALDSNAM),AL2(1)        DSN=
DSNLEN   DC    AL2(44)                    length -- patched at runtime
TUDSN    DC    CL44' '
TUDSMEM  DC    AL2(DALMEMBR),AL2(1),AL2(8)         MEMBER
TUMEMBR  DC    CL8' '
TUDSSA1  DC    AL2(DALSTATS),AL2(1),AL2(1),X'08'   DISP=SHR
TUDCLOSE DC    AL2(DALCLOSE),AL2(0)         FREE=CLOSE (flag, no parm)
TURTDDN  DC    AL2(DALRTDDN),AL2(1),AL2(8)  Return assigned ddname here
DDNRET   DC    CL8' '              SVC99 stores the ddname in this area
*
*---------------------------------------------------------------------*
*
* This is the TU list for the unallocate, used if SVC99 info failed
*
TUPTRUNA DS    0F
TUUPTRDS DC    A(TUUDSNA1)             DSNAME
TUUPTRMN DC    A(TUUDSMEM+X'80000000') Dataset member
*
TUUDSNA1 DC    AL2(DUNDSNAM),AL2(1),AL2(44)        DSN=
TUUDSN   DC    CL44' '
TUUDSMEM DC    AL2(DUNMEMBR),AL2(1),AL2(8)         MEMBER 
TUUMEMBR DC    CL8' '
*
         LTORG
         PRINT NOGEN
         IEFZB4D2               Gen table of equates for TU keys 
         END   MVSDALC