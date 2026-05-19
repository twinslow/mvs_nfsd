# MVS support for the VFS module

This module contains functions to access MVS resources, such as PDS directories and dataset attributes (DSORG, RECFM, LRECL, BLKSIZE etc).

These functions return MVS type information. It is the MVS VFS implementation that provides the 
translation from MVS constructs to a hierarchical file system (un*x) paradigm.

# VFS Routines

## vfs_stat

This will be called for both a "directory" and "files". 

# MVS Support Routines

* mvs_get_dsinfo - Get dataset information, such as volume serial number, LRECL,
  BLKSIZE, DSORG, RECFM.
* mvs_get_pdsdir - Get PDS member list information (member name and ISPF stats)
* mvs_read - Read a block from a PDS member
* mvs_write - Write a block to a PDS member

## mvs_get_dsinfo

```c
typedef struct {
    unsigned char           volser[7];
    unsigned char           dsorg;
    short int               lrecl;
    short int               blksize;
    unsigned char           recfm;           
} mvs_dsinfo_t;

mvs_dsinfo_t *mvs_get_dsinfo(
    unsigned char *dsname,
    unsigned char *volser
);
```

The `mvs_get_dsinfo` function will return NULL if the dataset is not found. The 
volser maybe specified as NULL, in which case the dataset will be located via a
catalog entry.

## mvs_get_pdsdir

```c
typedef struct {
    unsigned char           member_name[9];
    int                     line_count;
    short int               version;
    short int               level;

} mvs_dir_entry_t;

mvs_dir_entry_t[] mvs_get_pdsdir(
    unsigned char          *dsname,
    unsigned char          *volser,
    unsigned char          *after_member_name,
    int                     max_member_count
);
```
## mvs_read

## mvs_write


## Calling assembler functions from JCC compiled code






