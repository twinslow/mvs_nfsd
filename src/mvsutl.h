#ifndef _MVSUTL_H_INCLUDED
#define _MVSUTL_H_INCLUDED

#include <time.h>    /* time_t for the timezone conversion helpers */

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

/* Convert a decoded ISPF LOCAL wall-clock epoch (a value whose gmtime()
   yields the local wall clock) to true UTC epoch seconds. */
time_t mvs_local_epoch_to_utc(time_t local_epoch);

/* Inverse: convert a true UTC epoch to a LOCAL-domain epoch, so gmtime()
   of the result yields the local wall clock for encoding back into ISPF
   stats. */
time_t mvs_utc_to_local_epoch(time_t utc_epoch);

#endif
