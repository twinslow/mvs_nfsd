#include <stdio.h>
#include <string.h>
#include <time.h>

#include "types.h"    /* uint8_t / uint16_t / uint32_t */
#include "mvsutl.h"
#include "asmutils.h"

typedef unsigned char  BYTE;
typedef unsigned int   ADDR31;   /* S/370 addresses fit in 4 bytes */

/*
 * Read a 31-bit pointer field stored at absolute address 'addr'.
 * MVS pointer fields sometimes have flag bits in the high byte
 * (e.g. AVT slot in-use bit), so mask to 31 bits to be safe.
 */
static void *fetch_ptr(ADDR31 addr)
{
    ADDR31 raw = *(volatile ADDR31 *)addr;
    return (void *)(raw & 0x7FFFFFFFu);
}

/*
 * get_jes2_jobid()
 *
 * Walks PSA -> TCB -> JSCB -> SSIB for the CURRENTLY EXECUTING TCB
 * and extracts the JES2-assigned job identifier.
 *
 *   PSA + X'21C' (540) -> current TCB     (PSATOLD)
 *   TCB + X'B4'        -> JSCB            (TCBJSCB)
 *   JSCB + X'13C'       -> SSIB            (JSCBSSIB)
 *   SSIB + X'0C', 8 bytes -> job id, e.g. "JOB01234"
 *
 * Returns a static char output buffer pointer, or
 * NULL if any pointer in the chain is null.
 */
char *get_jes2_jobid()
{
    static char out_jobid[9];
    BYTE *tcb, *jscb, *ssib;

    tcb = fetch_ptr(540);
    if (tcb == NULL) return NULL;

    jscb = fetch_ptr((ADDR31)(tcb + 0xB4));
    if (jscb == NULL) return NULL;

    ssib = fetch_ptr((ADDR31)(jscb + 0x13C));
    if (ssib == NULL) return NULL;

    memcpy(out_jobid, ssib + 0x0C, 8);
    out_jobid[8] = '\0';

    return &out_jobid[0];
}

int get_int_cvt_val(int cvt_offset)
{
    void **pcvt = (void **)0x00000010;
    unsigned char *cvt = *(unsigned char **)pcvt;
    int val;

    //val = *(int *)( cvt + 0x0130 );
    val = *(int *)( cvt + cvt_offset );
    fprintf(stderr, "cvt at 0x%08X, offset 0x%08X = %d\n",
        cvt, cvt_offset, val);
    return val;
}

/*
 * This functions rounds to the qtr hour, if it is just 1 second off
 */
static int round_to_qtr_hour(int n) {
    int r = n % 900;
    if (r < 0) r += 900;   /* C's % is truncating, not floor-mod */
    if (r == 1)    return n - 1;
    if (r == 899) return n + 1;
    return n;
}

int get_tz_offset()
{
    int seconds;
    long long offset;

    offset = get_int_cvt_val(0x0130);

    fprintf(stderr, "offset (tod ticks) %d\n",offset);

    seconds = (int) (1048567LL * offset / 1000000LL);

    fprintf(stderr, "seconds=%d\n", seconds);
    // Round to half hour if it within 1 second
    return round_to_qtr_hour(seconds);
}

/* -------------------------------------------------------------------- */
/* Timezone offset cache + LOCAL<->UTC epoch conversion.  See mvsutl.h.  */
/* -------------------------------------------------------------------- */

/* local - GMT, in seconds.  0 until mvs_tz_init() runs, and always 0 on a
   non-MVS build, so every conversion is the identity there. */
static int g_tz_offset = 0;

void mvs_tz_init(void)
{
    /* CVTLDTO only changes across an IPL, so reading it once is enough. */
    g_tz_offset = get_tz_offset();
}

int mvs_tz_offset(void)
{
    return g_tz_offset;
}

void mvs_tz_set_offset(int seconds)
{
    g_tz_offset = seconds;
}

time_t mvs_local_epoch_to_utc(time_t local_epoch)
{
    return local_epoch - (time_t)g_tz_offset;
}

time_t mvs_utc_to_local_epoch(time_t utc_epoch)
{
    return utc_epoch + (time_t)g_tz_offset;
}

