//TONYWZ1  JOB (DINO),
//             'Test BLDL/STOW',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Get JES2 job id
//*
//********************************************************************
//STOWTST EXEC ASMFCLG,PARM.ASM=(OBJ,NODECK),MAC1='SYS2.MACLIB',   
//             REGION.GO=128K,PARM.GO='TEMP'
//ASM.SYSIN DD *                                                  
         YREGS
PDSUPDTE CSECT
         STM   R14,R12,12(R13)     SAVE REGISTERS IN CALLER'S AREA
         BALR  R12,0               ESTABLISH BASE REGISTER 12
         USING *,R12
         ST    R13,SAVEAREA+4      FORWARD CHAIN SAVEAREAS
         LA    R11,SAVEAREA         
         ST    R11,8(,R13)         BACKWARD CHAIN SAVEAREAS
         LR    R13,R11             POINT TO OUR SAVEAREA
         ST    R1,PARMLIST         Save address of parm list
*
*--- STEP 1: PARSE PARM FOR MEMBER NAME FROM JCL
*
         BAL   R6,GETMEMBR         Get the member name from parm fld
         LTR   R15,R15
         BNZ   ERR@NOPM  
*
*--- STEP 2: OPEN THE PDS VIA BPAM (REQUIRES DSORG=PO)
*
         OPEN  (PDSDCB,(OUTPUT))   OPEN PDS FOR DIRECTORY REWRITE
         TM    PDSDCB+48,X'10'     TEST IF OPEN WAS SUCCESSFUL
         BNO   ERR@OPEN            NO -> EXIT WITH OPEN ERROR
*
*--- STEP 3: ISSUE BLDL TO GET MEMBER METADATA                  
*
         BLDL  PDSDCB,BLDLLIST     LOOK UP EXISTING MEMBER STATS
         LTR   R15,R15             WAS MEMBER FOUND IN DIRECTORY?
         BNZ   ERR@BLDL            NO -> EXIT WITH BLDL ERROR
         WTO   'Member located with BLDL'
*
*--- STEP 4: UPDATE THE ISPF METADATA IN THE USER DATA
*
         BAL   R6,SETSTOWD         Setup STOW structure
         LTR   R15,R15             Test for error  
         BZ    DOFIND              No error, continue
         C     R15,=F'4'           Did we get rc 4 (no user data)
         BE    DOCLOSE             Continue on to close the PDS DCB
         B     ERR@OTHR            Other error 
*
*--- STEP 5: Issue a find with the TTRK value that came from BLDL
*
DOFIND   DS    0H
         BAL   R6,EXFIND           Execute the FIND
         LTR   R15,R15
         BNZ   ERR@FIND
*
*--- STEP 6: Issue the STOW REPLACE
*
         BAL   R6,EXSTOW
         LTR   R15,R15             Error? 
         BNZ   ERR@STOW            Yes, Exit with STOW error
*
*--- STEP 7: CLOSE PDS AND TERMINATE PROGRAM
*
DOCLOSE  DS    0H
         CLOSE (PDSDCB)            CLOSE THE DATASET
         WTO   'Close completed'
         SR    R15,R15             RETURN CODE = 0 (SUCCESS)
         B     EXIT
*
*---------------------------------------------------------------------*
* Error handling and exit paths                                       *
*---------------------------------------------------------------------*
*
ERR@NOPM DS    0H
         WTO   'No member name specified'
         LA    R15,4               RC=4: JCL MISSING PARM MEMBER NAME
         B     EXIT
*
ERR@OTHR DS    0H
         WTO   'Other error... ending'
         LA    R15,12              RC=4: JCL MISSING PARM MEMBER NAME
         B     EXIT
*
ERR@OPEN DS    0H     
         WTO   'Failed to open dataset/DCB'
         LA    R15,8               RC=8: FAILED TO OPEN DATASET
         B     EXIT
*
ERR@BLDL DS    0H
         WTO   'Member not found in PDS'
         LA    R15,12              RC=12: MEMBER NOT FOUND IN PDS
         B     EXIT
*
ERR@FIND DS    0H
         LR    R3,R15              Save return code
         LR    R4,R0               Save reason code 
         WTO   'FIND rejected/failed'
         MVC   WTOMSG,=CL30'FIND return code'
         MVC   WTOREG,=CL3'R15'
         BAL   R7,DEBUGWTO
         MVC   WTOMSG,=CL30'FIND reason code'
         MVC   WTOREG,=CL3'R0'
         LR    R3,R4
         BAL   R7,DEBUGWTO
