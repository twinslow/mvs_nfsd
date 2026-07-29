#ifndef ASMUTILS_H
#define ASMUTILS_H

#include "types.h"

#define getcib          GETCIB
#define mvs_dynalloc    MVSDALC
#define mvs_stow        MVSSTOW
#define mvs_enq         MVSENQ
#define mvs_dscb        MVSDSCB

#define MVS_DYNALLOC_REQ_ALLOC      1
#define MVS_DYNALLOC_REQ_UNALLOC    2
#define MVS_DYNALLOC_OPT_FREECLOSE  0x01

#define MVS_ENQ_REQ_ENQ     1
#define MVS_ENQ_REQ_DEQ     2
#define MVS_ENQ_REQ_ENQTEST 3

#define MVS_ENQ_OPT_EXC     0x01
#define MVS_ENQ_OPT_SHR     0x00

/* -------------------------------------------------------------------- */
/* mvs_dscb: dataset information straight from the catalog and the VTOC  */
/* -------------------------------------------------------------------- */

#define MVS_DSCB_REQ_INFO   1     /* the only request type so far        */

/* Per-dataset status, in mvs_dscb_info_t.status */
#define MVS_DSCB_ST_OK        0
#define MVS_DSCB_ST_NOTFOUND  8   /* not cataloged, or no DSCB on the vol */
#define MVS_DSCB_ST_MULTIVOL 12   /* multi-volume, which is not supported */

/*
 * One entry per dataset.  The layout MUST match the DSCBOUT DSECT in
 * src-asm/mvsdscb.asm byte for byte.
 *
 * 'tracks' lands on offset 52, which is already fullword aligned, so the
 * compiler inserts no padding and the total is exactly 96 bytes.  The
 * size assertion below enforces that rather than trusting it.
 *
 * The DSCB fields that are binary halfwords (dsorg, blksize, lrecl) are
 * kept as raw byte pairs, NOT uint16_t: dsorg sits at an odd offset, so
 * declaring it as a halfword would force the compiler to pad and shift
 * everything after it.  Use MVS_DSCB_U16() to read them.
 *
 * The three date fields are the DSCB's own 3-byte form: a two digit year
 * then the day of the year as a binary halfword.  MVS_DSCB_YEAR and
 * MVS_DSCB_DAY unpack them (see the note on the century window there).
 * All zeros means "not recorded"; ref_date IS maintained on MVS 3.8,
 * contrary to what older documentation implies.
 */
typedef struct {
    uint8_t  status;            /* offset  0 - MVS_DSCB_ST_*            */
    char     dsname[44];        /* offset  1 - blank padded, not NUL    */
    char     volser[6];         /* offset 45 - blank padded, not NUL    */
    uint8_t  nextents;          /* offset 51 - extents on the volume    */
    uint32_t tracks;            /* offset 52 - tracks over all extents  */
    uint8_t  create_date[3];    /* offset 56                            */
    uint8_t  expire_date[3];    /* offset 59                            */
    uint8_t  ref_date[3];       /* offset 62 - may be zero on MVS 3.8   */
    uint8_t  dsorg[2];          /* offset 65 - see MVS_DSCB_U16()       */
    uint8_t  recfm;             /* offset 67                            */
    uint8_t  blksize[2];        /* offset 68 - see MVS_DSCB_U16()       */
    uint8_t  lrecl[2];          /* offset 70 - see MVS_DSCB_U16()       */
    /*
     * Device characteristics.  devtype is the UCB device type from the
     * catalog; the rest come from the volume's format 4 DSCB.  All zero
     * means the geometry could not be read (the call then returns 4).
     *
     * To identify the model, prefer trklen over devtype: bytes per track
     * is unique per device family and needs no lookup table that can go
     * stale.  See MVS_DSCB_MODEL().
     */
    uint8_t  devtype[4];        /* offset 72 - UCB device type          */
    uint8_t  trklen[2];         /* offset 76 - DS4DEVTK bytes per track */
    uint8_t  trkcyl[2];         /* offset 78 - DS4DSTRK tracks per cyl  */
    uint8_t  devi;              /* offset 80 - overhead, not last block */
    uint8_t  devl;              /* offset 81 - overhead, last block     */
    uint8_t  devk;              /* offset 82 - extra overhead if keyed  */
    uint8_t  devfg;             /* offset 83 - device flags             */
    /*
     * Secondary allocation -- what the dataset may EXTEND by, which the
     * current extents cannot tell you.  A dataset allocated (30,20) in
     * cylinders that has never extended still holds only its 30, but its
     * secondary of 20 is recorded here.
     *
     * sec_flags is DS1SCAL1; its top two bits give the units of sec_qty
     * (see MVS_DSCB_SEC_*).  sec_tracks is the same amount converted to
     * TRACKS, and is 0 when the assembler could not convert it -- which
     * happens for MVS_DSCB_SEC_BLK, where the answer depends on how many
     * blocks fit on a track.  Convert that case with:
     *
     *     n = mvs_blocks_per_track(e->devtype[3],
     *                              MVS_DSCB_U16(e->trklen),
     *                              MVS_DSCB_U16(e->blksize));
     *     tracks = n ? (e->sec_qty + n - 1) / n : 0;
     */
    uint8_t  sec_flags;         /* offset 84 - DS1SCAL1 units + flags   */
    uint8_t  reserved[3];       /* offset 85 - alignment padding        */
    uint32_t sec_qty;           /* offset 88 - DS1SCAL3, in those units */
    uint32_t sec_tracks;        /* offset 92 - same in tracks, 0 if n/a */
} mvs_dscb_info_t;              /* 96 bytes                             */

/*
 * Compile-time check that the struct really is 96 bytes with the fields
 * where the assembler expects them.  A negative array width fails the
 * build rather than letting a silently repacked struct read garbage.
 */
