
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

#include "nfsd.h"
#include "mvsio.h"
#include "mvsprw.h"
#include "logger.h"

/* -------------------------------------------------------------------- */
/* Read cache                                                           */
/* -------------------------------------------------------------------- */
static mvs_rcache_entry_t g_mvs_rcache_entries[MVS_RCACHE_ENTRIES];

void mvs_rcache_init(void) 
{
    memset(g_mvs_rcache_entries, 0, sizeof(g_mvs_rcache_entries));
}

mvs_rcache_entry_t *mvs_rcache_find_entry(
    const char *dsname, 
    const char *member_name, 
    int export_idx) 
{
    time_t now = time(NULL);

    int i;
    for (i = 0; i < MVS_RCACHE_ENTRIES; i++) {
        if (g_mvs_rcache_entries[i].status == MVS_RCACHE_STATUS_USED &&
            g_mvs_rcache_entries[i].last_used_time + MVS_RCACHE_MAX_AGE_SECONDS >= now &&
            strcmp(g_mvs_rcache_entries[i].dsname, dsname) == 0 &&
            strcmp(g_mvs_rcache_entries[i].member_name, member_name) == 0 &&
            g_mvs_rcache_entries[i].export_idx == export_idx) {
            return &g_mvs_rcache_entries[i];
        }
    }
    return NULL; // Not found
}

void mvs_rcache_entry_reset(mvs_rcache_entry_t *entry) {
    memset(entry, 0, sizeof(mvs_rcache_entry_t));
}

mvs_rcache_entry_t *mvs_rcache_get_free_entry(void) {
    int i;
    time_t now = time(NULL);
    int lru_index;

    for (i = 0; i < MVS_RCACHE_ENTRIES; i++) {
        if (g_mvs_rcache_entries[i].status == MVS_RCACHE_STATUS_UNUSED ||
            g_mvs_rcache_entries[i].last_used_time + MVS_RCACHE_MAX_AGE_SECONDS < now) {
            mvs_rcache_entry_reset(&g_mvs_rcache_entries[i]);
            return &g_mvs_rcache_entries[i];
        }
    }

    // No free entry found, evict the least recently used entry
    lru_index = 0;
    for (i = 1; i < MVS_RCACHE_ENTRIES; i++) {
        if (g_mvs_rcache_entries[i].last_used_time < g_mvs_rcache_entries[lru_index].last_used_time) {
            lru_index = i;
        }
    }
    mvs_rcache_entry_reset(&g_mvs_rcache_entries[lru_index]);
    return &g_mvs_rcache_entries[lru_index];
}

/* -------------------------------------------------------------------- */
/* High-level file (PDS member) read                                    */
/* -------------------------------------------------------------------- */


int mvs_pds_member_open(
    mvs_rcache_entry_t *cache_entry,
    FILE **fh_return)
{
    const char *open_prefix = "//DSN:"; /* matches prefix used by mvs_open_pds_dir */
    char        file_name_buff[6 + 44 + 1 + 8 + 1 + 1]; /* prefix + dsname + '(' + member + ')' + null */
    const char *mode = "rt";

    strcpy(file_name_buff, open_prefix);
    strcat(file_name_buff, cache_entry->dsname);
    strcat(file_name_buff, "(");
    strcat(file_name_buff, cache_entry->member_name);
    strcat(file_name_buff, ")");

    *fh_return = fopen(file_name_buff, mode);
    if ( !*fh_return ) {
        return -1;
    }
    return 0;
}

int mvs_pds_member_close(
    mvs_rcache_entry_t *cache_entry,
    FILE *fh)
{
    (void)cache_entry;
    return fclose(fh);
}

