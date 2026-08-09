/*
 * mvsdol.h - MVS directory open-list (DOL) for the NFS server.
 *
 * Defines the vfs_dir_t structure (the concrete implementation of the
 * opaque vfs_dir_t handle declared in nfsd.h) and the pool of open
 * directory handles used by the MVS VFS layer (mvsvfs.c).
 *
 * The dir_openlist_* functions manage that pool:
 *
 *   dir_openlist_init()              -- zero the entire pool at startup
 *   dir_openlist_find_free()         -- allocate one handle
 *   dir_openlist_free()              -- release a handle
 *   dir_openlist_search_members()    -- binary search (GT only)
 *   dir_openlist_search_members2()   -- binary search (full operator set)
 *
 * Operator constants for dir_openlist_search_members2():
 *
 *   OP_LT  (1)   strictly less than
 *   OP_EQ  (2)   equal
 *   OP_GT  (4)   strictly greater than
 *   OP_LE  (3)   less than or equal    (OP_LT | OP_EQ)
 *   OP_GE  (6)   greater than or equal (OP_GT | OP_EQ)
 */

#ifndef MVSDOL_H_INCLUDED
#define MVSDOL_H_INCLUDED

#include <stdio.h>
#include <time.h>
#include "types.h"
#include "mvspdir.h"  /* pds_member_entry_t, pds_member_list_t */

/* --------------------------------------------------------------------- */
/* Pool slot status and entry timeout                                    */
/* --------------------------------------------------------------------- */
#define MVSVFS_DIR_OPENLIST_FREE          0
#define MVSVFS_DIR_OPENLIST_USED          1
#define MVSVFS_DIR_OPENLIST_TIMEOUT_SECS  30   /* seconds before an idle USED entry expires */

/* --------------------------------------------------------------------- */
/* Concrete definition of the vfs_dir_t handle (forward-declared in      */
/* nfsd.h as an incomplete struct for callers that only hold pointers).  */
/* --------------------------------------------------------------------- */
struct vfs_dir {
    uint8_t             status;                     /* OPENLIST_FREE / OPENLIST_USED */
    int                 dir_level;                  /* MVS_PATH_TYPE_ROOT or _DATASET */
    int                 export_idx;
    int                 dataset_idx;                /* dataset within export (-1 for ROOT) */
    FILE               *pds_fh;                     /* file handle for the open dir  */
    char                pds_dsname_ebcdic[45];      /* EBCDIC dataset name from path */
    uint64_t            next_cookie;                /* This 64bit value is actually the EBCDIC member name  */
    pds_member_list_t   member_list;                /* growable cache of PDS directory members  */
    int                 end_of_dir_read;            /* 1 when end-of-directory seen  */
    time_t              last_used_time;             /* wall-clock time of last access */
};

/* --------------------------------------------------------------------- */
/* Search operator constants for dir_openlist_search_members2()          */
/* --------------------------------------------------------------------- */
#define SEARCH_OP_LT   1           /* <  strictly less than    */
#define SEARCH_OP_EQ   2           /* =  equal                 */
#define SEARCH_OP_GT   4           /* >  strictly greater than */
#define SEARCH_OP_LE   (SEARCH_OP_LT | SEARCH_OP_EQ)    /* 3: <= */
#define SEARCH_OP_GE   (SEARCH_OP_GT | SEARCH_OP_EQ)    /* 6: >= */

/* --------------------------------------------------------------------- */
/* Prototypes                                                            */
/* --------------------------------------------------------------------- */

/* Initialise the pool (call once at startup). */
void dir_openlist_init(void);

/* Return a pointer to a free, zeroed handle, or NULL (errno=EMFILE).
 * USED entries that have exceeded MVSVFS_DIR_OPENLIST_TIMEOUT_SECS are
 * reclaimed and may be returned.  The returned entry has last_used_time
 * set to the current time. */
vfs_dir_t *dir_openlist_find_free(void);

/* Release a handle back to the pool. */
void dir_openlist_free(vfs_dir_t *entry);

/* Search the pool for a USED, non-expired entry whose pds_dsname_ebcdic
 * matches the supplied name.  Refreshes last_used_time on a hit.
 * Returns NULL if not found or if the matching entry has timed out. */
vfs_dir_t *dir_openlist_find_by_dsname(const char *pds_dsname_ebcdic);

/* Drop any cached directory listing for a PDS (call after a member STOW so
 * the next readdir re-reads the directory and sees the new member). */
void dir_openlist_invalidate(const char *pds_dsname_ebcdic);

/*
 * Binary search of the cached member list with a full operator set
 * (SEARCH_OP_LT, SEARCH_OP_EQ, SEARCH_OP_GT, SEARCH_OP_LE, SEARCH_OP_GE).
 * Sets *member_info on success.
 * Returns 0 if found, -1 if not.
 */
int dir_openlist_search_members(
    vfs_dir_t           *dir_entry,
    const char          *member_name,
    int                  operator,
    pds_member_entry_t **member_info);

#endif /* MVSDOL_H_INCLUDED */