typedef char mvs_dscb_size_check[
    (sizeof(mvs_dscb_info_t) == 96) ? 1 : -1];

/*
 * Units of sec_qty, from the top two bits of sec_flags.  A dataset
 * allocated in cylinders or tracks is converted for you (sec_tracks);
 * one allocated by average block length is not, because that conversion
 * needs the device's track capacity.
 */
#define MVS_DSCB_SEC_UNITS(f)  ((f) & 0xC0)
#define MVS_DSCB_SEC_CYL       0xC0
#define MVS_DSCB_SEC_TRK       0x80
#define MVS_DSCB_SEC_BLK       0x40   /* average block length          */
#define MVS_DSCB_SEC_ABS       0x00   /* absolute track address        */

/* Read one of the raw big-endian halfword fields above. */
#define MVS_DSCB_U16(f)   ((uint16_t)(((uint16_t)(f)[0] << 8) | (f)[1]))

/*
 * Unpack a 3-byte DSCB date.
 *
 * The year byte is a TWO DIGIT year, not (year - 1900): MVS 3.8 predates
 * Y2K and its date routines store 26 for 2026, never 126.  Observed on a
 * live VTOC -- a member written today came back as year 26, day 209.
 * So the century has to be windowed: 00-69 is 20xx, 70-99 is 19xx, the
 * usual convention.  Anything created before 1970 on such a system is
 * beyond saving anyway.
 */
#define MVS_DSCB_YEAR(d)  (((d)[0] < 70 ? 2000 : 1900) + (int)(d)[0])
#define MVS_DSCB_DAY(d)   ((int)(((int)(d)[1] << 8) | (d)[2]))

/*
 * Identify the DASD model from its track length.  Bytes per track is
 * unique per device family and comes straight from the VTOC, so this
 * needs no device-type table and cannot go stale the way a UCB type
 * code lookup would.  Returns "unknown" for anything unrecognised --
 * the caller still has e->trklen and e->devtype to fall back on.
 * Note: 3390/3380/3350 values modified to be those as reported by
 *       disk info in VTOC.
 */
#define MVS_DSCB_MODEL(t)                 \
    ((t) == 58786 ? "3390" :              \
     (t) == 47968 ? "3380" :              \
     (t) == 35616 ? "3375" :              \
     (t) == 19254 ? "3350" :              \
     (t) == 13030 ? "3330" :              \
     (t) ==  8368 ? "3340" :              \
     (t) ==  7294 ? "2314" : "unknown")

/*
 * The same question answered from the UCB device type instead, whose low
 * byte is the device code.  Useful as a cross-check on the track length,
 * and it still works on a volume whose format 4 DSCB has not had its
 * device constants filled in.  0F/0E/0B are confirmed against live
 * 3390/3380/3350 volumes; the rest are unverified.
 */
#define MVS_DSCB_MODEL_T(dt)              \
    (((dt)[3]) == 0x0F ? "3390" :         \
     ((dt)[3]) == 0x0E ? "3380" :         \
     ((dt)[3]) == 0x0B ? "3350" :         \
     ((dt)[3]) == 0x0C ? "3375" :         \
     ((dt)[3]) == 0x0A ? "3340" :         \
     ((dt)[3]) == 0x09 ? "3330" :         \
     ((dt)[3]) == 0x08 ? "2314" : "unknown")

/*
 * NOTE: track capacity now lives in mvsutl.h as mvs_blocks_per_track(),
 * which is EXACT for 3390, 3380 and 3350.  The macro that used to be
 * here derived the answer from the format 4 DSCB's own overhead
 * constants, which turned out to be unusable: they are unpopulated for
 * 3380 and 3390 on MVS 3.8, and even where present they do not mean
 * what they appear to (a 3350 reports DS4DEVI = 11 against a true
 * overhead of 185 bytes per record).
 *
 *     n = mvs_blocks_per_track(e->devtype[3],
 *                              MVS_DSCB_U16(e->trklen),
 *                              MVS_DSCB_U16(e->blksize));
 */

/*
 * Fetch DSCB information for a list of cataloged datasets.
 *
 *   request_type -- MVS_DSCB_REQ_INFO
 *   options      -- reserved, pass 0
 *   dsnlist      -- array of pointers to NUL-terminated dataset names,
 *                   terminated by a NULL pointer
 *   data         -- array of at least as many entries as dsnlist has
 *                   names; each is filled in, including its status
 *
 * Returns:
 *    0  every dataset was reported
 *    4  at least one could not be (check each entry's status; this is
 *       also returned when the device geometry could not be read, in
 *       which case that entry's 'tracks' is zero but its other fields
 *       are still valid)
 *    8  parameter list error (a NULL dsnlist or data pointer)
 *   16  other error (unknown request type)
 *
 * Multi-volume datasets are not supported and are reported with
 * MVS_DSCB_ST_MULTIVOL; a PDS is always single-volume on MVS.
 */
extern int mvs_dscb(
    uint8_t           request_type,
    uint8_t           options,
    const char      **dsnlist,
    mvs_dscb_info_t  *data);

extern int getcib(
    void       *cibdata,
    size_t      cibdatalen,
    int        *length_data);

extern int mvs_dynalloc(
    uint8_t     request_type,
    uint8_t     options,
    const char *dsname,
    const char *member,
    char       *ddname     /* Area char[8] to receive ddname of allocated dataset */);

extern int mvs_stow(
    const char *ddname,
    const char *member,
    void       *userdata,
    int         user_data_length_bytes);

extern int mvs_enq(
    uint8_t     request_type,
    uint8_t     options,
    const char *qname,
    const char *rname);

#endif /* ASMUTILS_H */