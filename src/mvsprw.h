#ifndef MVSPRW_H_INCLUDED
#define MVSPRW_H_INCLUDED

#include <stdio.h>
#include <time.h>
#include "types.h"

#define MVS_RCACHE_ENTRIES                      20
#define MVS_RCACHE_STATUS_USED                  1
#define MVS_RCACHE_STATUS_UNUSED                0
#define MVS_RCACHE_MAX_AGE_SECONDS              5

/* -------------------------------------------------------------------- */
/* Read cache                                                           */
/* -------------------------------------------------------------------- */
typedef struct mvs_rcache_entry {
    uint8_t    status;     /* 0 = not in use, 1 = in use */

    char       dsname[45];
    char       member_name[9];
    int        export_idx;

    time_t     last_used_time; /* last used time for this cache entry, for LRU eviction */

    uint32_t   last_offset;    /* last read offset from NFS client */
    uint32_t   last_nread;     /* number of bytes returned to client for last read */
    fpos_t     last_getpos;    /* file position on host at end of the last read */
    uint8_t    has_last_getpos;/* 1 if last_getpos holds a usable position */

} mvs_rcache_entry_t;

void mvs_rcache_init(void);
mvs_rcache_entry_t *mvs_rcache_find_entry(
    const char *dsname,
    const char *member_name,
    int export_idx);
void mvs_rcache_entry_reset(mvs_rcache_entry_t *entry);
mvs_rcache_entry_t *mvs_rcache_get_free_entry(void);

int mvs_pds_member_open(
    mvs_rcache_entry_t *cache_entry,
    FILE               **fh_return);

int mvs_pds_member_close(
    mvs_rcache_entry_t *cache_entry,
    FILE               *fh);

int mvs_pds_member_pread(
    mvs_rcache_entry_t *cache_entry,
    FILE               *fh,
    uint8_t            *buff,
    uint32_t            count,
    uint64_t            offset,
    uint32_t           *nread,
    int                *eof);


/* -------------------------------------------------------------------- */
/* High-level file read                                                 */
/* -------------------------------------------------------------------- */

int mvs_pds_member_read(
    const char  *dsname,
    const char  *member,
    int          export_idx,
    uint64_t     offset,
    uint32_t     count,
    uint8_t     *buf,
    uint32_t    *nread,
    int         *eof);


#endif
