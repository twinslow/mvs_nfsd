/*
 * testdscb.c - standalone driver for the MVSDSCB assembler module.
 *
 * Builds a NULL-terminated list of dataset names, calls mvs_dscb() once
 * for the whole list, and prints what came back.  Dataset names come from
 * the JCL PARM (comma separated) if given, otherwise a small built-in set
 * is used.
 *
 * WHAT TO LOOK FOR, in the order the faults are most likely:
 *
 *   1. The character fields.  If dsname/volser are shifted or full of
 *      rubbish, the format 1 DSCB DSECT is mis-based.  OBTAIN CAMLST
 *      SEARCH returns only the 96-byte DATA portion, so mvsdscb.asm bases
 *      the DSECT 44 bytes low; getting that wrong moves every field.
 *
 *   2. The track count.  Extents are CCHH ranges, so tracks depend on the
 *      device geometry read from the volume's format 4 DSCB.  A count that
 *      is wrong by a consistent factor points at the DS4DSTRK offset; a
 *      count of zero means the format 4 read failed altogether (the
 *      overall return code will be 4 in that case).
 *
 *   3. The extent walk.  A dataset with more than 3 extents needs the
 *      format 3 DSCB, which is read with CAMLST SEEK -- and SEEK returns
 *      the FULL 140 bytes including the key, unlike SEARCH.  If a
 *      3-extent dataset is right but a 5-extent one is wrong, that is the
 *      place to look.
 *
 * Build: see jcl/mktstdsc.jcl
 */

#include <stdio.h>
#include <string.h>

#include "types.h"
#include "asmutils.h"
#include "mvsutl.h"   /* mvs_blocks_per_track */

#define MAX_DSN      16       /* datasets accepted in one call        */
#define DSN_TEXT_MAX 45       /* 44 + NUL                             */

/* The names used when no PARM is supplied.  Deliberately chosen to
   exercise the interesting paths; adjust to suit the system. */
static const char *g_default_dsn[] = {
    "SYS1.MACLIB",             /* big, almost certainly multi-extent  */
    "SYS2.MACLIB",
    "TEMP.ITEST.FB",           /* small PDS made by the itest JCL     */
    "NO.SUCH.DATASET.AT.ALL",  /* must come back as status 8          */
    NULL
};

/* -------------------------------------------------------------------- */
/* Printing helpers                                                      */
/* -------------------------------------------------------------------- */

static const char *status_text(uint8_t st)
{
    switch (st) {
    case MVS_DSCB_ST_OK:       return "ok";
    case MVS_DSCB_ST_NOTFOUND: return "NOT FOUND";
    case MVS_DSCB_ST_MULTIVOL: return "MULTI-VOLUME (unsupported)";
    default:                   return "??? unexpected status";
    }
}

/*
 * A DSCB date is 3 bytes: a two digit year, then the day of the year as
 * a binary halfword.  All zeros means the field was never set.  The
 * century is windowed by MVS_DSCB_YEAR -- MVS 3.8 stores 26 for 2026.
 */
static void print_date(const char *label, const uint8_t *d)
{
    if (d[0] == 0 && d[1] == 0 && d[2] == 0) {
        printf("  %-10s (not set)\n", label);
        return;
    }
    printf("  %-10s %4d.%03d   (raw %02X %02X %02X)\n",
           label, MVS_DSCB_YEAR(d), MVS_DSCB_DAY(d), d[0], d[1], d[2]);
}

/*
 * DSORG and RECFM are printed raw as well as decoded: if the decode looks
 * absurd the raw bytes say whether the field is wrong or just unfamiliar.
 * Bit values are from the format 1 DSCB.
 */
static void print_dsorg(const uint8_t *o)
{
    uint16_t v = MVS_DSCB_U16(o);

    printf("  %-10s %04X  ", "dsorg", v);
    if (o[0] & 0x40) printf("PS ");
    if (o[0] & 0x02) printf("PO ");
    if (o[0] & 0x20) printf("DA ");
    if (o[0] & 0x80) printf("IS ");
    if (o[0] & 0x01) printf("unmovable ");
    printf("\n");
}

