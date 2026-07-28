         TITLE 'MVSSTOW - Update member with ISPF stats'
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
MVSSTOW  CSECT
         JCCPROLG
         LR    R12,R15          establish module addressability
         USING MVSSTOW,R12
*
         ST    R1,PARMLIST      Save the callers parameter list addr
***********************************************************************
* STOW processing to update ISPF stats in directory                   *
*                                                                     *
* The parameters passed are --                                        *
*    char *ddname               Null terminated string                *
*    char *member               Null terminated string                *
*    void *userdata                                                   *
*    int   user_data_length_bytes                                     *
***********************************************************************
         DS    0H
*---------------------------------------------------------------------*
* Copy the DDNAME to a local area and open the DCB
*---------------------------------------------------------------------*
         L     R2,0(R1)         1st parm - ddname
         LA    R3,DDNAME        Destination
         LA    R4,L'DDNAME      Length of destination field
         BAL   R6,STRCOPY       Copy string and pad if required
*
         BAL   R6,DOOPEN        Open the DCB
         LTR   R15,R15
         BNZ   RETC@M1          RC -1 if error
*
*---------------------------------------------------------------------*
* Execute the BLDL to get TTRK for the member then FIND to set in DCB *
*---------------------------------------------------------------------*
*
         L     R1,PARMLIST
         L     R2,4(R1)         2nd parm - member name
         LA    R3,MEMBER        Destination
         LA    R4,L'MEMBER      Length of destination field
         BAL   R6,STRCOPY       Copy string and pad if required
*
         BLDL  PDSDCB,BLDLLIST  Look up existing member info
         LTR   R15,R15
         BZ    BLDLOK           If no error, continue
         BAL   R6,DOCLOSE       Got error, close DCB
         B     RETC@M1          RC -1
*
BLDLOK   DS    0H
         FIND  PDSDCB,BLDLTTRK,C   Set the TTRK in the DCB
         LTR   R15,R15
         BZ    FINDOK
*
         BAL   R6,DOCLOSE       Got error, close DCB
         B     RETC@M1          RC -1
*
FINDOK   DS    0H
*---------------------------------------------------------------------*
* Lastly, setup STOW area and issue STOW                              *
*---------------------------------------------------------------------*
         BAL   R6,SETSTOW       Setup the STOWBUFF area, copy UD
         STOW  PDSDCB,STOWBUFF,R
         LTR   R15,R15          Error?
         BZ    CLEANUP          No, goto cleanup
         BAL   R6,DOCLOSE       There was an error ... close DCB
         B     RETC@M1          RC -1
*
CLEANUP  DS    0H
         BAL   R6,DOCLOSE       Close the DCB
         B     RETC@0           RC = 0
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
* Open the DCB                                                        *
*---------------------------------------------------------------------*
DOOPEN   DS    0H
*
         LA    R3,PDSDCB           Address of DCB
         USING IHADCB,R3           DSECT for DCB
         MVC   DCBDDNAM,DDNAME     Copy the DDNAME into the DCB
         DROP  R3
*
         OPEN  (PDSDCB,(OUTPUT))   Open the PDS for directory update
         TM    PDSDCB+48,X'10'     Test if open was successful
         BNO   OPENFAIL            No -> Exit with open error
         SR    R15,R15
         BR    R6
*
OPENFAIL DS    0H
         LA    R15,8               Open failed error
         BR    R6
*
*---------------------------------------------------------------------*
* Setup the STOWBUFF area                                             *
*---------------------------------------------------------------------*
SETSTOW  DS    0H
         XC    STOW@USR,STOW@USR
         MVC   STOW@MEM,MEMBER     Copy member name
         MVC   STOW@TTR,BLDL@TTR   Copy the TTR
*
         L     R1,PARMLIST
         L     R2,8(R1)            Load address of user data 3rd parm
         L     R3,12(R1)           Load length of user data in bytes
*
         LR    R4,R3               Copy length in bytes
         SRL   R4,1                Convert length to halfwords
         ICM   R5,1,BLDL@FLG       Get flags
         N     R5,=XL4'000000E0'   Clear out the HW count
         OR    R4,R5               Put the flags back with new length
         STC   R4,STOW@UDL         Store the user data length and flags
*
         BCTR  R3,0                Decrement user data length
         EX    R3,EX@CPYUD         Copy user data
*
         BR    R6
*
EX@CPYUD MVC   STOW@USR(0),0(R2)   Copies user data ... EX'D
*
*---------------------------------------------------------------------*
* Close the DCB                                                       *
*---------------------------------------------------------------------*
DOCLOSE  DS    0H
         CLOSE (PDSDCB)
         SR    R15,R15
         BR    R6
*
*---------------------------------------------------------------------*
* Subroutines                                                         *
*---------------------------------------------------------------------*
         COPY  STRCOPY
***********************************************************************
***********************************************************************
* Static working storage and macros                                   *
***********************************************************************
***********************************************************************
*
PARMLIST DS    F           Address of callers parm list (a full word!)
DDNAME   DS    CL8
*
*--- BLDL structure to receive data
*
BLDLLIST DS    0H
         DC    H'1'                Number of entries in this list
         DC    H'52'               Length of entry (14 fixed + 40 UD)
MEMBER   DC    CL8' '              Target member name store
BLDLTTRK DS    0XL4                The TTRK value from BLDL, for FIND
BLDL@TTR DC    XL3'00'             Track/record pointer from BLDL
BLDL@K   DC    XL1'00'             Concatenation number
BLDL@Z   DC    XL1'00'             Source lib... 0 = Private
BLDL@FLG DC    XL1'00'             Flags bitmask & user data size (HW)
BLDL@USR DC    XL40'00'            ISPF user data storage slot
*
*--- STOW structure to set member info
*
STOWBUFF DS    0H
STOW@MEM DS    CL8            Member name
STOW@TTR DS    XL3            TTR of member
STOW@UDL DS    XL1            This is user data len and flags
STOW@USR DS    XL40           User data, normally 15 halfwords
*---------------------------------------------------------------------*
* DCB                                                                 *
*---------------------------------------------------------------------*
* The DDNAME in the DCB is set before the open
PDSDCB   DCB   DSORG=PO,MACRF=W,DDNAME=XXXXXXXX
*
         LTORG
         DCBD  DSORG=PO,DEVD=DA
         END   MVSSTOW