int mvs_pds_member_pread(
    mvs_rcache_entry_t      *cache_entry,
    FILE                    *fh, 
    uint8_t                 *buff,
    uint32_t                count,
    uint64_t                offset,
    uint32_t                *nread,
    int                     *eof)
{

    int rc = 0;
    size_t read_bytes;
    char     discard[1024];

    log_info("mvsprw.mvs_pds_member_pread: Starting with offset = %llu", offset);

    if (offset > 0) {
        if (cache_entry->has_last_getpos &&
            offset == (cache_entry->last_offset + cache_entry->last_nread) ) {
            log_info("mvsprw.mvs_pds_member_pread: Continuing to read from prior pos");
            /* We have a saved fgetpos value that we can use ...*/
            rc = fsetpos(fh, &cache_entry->last_getpos);
        } else {
            /* We did not have a usable fgetpos value ... so seek */
            log_info("mvsprw.mvs_pds_member_pread: Seeking to new pos");
            rc = fseek(fh, (long)offset, SEEK_SET);
        }
        if ( rc < 0 ) {
            log_info("mvsprw.mvs_pds_member_pread: Got error from fsetpos or fseek");
            mvs_rcache_entry_reset(cache_entry);
            return -1;
        }
    }

    /* Now read the data from the file of the required size */
    log_info("mvsprw.mvs_pds_member_pread: Reading data from file");
    read_bytes = fread(buff, 1, (size_t)count, fh);

    /* Did we hit an error */
    if ( ferror(fh) ) {
        log_info("mvsprw.mvs_pds_member_pread: fread - file in error, errno - %d", errno);
        mvs_rcache_entry_reset(cache_entry);
        return -1;
    }

    /* Did we hit eof? */
    *eof = feof(fh);

    *nread = (uint32_t)read_bytes;

    /* If we hit EOF, then we don't need the cache entry */
    if ( *eof ) {
        mvs_rcache_entry_reset(cache_entry);
        return 0;
    }

    /* Update the cache entry */
    log_info("mvsprw.mvs_pds_member_pread: Updating file position cache entry at 0x%08X", cache_entry);
    cache_entry->last_used_time = time(NULL);
    cache_entry->last_offset = (uint32_t)offset;
    cache_entry->last_nread = (uint32_t)read_bytes;

    log_info("mvsprw.mvs_pds_member_pread: Calling fgetpos");
    rc = fgetpos(fh, &(cache_entry->last_getpos));
    log_info("mvsprw.mvs_pds_member_pread: fgetpos ended rc = %d", rc);
    if ( rc ) {
        fprintf(stderr, "fgetpos returned error, errno - %d", errno);
        /* Because fgetpos returned an error, we'll reset the cache entry so it's not usable */
        mvs_rcache_entry_reset(cache_entry);
    } else {
        log_info("mvsprw.mvs_pds_member_pread: Set indicator in cache that we have fgetpos info");
        cache_entry->has_last_getpos = 1;
    }

    return 0;
}

int mvs_pds_member_read(
    const char  *dsname,
    const char  *member,
    int          export_idx,
    uint64_t     offset,
    uint32_t     count,
    uint8_t     *buf,
    uint32_t    *nread,
    int         *eof)
{
    mvs_rcache_entry_t *cache_entry;
    FILE *fh;
    int rc = 0;
    int saved_errno;

    // Get new or existing cache entry for this file
    // Always start fresh when reading at offset 0
    if (offset == 0) {
        cache_entry = mvs_rcache_get_free_entry();
    } else {
        cache_entry = mvs_rcache_find_entry(dsname, member, export_idx);
        if (cache_entry == NULL) {
            cache_entry = mvs_rcache_get_free_entry();
        }
    }

    // Setup new cache entry
    if ( cache_entry->status == MVS_RCACHE_STATUS_UNUSED ) {
        strncpy(cache_entry->dsname, dsname, sizeof(cache_entry->dsname) - 1);
        strncpy(cache_entry->member_name, member, sizeof(cache_entry->member_name) - 1);
        cache_entry->export_idx = export_idx;
        cache_entry->status = MVS_RCACHE_STATUS_USED;
    }

    // Open the file
    rc = mvs_pds_member_open(cache_entry, &fh);
    if ( rc < 0 )
        return -1;

    // Read the requested data from the file
    rc = mvs_pds_member_pread(cache_entry, fh, buf, count, offset, nread, eof);
    if ( rc  < 0 ) {
        saved_errno = errno;
        (void)mvs_pds_member_close(cache_entry, fh);
        errno = saved_errno;
        return -1;
    }

    // Close the file
    rc = mvs_pds_member_close(cache_entry, fh);
    if ( rc < 0 ) {
        return -1;
    }
    log_info("mvs_pds_member_read: Read completed (member closed)");
    return 0;
}
