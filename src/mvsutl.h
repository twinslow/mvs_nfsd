#ifndef _MVSUTL_H_INCLUDED
#define _MVSUTL_H_INCLUDED

#include <time.h>    /* time_t for the timezone conversion helpers */
#include "types.h"   /* uint8_t / uint32_t for the capacity helpers */

char *get_jes2_jobid(void);
int get_int_cvt_val(int cvt_offset);
int get_tz_offset(void);

/* -------------------------------------------------------------------- */
/* Timezone handling                                                     */
/*                                                                       */
/* This MVS host runs its TOD clock on LOCAL time (not GMT), but JCC's    */
/* time()/gettimeofday() already return correct UTC epoch seconds (the C  */
/* runtime applies the CVTLDTO offset), so wall-clock "now" needs NO      */
/* correction.  ISPF member statistics, however, are stored natively in   */
/* packed-decimal LOCAL time, so converting them to/from UTC epoch needs  */
/* the local-minus-GMT offset.  That offset lives in the CVT (CVTLDTO)    */
/* and only changes across an IPL, so mvs_tz_init() reads it ONCE at      */
/* startup and caches it in a global for cheap access thereafter.         */
/*                                                                       */
/* On a non-MVS build (and before mvs_tz_init() runs) the cached offset  */
/* is 0, so both conversions below are the identity.                      */
/* -------------------------------------------------------------------- */

/* Read CVTLDTO once and cache it.  Call once at startup.  No-op (leaves
   the offset 0) on a non-MVS build. */
void   mvs_tz_init(void);

/* Cached local-minus-GMT offset in seconds (negative west of GMT). */
int    mvs_tz_offset(void);

/* Override the cached offset directly, bypassing the CVT read.  Provided so
   the pure conversions below can be exercised with a known offset (and as a
   hook for any future non-CVT offset source).  Normal startup uses
   mvs_tz_init() instead. */
void   mvs_tz_set_offset(int seconds);

/* Convert a decoded ISPF LOCAL wall-clock epoch (a value whose gmtime()
   yields the local wall clock) to true UTC epoch seconds. */
time_t mvs_local_epoch_to_utc(time_t local_epoch);

/* Inverse: convert a true UTC epoch to a LOCAL-domain epoch, so gmtime()
   of the result yields the local wall clock for encoding back into ISPF
   stats. */
time_t mvs_utc_to_local_epoch(time_t utc_epoch);


/* -------------------------------------------------------------------- */
/* DASD track capacity                                                   */
/*                                                                       */
/* Device codes are the low byte of the UCB device type, which is what   */
/* mvs_dscb() returns in mvs_dscb_info_t.devtype[3].  Confirmed against  */
/* live 3390, 3380 and 3350 volumes.                                     */
/* -------------------------------------------------------------------- */
#define MVS_DEV_3350   0x0B
#define MVS_DEV_3375   0x0C
#define MVS_DEV_3380   0x0E
#define MVS_DEV_3390   0x0F

/*
 * Physical blocks of 'blksize' bytes that fit on one track of the given
 * device.  EXACT for 3390, 3380 and 3350 -- each is derived from IBM's
 * published capacities and checked at every documented boundary.
 *
 * For any other device the answer is an overhead-free estimate from
 * trklen, which is an UPPER BOUND and may be one block high; pass the
 * VTOC's track length for that case (it is ignored for the three known
 * devices).  Use mvs_blocks_exact() to find out which you got.
 *
 * Returns 0 if nothing fits, or blksize is 0.
 */
int mvs_blocks_per_track(uint8_t devcode, uint32_t trklen, uint32_t blksize);

/* Non-zero if mvs_blocks_per_track() is exact for this device rather
   than an upper-bound estimate. */
int mvs_blocks_exact(uint8_t devcode);

/* -------------------------------------------------------------------- */
/* Abend diagnostics                                                     */
/* -------------------------------------------------------------------- */

/*
 * System completion code out of an SDWA copy -- e.g. 0xB14 for an SB14.
 * 'sdwa' is the unsigned int[26] (104-byte IHASDWA copy) filled in by
 * _setjmp_stae() when it intercepts an abend.
 */
#define SDWA_ABEND_CODE(sdwa)   (((sdwa)[1] & 0x00FFF000u) >> 12)

#endif