*
         CLOSE (PDSDCB)
         LA    R15,15              RC=15: FIND MACRO REJECTION
         B     EXIT
*
ERR@STOW DS    0H
         WTO   'STOW rejected/failed'
         LA    R15,16              RC=16: STOW MACRO REJECTION
         CLOSE (PDSDCB)
*
EXIT     L     R13,SAVEAREA+4      RESTORE CALLER'S SAVEAREA
         L     R14,12(,R13)        RESTORE RETURN REGISTER
         LM    R0,R12,20(R13)      RESTORE REMAINING REGISTERS
         BR    R14                 RETURN TO MVS OS
*
***********************************************************************
***********************************************************************
**                                                                   **
** Subroutines                                                       **
**                                                                   **
***********************************************************************
***********************************************************************
*
*---------------------------------------------------------------------*
* Parse JCL parm field for member name
*---------------------------------------------------------------------*
*
GETMEMBR DS    0H
         L     R1,PARMLIST
         L     R2,0(,R1)           R2 -> PARM POINTER (HALFWORD LEN)
         LH    R3,0(,R2)           R3 = Length of parm seting
         LTR   R3,R3               Is the parm length zero?
         BNZ   HAVEPARM            No, so we a have a parameter
         LA    R15,8               RC=8, No parameter specified
         BR    6
HAVEPARM DS    0H
         CH    R3,=H'8'            IS IT GREATER THAN 8 CHARS?
         BH    FIX@LEN             YES -> CAP IT TO 8
         B     MOVE@PM             NO -> PROCEED
FIX@LEN  LA    R3,8                FORCE MAXIMUM 8 BYTES
MOVE@PM  BCTR  R3,0                DECREMENT LENGTH BY 1 FOR EX EXECUTE
         MVC   MEMBER,=CL8' '      INITIALIZE MEMBER FIELD WITH SPACES
         EX    R3,EX@MVC           EXECUTE THE MVC TO EXTRACT MEMBER
         SR    R15,R15             RC=0, all ok
         BR    R6                  Return to caller
*
*---------------------------------------------------------------------*
* Setup structure for the STOW and update ISPF stats                  *
*---------------------------------------------------------------------*
SETSTOWD DS    0H
*
* THE USER DATA PORTION IN BLDL@USR STARTS AT OFFSET 12 OF THE ENTRY.
* ISPF FORMAT IS:
*   OFFSET +0 (1 BYTE):  VERSION (PACKED HEX OR BINARY)
*   OFFSET +1 (1 BYTE):  MOD LEVEL 
*   OFFSET +4 (4 BYTES): CREATION DATE (00YYDDDF IN PACKED DECIMAL)
*   OFFSET +8 (4 BYTES): MODIFICATION DATE (00YYDDDF IN PACKED DECIMAL)
*   OFFSET +12(2 BYTES): MODIFICATION TIME (HHMM IN PACKED DECIMAL)
*   OFFSET +14(2 BYTES): CURRENT NUMBER OF LINES
*   OFFSET +16(2 BYTES): INITIAL NUMBER OF LINES
*   OFFSET +18(2 BYTES): MODIFIED LINES COUNT
*   OFFSET +20(8 BYTES): USERID (CHARACTER FORMAT)
*
         XC    STOW@USR,STOW@USR
         MVC   STOW@MEM,MEMBER     COPY MEMBER NAME
         MVC   STOW@TTR,BLDL@TTR   COPY TTR
         MVC   STOW@UDL,BLDL@FLG   COPY USER DATA LEN AND FLAGS
         WTO   'Copied base directory entry data'
         IC    R3,BLDL@FLG         GET FLAGS AND LENGTH
         N     R3,=XL4'1F'         MASK OUT JUST THE LENGTH
*
         MVC   WTOMSG,=CL30'USER DATA HW LENGTH'
         BAL   R7,DEBUGWTO
*
         LTR   R3,R3               Test for user data length
         BNZ   HASUD
         WTO   'This member has no user data'
         LA    R15,4
         BR    R6
*
HASUD    SLL   R3,1                DOUBLE THE HW COUNT FOR BYTE COUNT
         BCTR  R3,0                DECREMENT FOR THE EX/MVC
*
         MVC   WTOMSG,=CL30'EX/MVC LENGTH'
         BAL   R7,DEBUGWTO
*
*
         WTO   'Aboutto copy directory entry user data'
         EX    R3,EX@CPYUD         COPY USER DATA   
         WTO   'Copied directory entry user data'
* 
         MVI   STOW@USR+0,X'0F'    FORCE VERSION TO 15
         MVI   STOW@USR+1,X'10'    FORCE MODIFICATION LEVEL TO 16