static void print_recfm(uint8_t r)
{
    printf("  %-10s %02X    ", "recfm", r);
    switch (r & 0xC0) {
    case 0x40: printf("V"); break;
    case 0x80: printf("F"); break;
    case 0xC0: printf("U"); break;
    default:   printf("?"); break;
    }
    if (r & 0x10) printf("B");
    if (r & 0x04) printf("A");
    if (r & 0x02) printf("M");
    if (r & 0x08) printf("S");
    printf("\n");
}

/* Text for the secondary-allocation units in DS1SCAL1. */
static const char *sec_units(uint8_t f)
{
    switch (MVS_DSCB_SEC_UNITS(f)) {
    case MVS_DSCB_SEC_CYL: return "cylinders";
    case MVS_DSCB_SEC_TRK: return "tracks";
    case MVS_DSCB_SEC_BLK: return "avg block length";
    default:               return "absolute track address";
    }
}

static void print_entry(int n, const char *asked_for,
                        const mvs_dscb_info_t *e)
{
    char     dsn[DSN_TEXT_MAX];
    char     vol[7];
    uint16_t trklen;
    uint16_t blksz;
    int      sec_trk;

    /* The returned name and volser are blank padded, not NUL terminated. */
    memcpy(dsn, e->dsname, 44);
    dsn[44] = '\0';
    memcpy(vol, e->volser, 6);
    vol[6] = '\0';

    printf("\n[%d] requested: %s\n", n, asked_for);
    printf("  %-10s %d (%s)\n", "status", (int)e->status,
           status_text(e->status));

    if (e->status != MVS_DSCB_ST_OK) {
        /* Even a failed entry echoes the name back, which is worth
           showing: if THAT is wrong the fault is in the input handling,
           not in the VTOC lookup. */
        printf("  %-10s '%s'\n", "dsname", dsn);
        return;
    }

    printf("  %-10s '%s'\n", "dsname", dsn);
    printf("  %-10s '%s'\n", "volser", vol);
    printf("  %-10s %d\n",   "extents", (int)e->nextents);
    printf("  %-10s %lu\n",  "tracks", (unsigned long)e->tracks);
    print_date("created", e->create_date);
    print_date("expires", e->expire_date);
    print_date("last ref", e->ref_date);
    print_dsorg(e->dsorg);
    print_recfm(e->recfm);
    printf("  %-10s %u\n", "blksize", (unsigned)MVS_DSCB_U16(e->blksize));
    printf("  %-10s %u\n", "lrecl",   (unsigned)MVS_DSCB_U16(e->lrecl));

    /* Device characteristics, and what they imply. */
    trklen = MVS_DSCB_U16(e->trklen);
    blksz  = MVS_DSCB_U16(e->blksize);

    printf("  %-10s %02X %02X %02X %02X\n", "devtype",
           e->devtype[0], e->devtype[1], e->devtype[2], e->devtype[3]);
    printf("  %-10s %u bytes/track, %u tracks/cyl  -> %s (by track len)"
           " / %s (by dev type)\n",
           "geometry", (unsigned)trklen,
           (unsigned)MVS_DSCB_U16(e->trkcyl), MVS_DSCB_MODEL(trklen),
           MVS_DSCB_MODEL_T(e->devtype));
    printf("  %-10s I=%u L=%u K=%u FG=%02X%s\n", "dev const",
           (unsigned)e->devi, (unsigned)e->devl, (unsigned)e->devk,
           e->devfg,
           (e->devi == 0 && e->devl == 0)
               ? "   (not populated on this system)" : "");

    if (blksz > 0) {
        int per_trk = mvs_blocks_per_track(e->devtype[3], trklen, blksz);
        int exact   = mvs_blocks_exact(e->devtype[3]);

        printf("  %-10s %d blocks of %u per track  [%s]\n", "capacity",
               per_trk, (unsigned)blksz,
               exact ? "exact" : "ESTIMATE, upper bound - unknown device");
        printf("  %-10s %d tracks allocated => room for %d blocks\n",
               "", (int)e->tracks, per_trk * (int)e->tracks);

        /*
         * Secondary allocation.  The assembler converts cylinders and
         * tracks for us; average-block-length has to be done here,
         * because it needs the track capacity worked out just above.
         */
        sec_trk = (int)e->sec_tracks;
        if (sec_trk == 0 &&
            MVS_DSCB_SEC_UNITS(e->sec_flags) == MVS_DSCB_SEC_BLK &&
            per_trk > 0)
            sec_trk = ((int)e->sec_qty + per_trk - 1) / per_trk;

        printf("  %-10s %lu %s", "secondary",
               (unsigned long)e->sec_qty, sec_units(e->sec_flags));
        if (sec_trk > 0)
            printf(" = %d tracks per extent", sec_trk);
        else if (e->sec_qty == 0)
            printf("  (none - the dataset cannot extend)");
        else
            printf("  (could not be converted to tracks)");
        printf("   [flags %02X]\n", e->sec_flags);
    }

    /* Cheap sanity checks -- these are the symptoms described at the top
       of this file, called out so they are not missed in the output. */
    if (e->tracks == 0)
        printf("  ** tracks is zero: the format 4 DSCB read probably"
               " failed, so the device geometry is unknown\n");
    if (e->nextents == 0)
        printf("  ** extent count is zero, which no allocated dataset"
               " should report\n");
    if (vol[0] == ' ' || vol[0] == '\0')
        printf("  ** volser is blank on a dataset reported as found\n");
    if (trklen == 0)
        printf("  ** track length is zero: the device geometry was not"
               " read, so model and capacity are unknown\n");
}

