         TITLE 'MVSDSCB - Utility for getting DSCB info for datasets'
*
***********************************************************************
*  Setup                                                              *
***********************************************************************
         PRINT NOGEN
         YREGS
         PRINT GEN
*
***********************************************************************
* Our working storage stack frame layout DSECT                        *
***********************************************************************
         DSECT
SFSTART  DS    0F
SFJCC    DS    3F               Stack frame mgmt, as per JCC stds
*                               Don't let anything touch the above!
SFSAVE   DS    18F              A standard save area
SFPRMLST DS    F
****
**** Add working storage fields here to be part of the stack
**** frame allocated by JCC
****
SFEND    DS    0F
SFSIZE   EQU   SFEND-SFSTART
*
***********************************************************************
*  Entry and stack frame linkage                                      *
***********************************************************************
MVSDSCB  CSECT
         JCCPROLG FRAME=SFSIZE
         LR    R12,R15          establish module addressability
         USING MVSDSCB,R12
         USING SFSTART,R13      R13 is set by JCCPROLG
*
         ST    R1,SFPRMLST      Save the callers parameter list addr
***********************************************************************
* MVSDSCB processing                                                  *
*                                                                     *
* Get the DSCB info for a list of cataloged datasets                  *
* The parameters passed are --                                        *
*    uint8_t   request_type                                           *
*                 1 - Get DSCB                                        *
*    uint8_t   options                                                *
*                 0x01 - ???                                          *
*    char     *dsnlist     An array of pointers to null terminated    *
*                          strings. Each string contains a dataset    *
*                          name for which the DSCB information is to  *
*                          returned.                                  *
*                          The last dataset pointer should be followed*
*                          by a null pointer to indicate end of list. *
*    void     *data        An area of memory, large enough to hold    *
*                          the return data for each dataset in the    *
*                          dsnlist. Each dataset will have an         *
*                          a set of fields as below.                  *
*                                                                     *
*    Each dataset will have a set of fields as below                  *
*                                                                     *
*    DS    CL1     - 0 = Dataset found, 8 = Not found                 *
*    DS    CL44    - Dataset name                                     *
*    DS    CL6     - Volume serial                                    *
*    DS    CL1     - Number of extents                                *
*    DS    F       - The number of tracks in the current allocation   *
*                    from all extents                                 *
*    DS    CL3     - Dataset creation date                            *
*    DS    CL3     - Dataset expiration date                          *
*    DS    CL3     - Last referenced date                             *
*    DS    CL2     - DSORG field value from VTOC DSCB 1               *
*    DS    CL1     - RECFM field value from VTOC DSCB 1               *
*    DS    CL2     - Block size                                       *
*    DS    CL2     - Logical record length                            *
*                                                                     *
*    Second and subsequent datasets repeat the above fields           *
*                                                                     *
* Returns --                                                          *
*     0     - All dataset info retrieved                              *
*     4     - One or more of the listed datasets could not have its   *
*             information retrieved.                                  *
*     8     - Parameter list error                                    *
*    16     - Other error                                             *
*                                                                     *
* Note that when JCC passes an unsigned char value as a parameter it  *
* is in the parm list as a 32 bit value. Thus, loading it as a 32 bit *
* value is correct. Loading a character (IC/ICM) is wrong as it would *
* load the high order byte of the parameter, which would be zero.     *
***********************************************************************
         L     R1,SFPRMLST
         L     R2,0(R1)      Load request type. Even though it
*                            is defined as an unsigned char, JCC
*                            will pass the value as a 32 bit value in
*                            the parm list.
         C     R2,=F'1'
         BE    DO@GDSCB      Request is get DSCB info
         B     RETC@16       Request type is invalid
*
***********************************************************************
***********************************************************************
* DYNAMIC ALLOCATION REQUEST                                          *
***********************************************************************
***********************************************************************
DO@GDSCB DS    0H
*---------------------------------------------------------------------*
* Get DSCB info for the list of datasets units                        *
*---------------------------------------------------------------------*
         L     R1,SFPRMLST
*
EN@GDSCB DS    0H
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
RETC@4   DS    0H
         L     R15,=F'4'
         B     RETURN
*
RETC@8   DS    0H
         L     R15,=F'8'
         B     RETURN
*
RETC@16  DS    0H
         L     R15,=F'16'
         B     RETURN
*
RETC@0   DS    0H
         SR    R15,R15               R15=0 and fall through
*
RETURN   JCCRETRN                    Assumes R15 has return value
*
***********************************************************************
***********************************************************************
* Subroutines                                                         *
***********************************************************************
***********************************************************************
*
*
*---------------------------------------------------------------------*
* Include utility sub routines                                        *
*---------------------------------------------------------------------*
         COPY  STRCOPY
*
*---------------------------------------------------------------------*
* Literals and other constants                                        *
*---------------------------------------------------------------------*
         LTORG
*
         PRINT NOGEN
         END
