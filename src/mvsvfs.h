/*
 * VFS internal declarations
 *
 * This is only used on MVS
 */
 
#ifndef VFS_H
#define VFS_H
 
#include "types.h"
 
#define VFS_NODE_TYPE_VFS_DIR        1
#define VFS_NODE_TYPE_PDS_DIR        2
#define MVSVFS_PDS_DIR_CACHE_SIZE    100 /* Number of PDS directory entries to cache */ 

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

#define MVS_PATH_TYPE_DATASET       1
#define MVS_PATH_TYPE_PDS_MEMBER    2

int mvs_path_type(const char *path);


#endif /* VFS_H */