/* ==================================================================== */
/* DASD track capacity                                                  */
/*                                                                      */
/* How many physical blocks of a given size fit on one track.  This is  */
/* per DEVICE, not per dataset, because it depends entirely on the      */
/* recording geometry.                                                  */
/*                                                                      */
/* Note that the format 4 DSCB's own overhead constants (DS4DEVI etc.)  */
/* are NOT used.  They are unpopulated on this system for 3380 and 3390 */
/* -- MVS 3.8 predates both, and those volumes were initialised by      */
/* tooling that filled in only the geometry -- and even where they ARE  */
/* present they do not mean what a naive reading suggests: a 3350       */
/* reports DS4DEVI = 11, yet its true per-record overhead is 185 bytes. */
/* So each device is handled from published figures instead, and every  */
/* case below is checked against a known boundary value.                */
/* ==================================================================== */

/*
 * 3390: no simple formula reproduces IBM's published capacities -- the
 * successive block-size steps share no common cell size -- so the table
 * from the documentation is transcribed directly.  Rows are ordered by
 * DESCENDING minimum length, and each row's range is contiguous with the
 * next, so the first row whose minimum a block reaches is its answer.
 *
 * Source: IBM z/OS "Basic access methods - track capacity", table
 * "3390 track capacity without keys".
 */
typedef struct {
    uint32_t min_len;    /* smallest block length that yields nrec */
    uint16_t nrec;       /* blocks per track                       */
} trkcap_row_t;

static const trkcap_row_t g_cap_3390[] = {
    { 27999,  1 },   /* 27999 - 56664 */
    { 18453,  2 },   /* 18453 - 27998 */
    { 13683,  3 },   /* 13683 - 18452 */
    { 10797,  4 },   /* 10797 - 13682 */
    {  8907,  5 },   /*  8907 - 10796 */
    {  7549,  6 },   /*  7549 -  8906 */
    {  6519,  7 },   /*  6519 -  7548 */
    {  5727,  8 },   /*  5727 -  6518 */
    {  5065,  9 },   /*  5065 -  5726 */
    {  4567, 10 },   /*  4567 -  5064 */
    {  4137, 11 },   /*  4137 -  4566 */
    {  3769, 12 },   /*  3769 -  4136 */
    {  3441, 13 },   /*  3441 -  3768 */
    {  3175, 14 },   /*  3175 -  3440 */
    {  2943, 15 },   /*  2943 -  3174 */
    {  2711, 16 },   /*  2711 -  2942 */
    {  2547, 17 },   /*  2547 -  2710 */
    {  2377, 18 },   /*  2377 -  2546 */
    {  2213, 19 },   /*  2213 -  2376 */
    {  2083, 20 },   /*  2083 -  2212 */
    {  1947, 21 },   /*  1947 -  2082 */
    {  1851, 22 },   /*  1851 -  1946 */
    {  1749, 23 },   /*  1749 -  1850 */
    {  1647, 24 },   /*  1647 -  1748 */
    {  1551, 25 },   /*  1551 -  1646 */
    {  1483, 26 },   /*  1483 -  1550 */
    {  1387, 27 },   /*  1387 -  1482 */
    {  1319, 28 },   /*  1319 -  1386 */
    {  1251, 29 },   /*  1251 -  1318 */
    {  1183, 30 },   /*  1183 -  1250 */
    {  1155, 31 },   /*  1155 -  1182 */
    {  1087, 32 },   /*  1087 -  1154 */
    {  1019, 33 },   /*  1019 -  1086 */
    {   985, 34 },   /*   985 -  1018 */
    {   951, 35 },   /*   951 -   984 */
    {   889, 36 },   /*   889 -   950 */
    {   855, 37 },   /*   855 -   888 */
    {   821, 38 },   /*   821 -   854 */
    {   787, 39 },   /*   787 -   820 */
    {   753, 40 },   /*   753 -   786 */
    {   719, 41 },   /*   719 -   752 */
    {   691, 42 },   /*   691 -   718 */
    {   657, 43 },   /*   657 -   690 */
    {   623, 44 },   /*   623 -   656 */
    {   589, 45 },   /*   589 -   622 */
    {   555, 46 },   /*   555 -   588 */
    {   521, 48 },   /*   521 -   554 */
    {   487, 49 },   /*   487 -   520 */
    {   459, 50 },   /*   459 -   486 */
    {   425, 52 },   /*   425 -   458 */
    {   391, 54 },   /*   391 -   424 */
    {   357, 55 },   /*   357 -   390 */
    {   323, 57 },   /*   323 -   356 */
    {   289, 59 },   /*   289 -   322 */
    {   255, 61 },   /*   255 -   288 */
    {   227, 64 },   /*   227 -   254 */
    {   193, 66 },   /*   193 -   226 */
    {   159, 69 },   /*   159 -   192 */
    {   125, 72 },   /*   125 -   158 */
    {    91, 75 },   /*    91 -   124 */
    {    57, 78 },   /*    57 -    90 */
    {    23, 82 },   /*    23 -    56 */
    {     1, 86 },   /*     1 -    22 */
};