*                                                     (JULY 9, 2026) 
         MVC   STOW@USR+8(4),=X'0126190F'  MOD DATE: PACKED 2026/190
         MVC   STOW@USR+12(2),=X'1656'     MOD TIME: PACKED 1656
         MVC   STOW@USR+20(8),=CL8'TESTUSER' FORCE NEW USER ID
         WTO   'Updated ISPF user data'
*
         BR    R6
*
*---------------------------------------------------------------------*
* Execute the FIND to set TTRK value in DCB                           *
*---------------------------------------------------------------------*
EXFIND   DS    0H
*
* The DCB must be updated to have the TTRK value of the member. We
* got this back in the BLDL, so well use FIND to set the DCB. This
* does not read the directory entry again.
*
         FIND  PDSDCB,BLDLTTRK,C   Set the TTRK in the DCB
         LTR   R15,R15
         BZ    FINDOK
         LA    R15,8               Find error
         BR    R6 
*
FINDOK   WTO   'FIND completed'
         BR    R6
*
*---------------------------------------------------------------------*
* Perform the STOW REPLACE                                            *
*---------------------------------------------------------------------*
EXSTOW   DS    0H          
*
         MVC   WTOMSG,=CL30'XXXXXXXX member has UD len'
         MVC   WTOMSG(8),STOW@MEM
         IC    R3,STOW@UDL
         N     R3,=XL4'1F'
         BAL   R7,DEBUGWTO
*
         STOW  PDSDCB,STOWBUFF,R   Issue the STOW REPLACE
         LTR   R15,R15             Did the STOW succeed?
         BZ    STOWOK              Yes, continue
         LA    R15,8               No, return with error
         BR    R6
* 
STOWOK   DS    0H
         WTO   'STOW completed'
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
*
***********************************************************************
***********************************************************************
**                                                                   **
** Data structures, constants etc.                                   **
**                                                                   **
***********************************************************************
***********************************************************************
*
*--- EXECUTED INSTRUCTIONS & DATA STRUCTS
*
EX@MVC   MVC   MEMBER(0),2(R2)     EXTRACTS PARM FROM JCL STORAGE AREA
EX@CPYUD MVC   STOW@USR(0),BLDL@USR COPIES USER DATA 
*
PARMLIST DS    F
SAVEAREA DC    18F'0'              STANDARD MVS REGISTER SAVE AREA
*
* Area for debug WTO message
DOUBLE   DS    D                   8-byte doubleword work area for CVD
WTOPLIST DS    0F
         DC    AL2(WTOLEN)         Total length of WTO buffer (Data+4)
         DC    XL2'0000'           MCS flags
WTOMSG   DC    CL30'PROMPT TEXT'
WTOREG   DC    CL3'R3 '
         DC    C'='
WTOVAL   DC    C'XXXXXXXXXX'       Message text area (10 bytes for val)
WTOLEN   EQU   *-WTOPLIST
*
*--- BLDL STRUCTURE
*
BLDLLIST DS    0H                  ALIGN TO HALFWORD
         DC    H'1'                NUMBER OF ENTRIES IN THIS LIST
         DC    H'52'               LENGTH OF ENTRY (12 FIXED + 40 UD)
MEMBER   DC    CL8' '              TARGET MEMBER NAME STORE
BLDLTTRK DS    0XL4                The TTRK value from BLDL, for FIND
BLDL@TTR DC    XL3'00'             TRACK/RECORD POINTER (FROM BLDL)
BLDL@K   DC    XL1'00'             CONCATENATION NUMBER
BLDL@Z   DC    XL1'00'             SOURCE LIB... 0 = PRIVATE  
BLDL@FLG DC    XL1'00'             FLAGS BITMASK & USER DATA SIZE
BLDL@USR DC    XL40'00'            ISPF USER DATA STORAGE SLOT
*
*--- STOW STRUCTURE, BUILT WITH DATA FROM BLDL
*
STOWBUFF DS    0H
STOW@MEM DS    CL8            Member name
STOW@TTR DS    XL3            TTR of member 
STOW@UDL DS    XL1            This is user data len and entry type
STOW@USR DS    XL40           User data, normally 15 halfwords
*            
*--- BPAM DCB
*
PDSDCB   DCB   DDNAME=PDS,                                             X
               DSORG=PO,                                               X
               MACRF=(W)           OUTPUT OPERATION CAPABLE
         LTORG
         END   PDSUPDTE
//GO.PDS   DD   DISP=SHR,DSN=TEMP.TESTPROJ.CNTL
//