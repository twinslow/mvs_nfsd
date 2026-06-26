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
#include "types.h"
#include "mvspdir.h"  /* pds_member_entry_t */

/* -------------------------------------------------------------------- */
/* Pool slot status and cache size                                       */
/* -------------------------------------------------------------------- */
#define MVSVFS_DIR_OPENLIST_FREE    0
#define MVSVFS_DIR_OPENLIST_USED    1
#define MVSVFS_PDS_DIR_CACHE_SIZE   100  /* PDS directory entries cached per open dir */

/* -------------------------------------------------------------------- */
/* Concrete definition of the vfs_dir_t handle (forward-declared in     */
/* nfsd.h as an incomplete struct for callers that only hold pointers).  */
/* -------------------------------------------------------------------- */
struct vfs_dir {
    uint8_t             status;                         /* OPENLIST_FREE / OPENLIST_USED */
    int                 export_idx;
    FILE               *pds_fh;                         /* file handle for the open dir  */
    char                pds_dsname_ebcdic[45];           /* EBCDIC dataset name from path */
    uint64_t            next_cookie;                     /* 1-based index of next entry   */
    int                 pds_entries_cached;              /* valid entries in members[]    */
    pds_member_entry_t  members[MVSVFS_PDS_DIR_CACHE_SIZE];
    int                 end_of_dir_read;                 /* 1 when end-of-directory seen  */
};

/* -------------------------------------------------------------------- */
/* Search operator constants for dir_openlist_search_members2()          */
/* -------------------------------------------------------------------- */
#define SEARCH_OP_LT   1           /* <  strictly less than    */
#define SEARCH_OP_EQ   2           /* =  equal                 */
#define SEARCH_OP_GT   4           /* >  strictly greater than */
#define SEARCH_OP_LE   (SEARCH_OP_LT | SEARCH_OP_EQ)    /* 3: <= */
#define SEARCH_OP_GE   (SEARCH_OP_GT | SEARCH_OP_EQ)    /* 6: >= */

/* Legacy single-operator constant (dir_openlist_search_members only)    */
#define SEARCH_MEMBER_GT    SEARCH_OP_GT

/* -------------------------------------------------------------------- */
/* Prototypes                                                            */
/* -------------------------------------------------------------------- */

/* Initialise the pool (call once at startup). */
void dir_openlist_init(void);

/* Return a pointer to a free, zeroed handle, or NULL (errno=EMFILE). */
vfs_dir_t *dir_openlist_find_free(void);

/* Release a handle back to the pool. */
void dir_openlist_free(vfs_dir_t *entry);

/*
 * Binary search of the cached member list for the first entry that is
 * strictly greater than member_name.  Sets *member_info on success.
 * Returns 0 if found, -1 if not.
 * (operator argument is accepted but currently ignored -- GT only.)
 */
int dir_openlist_search_members(
    vfs_dir_t           *dir_entry,
    const char          *member_name,
    int                  operator,
    pds_member_entry_t **member_info);

/*
 * Binary search of the cached member list with a full operator set
 * (OP_LT, OP_EQ, OP_GT, OP_LE, OP_GE).  Sets *member_info on success.
 * Returns 0 if found, -1 if not.
 */
int dir_openlist_search_members2(
    vfs_dir_t           *dir_entry,
    const char          *member_name,
    int                  operator,
    pds_member_entry_t **member_info);

#endif /* MVSDOL_H_INCLUDED */
