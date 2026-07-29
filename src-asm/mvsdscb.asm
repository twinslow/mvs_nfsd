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
SFRC     DS    F                Overall return code being built
SFDSNPTR DS    F                -> current entry in the dsn list
SFOUTPTR DS    F                -> current entry in the output area
SFTRKCYL DS    F                Tracks per cylinder of current volume
SFTRACKS DS    F                Accumulated track count for this dsn
SFEXTCNT DS    F                Extents still to be accounted for
SFSVR6   DS    F                Saved R6 across a nested BAL
SFCVOL   DS    CL6              Volume that SFTRKCYL was derived from
         DS    CL2              (pad back to a fullword boundary)
SFGEO    DS    CL8              Cached geometry, laid out exactly as
*                               OUTGEO so it is one MVC to the caller
SFEND    DS    0F
SFSIZE   EQU   SFEND-SFSTART
*
***********************************************************************
* Layout of ONE returned dataset entry, 96 bytes.  This must match    *
* mvs_dscb_info_t in src/asmutils.h byte for byte.                    *
*                                                                     *
* The three fullwords land on offsets 52, 88 and 92, all of which are *
* already aligned, so a C struct of the same fields needs no padding  *
* and no packing pragma.  asmutils.h carries a compile time assertion *
* that sizeof() really is 96, so a repacked struct fails the build    *
* rather than silently reading the wrong fields.                      *
***********************************************************************
DSCBOUT  DSECT
OUTSTAT  DS    CL1              0=found, 8=not found, 12=multi-volume
OUTDSN   DS    CL44             Dataset name, blank padded
OUTVOL   DS    CL6              Volume serial
OUTNEXT  DS    CL1              Number of extents (DS1NOEPV)
OUTTRKS  DS    F                Tracks in the current allocation
OUTCRDT  DS    CL3              Creation date
OUTEXDT  DS    CL3              Expiration date
OUTRFDT  DS    CL3              Last referenced date
OUTDSORG DS    CL2              DSORG from the format 1 DSCB
OUTRECFM DS    CL1              RECFM from the format 1 DSCB
OUTBLKSZ DS    CL2              Block size
OUTLRECL DS    CL2              Logical record length
*
* Device characteristics.  The first is the UCB device type from the
* catalog entry; the rest come from the volume's format 4 DSCB and are
* what the track capacity calculation needs.  OUTGEO covers the seven
* format 4 values as one block so they can be moved in a single MVC.
*
OUTDEVT  DS    CL4              UCB device type (from LOCATE)
OUTGEO   DS    0CL8             The format 4 geometry, moved as a unit
OUTTRKLN DS    CL2              DS4DEVTK - bytes per track
OUTTRKCY DS    CL2              DS4DSTRK - tracks per cylinder
OUTDEVI  DS    CL1              DS4DEVI  - block overhead, not last
OUTDEVL  DS    CL1              DS4DEVL  - block overhead, last block
OUTDEVK  DS    CL1              DS4DEVK  - extra overhead when keyed
OUTDEVFG DS    CL1              DS4DEVFG - device flags
*
* Secondary allocation.  This is what the dataset is ALLOWED to extend
* by, which is not visible from the extents it currently holds.
*
OUTSCAL1 DS    CL1              DS1SCAL1 - units and flags
         DS    CL3              (pad to a fullword boundary)