#define CAP_3390_ROWS  (sizeof(g_cap_3390) / sizeof(g_cap_3390[0]))

int mvs_blocks_per_track(uint8_t devcode, uint32_t trklen, uint32_t blksize)
{
    unsigned i;

    if (blksize == 0)
        return 0;

    switch (devcode) {

    case MVS_DEV_3390:
        /* Longer than the largest supported block: nothing fits. */
        if (blksize > 56664u)
            return 0;
        for (i = 0; i < CAP_3390_ROWS; i++) {
            if (blksize >= g_cap_3390[i].min_len)
                return (int)g_cap_3390[i].nrec;
        }
        return 0;

    case MVS_DEV_3380:
        /*
         * 1499 cells of 32 bytes (1499 * 32 = 47968, which matches the
         * track length reported by the VTOC).  Each record costs 15
         * cells of overhead -- 8 for the count field, 7 for the data --
         * plus its data rounded UP to whole cells.  Verified exact at
         * every published boundary from 1 to 7 blocks per track.
         */
        return (int)(1499u / (15u + ((blksize + 12u + 31u) / 32u)));

    case MVS_DEV_3350:
        /*
         * Not cell based.  A flat 185 bytes of overhead per record, and
         * n * (185 + blksize) fills the 19254 byte track EXACTLY at
         * each published boundary (1 block of 19069, 2 of 9442, 3 of
         * 6233), which is a strong confirmation of the constant.
         */
        return (int)(19254u / (185u + blksize));

    default:
        /*
         * Unknown device.  Fall back to an overhead-free estimate, which
         * is an UPPER BOUND and can be one block high.  mvs_blocks_exact()
         * reports that this happened so a caller can refuse to rely on it.
         */
        if (trklen == 0)
            return 0;
        return (int)(trklen / blksize);
    }
}

int mvs_blocks_exact(uint8_t devcode)
{
    return (devcode == MVS_DEV_3390 ||
            devcode == MVS_DEV_3380 ||
            devcode == MVS_DEV_3350) ? 1 : 0;
}

char *mvs_dscb_dsorg_str(uint8_t dsorg, char *str) {
    switch(dsorg) {
        case MVS_DSCB_DSORG_IS: strcpy(str, "IS"); break;
        case MVS_DSCB_DSORG_PS: strcpy(str, "PS"); break;
        case MVS_DSCB_DSORG_DA: strcpy(str, "DA"); break;
        case MVS_DSCB_DSORG_PO: strcpy(str, "PO"); break;
        default: strcpy(str, "?");
    }
    if (dsorg & MVS_DSCB_DSORG_UNMV)
        strcat(str, "U");
    return str;
}

char *mvs_dscb_recfm_str(uint8_t recfm, char *str) {
    switch(recfm & MVS_DSCB_RECFM_MASK) {
        case MVS_DSCB_RECFM_V: strcpy(str, "V"); break;
        case MVS_DSCB_RECFM_F: strcpy(str, "F"); break;
        case MVS_DSCB_RECFM_U: strcpy(str, "U"); break;
        default: strcpy(str, "?");
    }
    if (recfm & MVS_DSCB_RECFM_BLK)
        strcat(str, "B");
    return str;
}


