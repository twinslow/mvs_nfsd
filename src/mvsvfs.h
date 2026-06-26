/*
 * VFS internal declarations
 *
 * This is only used on MVS
 */

#ifndef MVSVFS_H
#define MVSVFS_H

#include "types.h"
#include "mvspdir.h"  /* pds_member_entry_t */
 
#define VFS_NODE_TYPE_VFS_DIR        1
#define VFS_NODE_TYPE_PDS_DIR        2
#define MAX_PATH_LEN                 256

typedef struct vfs_node_vfs_dir {
    uint32_t        node_num;
    uint32_t        parent_node_num;
    uint32_t        next_node_num;
    uint8_t         node_type;
    uint32_t        first_child_node_num;
    unsigned char * directory_name;
} vfs_node_vfs_dir_t;
 
typedef struct vfs_node_pds_dir {
    uint32_t        node_num;
    uint32_t        parent_node_num;
    uint32_t        next_node_num;
    uint8_t         node_type;
    uint32_t        first_child_node_num;
    unsigned char * directory_name;
    unsigned char * file_name_ext;
    unsigned char   mvs_pds_dsname[45];
    unsigned char   mvs_vol_ser[7];
} vfs_node_pds_dir_t;


/*
 * Search the open directory pool for a non-expired entry matching
 * pds_dsname, then find member_name within that cached directory.
 * Sets *member_entry on success.
 * Returns 0 on success, -1 if the pool entry or member is not found.
 */
int mvsvfs_find_cached_member(
    const char          *pds_dsname,
    const char          *member_name,
    pds_member_entry_t **member_entry);

#endif /* MVSVFS_H */