/* -------------------------------------------------------------------- */
/* Split a comma separated PARM into individual dataset names            */
/* -------------------------------------------------------------------- */

static int split_parm(char *parm, char *store, const char **list, int max)
{
    int   n = 0;
    char *p = parm;
    char *out = store;

    while (*p != '\0' && n < max) {
        char *start = out;

        while (*p != '\0' && *p != ',' && (out - start) < DSN_TEXT_MAX - 1)
            *out++ = *p++;
        *out++ = '\0';

        if (start[0] != '\0')
            list[n++] = start;

        while (*p == ',')
            p++;
    }
    list[n] = NULL;
    return n;
}

/* -------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    const char      *dsnlist[MAX_DSN + 1];
    char             dsnstore[MAX_DSN * DSN_TEXT_MAX];
    mvs_dscb_info_t  info[MAX_DSN];
    int              count;
    int              rc;
    int              i;

    /* How the JCL PARM arrives is up to the runtime: it may come through
       whole as argv[1], or already split into separate arguments.  Handle
       both rather than depending on which. */
    if (argc > 2) {
        for (count = 0; count < MAX_DSN && count < argc - 1; count++)
            dsnlist[count] = argv[count + 1];
        dsnlist[count] = NULL;
        printf("testdscb: %d dataset name(s) from separate arguments\n",
               count);
    } else if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') {
        count = split_parm(argv[1], dsnstore, dsnlist, MAX_DSN);
        printf("testdscb: %d dataset name(s) from the PARM\n", count);
    } else {
        for (count = 0; count < MAX_DSN && g_default_dsn[count] != NULL;
             count++)
            dsnlist[count] = g_default_dsn[count];
        dsnlist[count] = NULL;
        printf("testdscb: no PARM given, using %d built-in name(s)\n",
               count);
    }

    if (count == 0) {
        printf("testdscb: nothing to do\n");
        return 4;
    }

    /* Poison the output area first.  If a field comes back as X'BB' the
       assembler never wrote it, which is a very different fault from
       writing the wrong value. */
    memset(info, 0xBB, sizeof(info));

    rc = mvs_dscb(MVS_DSCB_REQ_INFO, 0, dsnlist, info);

    printf("testdscb: mvs_dscb() returned %d", rc);
    switch (rc) {
    case 0:  printf("  (all datasets reported)\n");          break;
    case 4:  printf("  (one or more could not be reported,"
                    " see each status below)\n");            break;
    case 8:  printf("  (PARAMETER LIST ERROR)\n");           break;
    case 16: printf("  (OTHER ERROR - bad request type?)\n"); break;
    default: printf("  (UNEXPECTED return code)\n");         break;
    }

    if (rc == 8 || rc == 16)
        return rc;

    for (i = 0; i < count; i++)
        print_entry(i, dsnlist[i], &info[i]);

    printf("\ntestdscb: done\n");
    return rc;
}