OUTSCQTY DS    F                DS1SCAL3 - quantity, in those units
OUTSCTRK DS    F                The same in TRACKS, or 0 if it could
*                               not be converted (see below)
OUTLEN   EQU   *-DSCBOUT        Length of one entry (96 bytes)
*
***********************************************************************
* Format 1 DSCB (the dataset's VTOC entry).                           *
*                                                                     *
* Defined here rather than via IECSDSL1 so the module does not depend *
* on which variant of the system mapping macro is installed -- this   *
* layout has not changed since OS/360.                                *
*                                                                     *
* IMPORTANT: OBTAIN with CAMLST SEARCH returns only the 96-byte DATA  *
* portion, i.e. the DSCB from DS1FMTID onwards.  The code therefore   *
* bases this DSECT at (work area - 44) so the field names resolve     *
* correctly, which means DS1DSNAM lies BEFORE the work area and must  *
* never be referenced.                                                *
***********************************************************************
DSCB1    DSECT
DS1DSNAM DS    CL44             *** NOT PRESENT for CAMLST SEARCH ***
DS1FMTID DS    CL1              Format identifier, C'1'
DS1DSSN  DS    CL6              Volume serial
DS1VOLSQ DS    XL2              Volume sequence number
DS1CREDT DS    XL3              Creation date  (YDD, packed as YYDDD)
DS1EXPDT DS    XL3              Expiration date
DS1NOEPV DS    XL1              Number of extents on this volume
DS1NOBDB DS    XL1              Number of bytes in last dir block
DS1FLAG1 DS    XL1              Flags
DS1SYSCD DS    CL13             System code
DS1REFD  DS    XL3              Last referenced date (see note below)
DS1SMSFG DS    XL1              Reserved on MVS 3.8
DS1SCXTF DS    XL1              Reserved on MVS 3.8
DS1SCXTV DS    XL2              Reserved on MVS 3.8
DS1DSORG DS    XL2              Dataset organisation
DS1RECFM DS    XL1              Record format
DS1OPTCD DS    XL1              Option codes
DS1BLKL  DS    XL2              Block size
DS1LRECL DS    XL2              Logical record length
DS1KEYL  DS    XL1              Key length
DS1RKP   DS    XL2              Relative key position
DS1DSIND DS    XL1              Dataset indicators
DS1SCAL1 DS    XL1              Secondary allocation flags
DS1SCAL3 DS    XL3              Secondary allocation quantity
DS1LSTAR DS    XL3              Last used TTR
DS1TRBAL DS    XL2              Bytes remaining on last track
         DS    XL2              Reserved
DS1EXT1  DS    XL10             Extent descriptor 1
DS1EXT2  DS    XL10             Extent descriptor 2
DS1EXT3  DS    XL10             Extent descriptor 3
DS1PTRDS DS    XL5              CCHHR of the format 3 DSCB, or zero
*
* An extent descriptor is 10 bytes:
*      +0  XL1   extent type
*      +1  XL1   extent sequence number
*      +2  XL2   lower limit CC
*      +4  XL2   lower limit HH
*      +6  XL2   upper limit CC
*      +8  XL2   upper limit HH
EXTLOCC  EQU   2
EXTLOHH  EQU   4
EXTUPCC  EQU   6
EXTUPHH  EQU   8
EXTLEN   EQU   10
*
***********************************************************************
* Format 4 DSCB (the VTOC's own entry), read for the device geometry. *
* Only the fields we need are named.  OBTAIN CAMLST SEARCH again      *
* returns the data portion only, so these offsets are relative to the *
* WORK AREA, not to the start of the DSCB -- subtract 44 from the     *
* DSCB offset to get the work area offset.                            *
*                                                                     *
* The format 4 DSCB is found by SEARCHing for its KEY, which is 44    *
* bytes of X'04' -- see F4DSN below.                                  *
***********************************************************************
F4DSCYL  EQU   18               DSCB+62 cylinders per volume  (HW)
F4DSTRK  EQU   20               DSCB+64 tracks per cylinder   (HW)
F4DEVTK  EQU   22               DSCB+66 bytes per track       (HW)
F4DEVI   EQU   24               DSCB+68 overhead, not last block
F4DEVL   EQU   25               DSCB+69 overhead, last block
F4DEVK   EQU   26               DSCB+70 extra overhead if keyed
F4DEVFG  EQU   27               DSCB+71 device flags
*
***********************************************************************
* Format 3 DSCB (extents 4 to 16).  Read with CAMLST SEEK, which      *
* returns the FULL 140 bytes (44-byte key + 96-byte data), so these   *
* offsets are relative to the start of the work area.                 *
*                                                                     *
*      +0    XL4    DS3KEYID, four bytes of X'03'                     *
*      +4    4 extent descriptors  (40 bytes)                         *
*      +44   XL1    DS3FMTID, C'3'                                    *
*      +45   9 extent descriptors  (90 bytes)                         *
*      +135  XL5    DS3PTRDS, chain to a further format 3 (unused --  *
*                   3 + 4 + 9 = 16 extents is the single-volume max)  *
***********************************************************************
F3EXT1   EQU   4                First block of extents in the key
F3NEXT1  EQU   4                  ... four of them
F3EXT2   EQU   45               Second block of extents in the data
F3NEXT2  EQU   9                  ... nine of them
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
*                 Reserved, currently ignored                         *
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
*    Each dataset entry is 96 bytes, laid out as below.  This must    *
*    match the DSCBOUT DSECT further down AND mvs_dscb_info_t in      *
*    src/asmutils.h byte for byte.                                    *
*                                                                     *
*    off                                                              *
*      0  DS  CL1   0 = Dataset found, 8 = Not found,                 *
*                   12 = Multi-volume dataset, which is not supported *
*      1  DS  CL44  Dataset name, blank padded                        *
*     45  DS  CL6   Volume serial                                     *
*     51  DS  CL1   Number of extents                                 *
*     52  DS  F     The number of tracks in the current allocation    *
*                   from all extents                                  *
*     56  DS  CL3   Dataset creation date                             *
*     59  DS  CL3   Dataset expiration date                           *
*     62  DS  CL3   Last referenced date                              *
*     65  DS  CL2   DSORG field value from VTOC DSCB 1                *
*     67  DS  CL1   RECFM field value from VTOC DSCB 1                *
*     68  DS  CL2   Block size                                        *
*     70  DS  CL2   Logical record length                             *
*                                                                     *
*    Device characteristics.  The first comes from the catalog, the   *
*    rest from the volume's format 4 DSCB.                            *
*                                                                     *
*     72  DS  CL4   UCB device type.  Low byte identifies the model:  *
*                   X'0F'=3390, X'0E'=3380, X'0B'=3350                *
*     76  DS  CL2   Bytes per track (DS4DEVTK).  This is the PHYSICAL *
*                   track length, not the usable data capacity, and   *
*                   also identifies the model: 58786=3390,            *
*                   47968=3380, 19254=3350                            *
*     78  DS  CL2   Tracks per cylinder (DS4DSTRK)                    *
*     80  DS  CL1   DS4DEVI - block overhead, not last block          *
*     81  DS  CL1   DS4DEVL - block overhead, last block              *
*     82  DS  CL1   DS4DEVK - extra overhead when the record is keyed *
*     83  DS  CL1   DS4DEVFG - device flags                           *
*                                                                     *
*    NOTE on the four DS4DEV* values: they are returned for           *
*    completeness but are NOT a usable basis for track capacity       *
*    arithmetic.  MVS 3.8 predates the 3380 and 3390, so on those     *
*    volumes they are all zero, and even where they are populated     *
*    they do not mean what they appear to -- a 3350 reports           *
*    DS4DEVI = 11 against a true overhead of 185 bytes per record.    *
*    Use mvs_blocks_per_track() in src/mvsutl.c instead, which is     *
*    exact for 3390, 3380 and 3350.                                   *
*                                                                     *
*    Secondary allocation -- what the dataset may EXTEND by, which    *
*    the extents it currently holds cannot tell you.                  *
*                                                                     *
*     84  DS  CL1   DS1SCAL1.  Top two bits give the units of the     *
*                   quantity at offset 88:                            *
*                       X'C0' cylinders                               *
*                       X'80' tracks                                  *
*                       X'40' average block length                    *
*                       X'00' absolute track address                  *
*     85  DS  CL3   Unused, aligns the fullwords that follow          *
*     88  DS  F     Secondary quantity, in the units above            *
*     92  DS  F     The same quantity converted to TRACKS, or 0 when  *
*                   it could not be converted here.  Cylinders and    *
*                   tracks are converted; average block length is     *
*                   NOT, because that needs the device's track        *
*                   capacity -- the caller finishes that case with    *
*                   mvs_blocks_per_track() and the block size.        *
*                                                                     *
*    Second and subsequent datasets repeat the above fields           *
*                                                                     *
* Returns --                                                          *
*     0     - All dataset info retrieved                              *
*     4     - One or more of the listed datasets could not have its   *
*             information retrieved.  Check each entry's status.      *
*             Also returned when a volume's geometry could not be     *
*             read, in which case that entry's track count is zero    *
*             but every other field is still valid.                   *
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
* GET DSCB INFORMATION REQUEST                                        *
***********************************************************************
***********************************************************************
DO@GDSCB DS    0H
*---------------------------------------------------------------------*
* Get DSCB info for the list of datasets                              *
*---------------------------------------------------------------------*
         L     R1,SFPRMLST
*
         L     R2,8(R1)         3rd parm - the dsn pointer list
         LTR   R2,R2
         BZ    RETC@8           A null list is a parameter error
         ST    R2,SFDSNPTR
*
         L     R2,12(R1)        4th parm - the output data area
         LTR   R2,R2
         BZ    RETC@8           No output area is a parameter error
         ST    R2,SFOUTPTR
*
         SR    R2,R2
         ST    R2,SFRC          Overall return code starts at zero
         XC    SFCVOL,SFCVOL    No cached device geometry yet
*
*---------------------------------------------------------------------*
* Loop over the dataset names until the null terminator pointer       *
*---------------------------------------------------------------------*
GD@LOOP  DS    0H
         L     R7,SFDSNPTR
         L     R2,0(,R7)        Address of the next dataset name
         LTR   R2,R2
         BZ    EN@GDSCB         Null pointer - end of the list
*
         L     R9,SFOUTPTR
         USING DSCBOUT,R9
*
*        Start this entry clean: zero it, then blank the text fields
         XC    OUTSTAT(OUTLEN),OUTSTAT
         MVC   OUTDSN,BLANKS
         MVC   OUTVOL,BLANKS
*
*        Copy the caller's C string into a 44 byte blank padded field
         LA    R3,DSNAME
         LA    R4,L'DSNAME
         BAL   R6,STRCOPY
         MVC   OUTDSN,DSNAME    Echo back what we actually looked up
*
*---------------------------------------------------------------------*
* Find the dataset in the catalog to get its volume serial            *
*---------------------------------------------------------------------*
         LOCATE LOCCAML
         LTR   R15,R15
         L     R9,SFOUTPTR   Reinstate the output pointer after the SVC
         BNZ   GD@NOTFD         Not cataloged
*
         LH    R2,LOCWORK       Number of volumes in the catalog entry
         LTR   R2,R2
         BNP   GD@NOTFD         No volumes - treat as not found
         C     R2,=F'1'
         BH    GD@MULTI         Multi-volume is not supported
*
*        The volume list starts at +2, each entry being
*        4 bytes of device type, 6 bytes of volser, 2 bytes of seq no.
         MVC   VOLSER,LOCWORK+6
         MVC   OUTVOL,VOLSER
*        The 4 byte UCB device type sits ahead of the volser in the
*        same catalog entry.  Track length identifies the model far
*        more reliably than this code does, but both are returned.
         MVC   OUTDEVT,LOCWORK+2
*
*---------------------------------------------------------------------*
* Device geometry, needed to turn CCHH extents into a track count.    *
* Cached, because a list of datasets is very likely to be all on the  *
* same volume and each lookup is a real I/O to the VTOC.              *
*---------------------------------------------------------------------*
         CLC   VOLSER,SFCVOL
         BE    GD@HAVCY         Already have it for this volume
         BAL   R6,GETTRKCY
*                               A failure here is not fatal: the other
*                               DSCB fields are still valid, so report
*                               them with a zero track count and let
*                               the overall RC say it was imperfect.
*
GD@HAVCY DS    0H
         MVC   OUTGEO,SFGEO     Geometry, cached or freshly read
*---------------------------------------------------------------------*
* Read the format 1 DSCB from the volume's VTOC                       *
*---------------------------------------------------------------------*
         OBTAIN OBTCAML
         LTR   R15,R15
         L     R9,SFOUTPTR   Reinstate the output pointer after the SVC
         BNZ   GD@NOTFD         No DSCB - cataloged but not on the vol
*
*        CAMLST SEARCH returns the DATA portion only, so base the
*        DSECT 44 bytes low.  Never reference DS1DSNAM through this.
         LA    R3,DSCBWORK
         SH    R3,=H'44'
         USING DSCB1,R3
*
         MVC   OUTNEXT,DS1NOEPV
         MVC   OUTCRDT,DS1CREDT
         MVC   OUTEXDT,DS1EXPDT
         MVC   OUTRFDT,DS1REFD
         MVC   OUTDSORG,DS1DSORG
         MVC   OUTRECFM,DS1RECFM
         MVC   OUTBLKSZ,DS1BLKL
         MVC   OUTLRECL,DS1LRECL
*
*---------------------------------------------------------------------*
* Secondary allocation quantity, converted to tracks where possible   *
*                                                                     *
* DS1SCAL3 is the quantity and the top two bits of DS1SCAL1 say what  *
* it counts:                                                          *
*      X'80'  cylinders   -> multiply by tracks per cylinder          *
*      X'40'  tracks      -> use as is                                *
*      X'C0'  average block length -> CANNOT be converted here; it    *
*             depends on how many blocks fit on a track, which needs  *
*             the device capacity tables.  OUTSCTRK is left zero and  *
*             the caller converts using OUTSCQTY and the blocksize.   *
*                                                                     *
* The raw quantity and the flag byte are always returned, so nothing  *
* is lost when the conversion cannot be done.                         *
*---------------------------------------------------------------------*
         MVC   OUTSCAL1,DS1SCAL1     Units and flags, unchanged
*
         SR    R4,R4
         ICM   R4,B'0111',DS1SCAL3   3 byte quantity into R4
         ST    R4,OUTSCQTY
*
         SR    R5,R5
         IC    R5,DS1SCAL1
         N     R5,=XL4'000000C0'     Isolate just the units bits
*
         C     R5,=XL4'00000080'     Tracks already?
         BE    GD@SCSET              Yes - R4 is the answer
         C     R5,=XL4'000000C0'     Cylinders?
         BNE   GD@SCNON              No - avg block, or absolute track
*
*        Cylinders: multiply by the tracks per cylinder of this volume.
         L     R5,SFTRKCYL
         LTR   R5,R5
         BNP   GD@SCNON              No geometry, so no conversion
         LR    R5,R4                 Quantity into the odd register
         SR    R4,R4
         M     R4,SFTRKCYL           R4/R5 = quantity * tracks per cyl
         LR    R4,R5
         B     GD@SCSET
*
GD@SCNON DS    0H
         SR    R4,R4                 Leave it zero: not convertible
*
GD@SCSET DS    0H
         ST    R4,OUTSCTRK
*
*---------------------------------------------------------------------*
* Total the tracks across every extent                                *
*---------------------------------------------------------------------*
         SR    R4,R4
         ST    R4,SFTRACKS      Running total starts at zero
         IC    R4,DS1NOEPV      Extents to account for
         ST    R4,SFEXTCNT
*
         L     R4,SFTRKCYL      No geometry means no track count
         LTR   R4,R4
         BNP   GD@TRKDN
*
*        The first three extents live in the format 1 DSCB itself
         LA    R2,DS1EXT1
         LA    R10,3
         BAL   R6,SUMEXT
*
*        Any beyond that are in a format 3 DSCB, chained from DS1PTRDS.
*        3 + 4 + 9 = 16 extents, which is the single-volume maximum, so
*        one format 3 is always enough.
         L     R4,SFEXTCNT
         LTR   R4,R4
         BNP   GD@TRKDN         All extents already accounted for
*
         MVC   F3CCHHR,DS1PTRDS
         OC    F3CCHHR,F3CCHHR
         BZ    GD@TRKDN         No chain pointer - report what we have
*
         OBTAIN F3CAML
         LTR   R15,R15
         BNZ   GD@TRKDN         Unreadable - report what we have
*
         LA    R2,F3WORK+F3EXT1
         LA    R10,F3NEXT1
         BAL   R6,SUMEXT
*
         LA    R2,F3WORK+F3EXT2
         LA    R10,F3NEXT2
         BAL   R6,SUMEXT
*
GD@TRKDN DS    0H
         L     R4,SFTRACKS
         ST    R4,OUTTRKS
         DROP  R3
*
         MVI   OUTSTAT,X'00'    Found
         B     GD@NEXT
*
*---------------------------------------------------------------------*
* Per dataset failures.  These are NOT fatal: the remaining datasets  *
* are still processed and the overall return code becomes 4.          *
*---------------------------------------------------------------------*
GD@NOTFD DS    0H
         MVI   OUTSTAT,X'08'    Not found
         BAL   R6,SETRC4
         B     GD@NEXT
*
GD@MULTI DS    0H
         MVI   OUTSTAT,X'0C'    Multi-volume, which we do not support
         BAL   R6,SETRC4
         B     GD@NEXT
*
*---------------------------------------------------------------------*
* Step to the next dataset name and the next output entry             *
*---------------------------------------------------------------------*
GD@NEXT  DS    0H
         DROP  R9
         L     R2,SFDSNPTR
         LA    R2,4(,R2)        Next pointer in the caller's list
         ST    R2,SFDSNPTR
*
         L     R2,SFOUTPTR
         LA    R2,OUTLEN(,R2)   Next entry in the caller's data area
         ST    R2,SFOUTPTR
         B     GD@LOOP
*
EN@GDSCB DS    0H
         L     R15,SFRC         0, or 4 if any dataset failed
         LTR   R15,R15
         BZ    RETC@0
         B     RETC@4
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
*---------------------------------------------------------------------*
* SETRC4 - remember that at least one dataset could not be reported   *
*          Entry/exit: R6 return address                              *
*---------------------------------------------------------------------*
SETRC4   DS    0H
         L     R4,=F'4'
         ST    R4,SFRC
         BR    R6
*
*---------------------------------------------------------------------*
* GETTRKCY - get tracks per cylinder for the volume named in VOLSER   *
*                                                                     *
* Reads the volume's own format 4 DSCB, which carries the device      *
* geometry.  This avoids having to map device type codes to track     *
* counts, which would need a table that goes stale with every new     *
* device type.                                                        *
*                                                                     *
* On exit  SFTRKCYL holds the tracks per cylinder, and SFCVOL the     *
*          volume it came from, so the next dataset on the same       *
*          volume skips the I/O.  On failure SFTRKCYL is zero, the    *
*          cache is invalidated and the overall RC is set to 4.       *
*          R6 is the return address.                                  *
*---------------------------------------------------------------------*
GETTRKCY DS    0H
         ST    R6,SFSVR6       Saved here, NOT in SFSAVE: that is the
*                              standard 18 word save area addressed by
*                              R13, which the SVC below may store into.
*
         SR    R4,R4
         ST    R4,SFTRKCYL      Assume failure
         XC    SFCVOL,SFCVOL
         XC    SFGEO,SFGEO      Never report stale geometry
*
         OBTAIN F4CAML
         LTR   R15,R15
         BNZ   GTCY@ERR
*
         LH    R4,F4WORK+F4DSTRK    Tracks per cylinder
         LTR   R4,R4
         BNP   GTCY@ERR             Zero would give a nonsense answer
*
         ST    R4,SFTRKCYL
*
*        Keep the device constants too.  They are what a caller needs
*        to work out how many blocks of a given size fit on a track,
*        and they are per volume, so they cache alongside SFTRKCYL.
         MVC   SFGEO+0(2),F4WORK+F4DEVTK    Bytes per track
         MVC   SFGEO+2(2),F4WORK+F4DSTRK    Tracks per cylinder
         MVC   SFGEO+4(1),F4WORK+F4DEVI     Overhead, not last block
         MVC   SFGEO+5(1),F4WORK+F4DEVL     Overhead, last block
         MVC   SFGEO+6(1),F4WORK+F4DEVK     Extra overhead if keyed
         MVC   SFGEO+7(1),F4WORK+F4DEVFG    Device flags
*
         MVC   SFCVOL,VOLSER        Cache is now valid for this volume
         L     R6,SFSVR6
         BR    R6
*
GTCY@ERR DS    0H
         BAL   R6,SETRC4            Say the results are incomplete
         L     R6,SFSVR6
         BR    R6
*
*---------------------------------------------------------------------*
* SUMEXT - add the tracks described by a block of extent descriptors  *
*                                                                     *
* On entry                                                            *
*    R2  - address of the first extent descriptor in the block        *
*    R10 - number of descriptor slots in this block                   *
*    R6  - return address                                             *
*    SFEXTCNT - extents still to be accounted for                     *
*    SFTRKCYL - tracks per cylinder for the volume                    *
* On exit                                                             *
*    SFTRACKS increased, SFEXTCNT reduced by the number processed     *
*                                                                     *
* Tracks in one extent =                                              *
*    (upperCC - lowerCC) * tracks-per-cyl + (upperHH - lowerHH) + 1   *
*                                                                     *
* DS1NOEPV says exactly how many descriptors are live, so the slots   *
* are simply taken in order and no extent type checking is needed.    *
*---------------------------------------------------------------------*
SUMEXT   DS    0H
SUMEXTLP DS    0H
         L     R4,SFEXTCNT
         LTR   R4,R4
         BNP   SUMEXTX          Nothing left to account for
         LTR   R10,R10
         BNP   SUMEXTX          No slots left in this block
*
         LH    R5,EXTUPCC(,R2)  Upper limit cylinder
         LH    R4,EXTLOCC(,R2)  Lower limit cylinder
         SR    R5,R4            Cylinders spanned
         M     R4,SFTRKCYL      R4/R5 = cylinders * tracks per cylinder
*
         LH    R8,EXTUPHH(,R2)  Upper limit head
         LH    R11,EXTLOHH(,R2) Lower limit head
         SR    R8,R11           Heads spanned
         AR    R5,R8
         LA    R5,1(,R5)        Both limits are inclusive
*
         L     R4,SFTRACKS
         AR    R4,R5
         ST    R4,SFTRACKS
*
         L     R4,SFEXTCNT
         BCTR  R4,0
         ST    R4,SFEXTCNT
         LA    R2,EXTLEN(,R2)   Next descriptor
         BCTR  R10,0            One fewer slot in this block
         B     SUMEXTLP
*
SUMEXTX  DS    0H
         BR    R6
*
*---------------------------------------------------------------------*
* Include utility sub routines                                        *
*---------------------------------------------------------------------*
         COPY  STRCOPY
*
***********************************************************************
***********************************************************************
* Static working storage                                              *
*                                                                     *
* The CAMLST macro assembles the addresses of its operands, so the    *
* fields it points at have to be static rather than stack frame       *
* items.  Keeping the work areas alongside them means the CAMLSTs     *
* never need patching at run time.  dino-nfs is single threaded, so   *
* static work areas are safe here.                                    *
***********************************************************************
*
DSNAME   DS    CL44             Dataset name for LOCATE / OBTAIN
VOLSER   DS    CL6              Volume serial from the catalog
F3CCHHR  DS    XL5              CCHHR of the format 3 DSCB, for SEEK
BLANKS   DC    CL44' '
*
*
* The format 4 DSCB is found by SEARCHing the VTOC for its KEY, which
* is 44 bytes of X'04' -- NOT the text 'FORMAT4.DSCB', which is only
* what ISPF and LISTVTOC call it when displaying the VTOC.  OBTAIN
* SEARCH does a literal key comparison, so the text form never matches
* and the geometry lookup fails, leaving a zero track count.
*
F4DSN    DC    44X'04'          Key of the format 4 (VTOC) DSCB
*
*---------------------------------------------------------------------*
* CAMLSTs.  All operands are the static fields above, so these are    *
* built once at assembly time and never modified.                     *
*---------------------------------------------------------------------*
LOCCAML  CAMLST NAME,DSNAME,,LOCWORK
OBTCAML  CAMLST SEARCH,DSNAME,VOLSER,DSCBWORK
F4CAML   CAMLST SEARCH,F4DSN,VOLSER,F4WORK
F3CAML   CAMLST SEEK,F3CCHHR,VOLSER,F3WORK
*
*---------------------------------------------------------------------*
* Work areas.  All must be on a doubleword boundary.                  *
*   LOCWORK   265 bytes - catalog entry: a halfword volume count      *
*                         followed by 12 byte entries of device type, *
*                         volume serial and sequence number.          *
*   DSCBWORK  140 bytes - CAMLST SEARCH returns the 96 byte data      *
*                         portion plus the 5 byte CCHHR of the DSCB.  *
*   F4WORK    140 bytes - as above, for the format 4 DSCB.            *
*   F3WORK    140 bytes - CAMLST SEEK returns the FULL DSCB, key      *
*                         included, so offsets here start at zero.    *
*---------------------------------------------------------------------*
         DS    0D
LOCWORK  DS    XL265
         DS    0D
DSCBWORK DS    XL140
         DS    0D
F4WORK   DS    XL140
         DS    0D
F3WORK   DS    XL140
*
*---------------------------------------------------------------------*
* Literals and other constants                                        *
*---------------------------------------------------------------------*
         LTORG
*
         PRINT NOGEN
         END
