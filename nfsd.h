/*
 * nfsd.h - Minimal NFSv3 server - shared types, constants and prototypes.
 *
 * Pure C, no external library dependencies beyond standard C and POSIX
 * sockets.  Supports read, write, and create in exported directories.
 * Sub-directory creation is not supported.
 *
 * Platform: Linux x86_64 initially; designed for portability to MVS 3.8j.
 * The vfs.c module is the ONLY file that touches OS-specific filesystem
 * calls.  Replace it for MVS with a PDS/ISPF-aware implementation.
 *
 * All XDR encoding uses explicit byte manipulation so the code is
 * correct on both little-endian (Linux x86_64) and big-endian (MVS)
 * hosts without any reliance on htonl() or similar.
 */

#ifndef NFSD_H
#define NFSD_H

/*
 * Portable integer types.  If stdint.h is not available (some MVS
 * toolchains), define the types manually by uncommenting the block
 * below and commenting out the #include.
 */
#include <stdint.h>
#include <stddef.h>

/*
 * Uncomment for MVS if stdint.h is absent:
 *
 * typedef unsigned char      uint8_t;
 * typedef unsigned short     uint16_t;
 * typedef unsigned int       uint32_t;
 * typedef unsigned long long uint64_t;
 * typedef signed int         int32_t;
 */

/* ------------------------------------------------------------------ */
/* Network port numbers                                                 */
/* ------------------------------------------------------------------ */
#define PORT_PORTMAP     111
#define PORT_MOUNT       20048
#define PORT_NFS         2049

/* ------------------------------------------------------------------ */
/* RPC message type constants (RFC 5531)                                */
/* ------------------------------------------------------------------ */
#define RPC_CALL             0u
#define RPC_REPLY            1u
#define MSG_ACCEPTED         0u
#define MSG_DENIED           1u
#define RPC_SUCCESS          0u
#define PROG_UNAVAIL         1u
#define PROG_MISMATCH        2u
#define PROC_UNAVAIL         3u
#define GARBAGE_ARGS         4u
#define AUTH_FLAVOR_NULL     0u
#define AUTH_FLAVOR_UNIX     1u

/* ------------------------------------------------------------------ */
/* RPC program numbers and versions                                     */
/* ------------------------------------------------------------------ */
#define PROG_PORTMAP         100000u
#define PROG_NFS             100003u
#define PROG_MOUNT           100005u
#define VERS_PORTMAP         2u
#define VERS_NFS             3u
#define VERS_MOUNT           3u

/* ------------------------------------------------------------------ */
/* Portmapper procedure numbers                                         */
/* ------------------------------------------------------------------ */
#define PMAPPROC_NULL        0u
#define PMAPPROC_SET         1u
#define PMAPPROC_UNSET       2u
#define PMAPPROC_GETPORT     3u
#define PMAPPROC_DUMP        4u

/* Portmapper transport protocol codes */
#define IPPROTO_TCP_PMAP     6u
#define IPPROTO_UDP_PMAP     17u

/* ------------------------------------------------------------------ */
/* Mount protocol procedure numbers                                     */
/* ------------------------------------------------------------------ */
#define MOUNTPROC3_NULL      0u
#define MOUNTPROC3_MNT       1u
#define MOUNTPROC3_DUMP      2u
#define MOUNTPROC3_UMNT      3u
#define MOUNTPROC3_UMNTALL   4u
#define MOUNTPROC3_EXPORT    5u

/* ------------------------------------------------------------------ */
/* NFSv3 procedure numbers (RFC 1813)                                   */
/* ------------------------------------------------------------------ */
#define NFS3PROC_NULL         0u
#define NFS3PROC_GETATTR      1u
#define NFS3PROC_SETATTR      2u
#define NFS3PROC_LOOKUP       3u
#define NFS3PROC_ACCESS       4u
#define NFS3PROC_READLINK     5u
#define NFS3PROC_READ         6u
#define NFS3PROC_WRITE        7u
#define NFS3PROC_CREATE       8u
#define NFS3PROC_MKDIR        9u
#define NFS3PROC_SYMLINK      10u
#define NFS3PROC_MKNOD        11u
#define NFS3PROC_REMOVE       12u
#define NFS3PROC_RMDIR        13u
#define NFS3PROC_RENAME       14u
#define NFS3PROC_LINK         15u
#define NFS3PROC_READDIR      16u
#define NFS3PROC_READDIRPLUS  17u
#define NFS3PROC_FSSTAT       18u
#define NFS3PROC_FSINFO       19u
#define NFS3PROC_PATHCONF     20u
#define NFS3PROC_COMMIT       21u

/* ------------------------------------------------------------------ */
/* NFSv3 status codes                                                   */
/* ------------------------------------------------------------------ */
#define NFS3_OK               0u
#define NFS3ERR_PERM          1u
#define NFS3ERR_NOENT         2u
#define NFS3ERR_IO            5u
#define NFS3ERR_NXIO          6u
#define NFS3ERR_ACCES         13u
#define NFS3ERR_EXIST         17u
#define NFS3ERR_XDEV          18u
#define NFS3ERR_NODEV         19u
#define NFS3ERR_NOTDIR        20u
#define NFS3ERR_ISDIR         21u
#define NFS3ERR_INVAL         22u
#define NFS3ERR_FBIG          27u
#define NFS3ERR_NOSPC         28u
#define NFS3ERR_ROFS          30u
#define NFS3ERR_MLINK         31u
#define NFS3ERR_NAMETOOLONG   63u
#define NFS3ERR_NOTEMPTY      66u
#define NFS3ERR_DQUOT         69u
#define NFS3ERR_STALE         70u
#define NFS3ERR_REMOTE        71u
#define NFS3ERR_BADHANDLE     10001u
#define NFS3ERR_NOT_SYNC      10002u
#define NFS3ERR_BAD_COOKIE    10003u
#define NFS3ERR_NOTSUPP       10004u
#define NFS3ERR_TOOSMALL      10005u
#define NFS3ERR_SERVERFAULT   10006u
#define NFS3ERR_BADTYPE       10007u
#define NFS3ERR_JUKEBOX       10008u

/* ------------------------------------------------------------------ */
/* NFS file types                                                       */
/* ------------------------------------------------------------------ */
#define NF3REG    1u   /* regular file */
#define NF3DIR    2u   /* directory    */
#define NF3BLK    3u   /* block device */
#define NF3CHR    4u   /* char device  */
#define NF3LNK    5u   /* symlink      */
#define NF3SOCK   6u   /* socket       */
#define NF3FIFO   7u   /* named pipe   */

/* ------------------------------------------------------------------ */
/* ACCESS3 permission bits                                              */
/* ------------------------------------------------------------------ */
#define ACCESS3_READ     0x0001u
#define ACCESS3_LOOKUP   0x0002u
#define ACCESS3_MODIFY   0x0004u
#define ACCESS3_EXTEND   0x0008u
#define ACCESS3_DELETE   0x0010u
#define ACCESS3_EXECUTE  0x0020u

/* ------------------------------------------------------------------ */
/* WRITE stable_how values                                              */
/* ------------------------------------------------------------------ */
#define UNSTABLE     0u
#define DATA_SYNC    1u
#define FILE_SYNC    2u

/* ------------------------------------------------------------------ */
/* FSINFO property flags                                                */
/* ------------------------------------------------------------------ */
#define FSF3_LINK        0x0001u
#define FSF3_SYMLINK     0x0002u
#define FSF3_HOMOGENEOUS 0x0008u
#define FSF3_CANSETTIME  0x0010u

/* ------------------------------------------------------------------ */
/* CREATE mode values                                                   */
/* ------------------------------------------------------------------ */
#define CREATE_UNCHECKED  0u
#define CREATE_GUARDED    1u
#define CREATE_EXCLUSIVE  2u

/* sattr3 set-time mode values */
#define SET_DONT_CHANGE      0u
#define SET_TO_SERVER_TIME   1u
#define SET_TO_CLIENT_TIME   2u

/* ------------------------------------------------------------------ */
/* Mount protocol error codes                                           */
/* ------------------------------------------------------------------ */
#define MNT3_OK              0u
#define MNT3ERR_PERM         1u
#define MNT3ERR_NOENT        2u
#define MNT3ERR_IO           5u
#define MNT3ERR_ACCES        13u
#define MNT3ERR_NOTDIR       20u
#define MNT3ERR_INVAL        22u
#define MNT3ERR_NAMETOOLONG  63u
#define MNT3ERR_NOTSUPP      10004u
#define MNT3ERR_SERVERFAULT  10006u

/* ------------------------------------------------------------------ */
/* Size and buffer limits                                               */
/* ------------------------------------------------------------------ */

/* Maximum NFS3 file handle size (wire format) */
#define NFS3_FHSIZE       64

/* Our file handle is 16 bytes: magic(4) + export_id(4) + dev(4) + ino(4) */
#define OUR_FHSIZE        16
#define OUR_FH_MAGIC      0x4E465333u  /* 'NFS3' */

#define MAX_EXPORTS       16
#define MAX_PATH          256
#define MAX_NAME          256
#define FH_CACHE_SIZE     512
#define MAX_CONNECTIONS   16

/*
 * 128 KB buffers: large enough for max read/write (64 KB) plus headers.
 * On MVS, reduce if memory is constrained.
 */
#define BUF_SIZE          (128 * 1024)
#define MAX_READ_SIZE     (64  * 1024)
#define MAX_WRITE_SIZE    (64  * 1024)

/* Maximum open directory handles at once */
#define MAX_OPEN_DIRS     8

/* ------------------------------------------------------------------ */
/* Core data structures                                                 */
/* ------------------------------------------------------------------ */

/*
 * Our file handle (16 bytes, fits within NFS3 64-byte limit).
 *
 * All four fields are stored big-endian in the wire encoding regardless
 * of host endianness (see fh_encode / fh_decode in fhandle.c).
 *
 * For the MVS port:
 *   export_id  -> PDS dataset index in the exports table
 *   dev        -> could map to volume serial or dataset index
 *   ino        -> member index or hash within the PDS
 */
typedef struct {
    uint32_t magic;      /* always OUR_FH_MAGIC */
    uint32_t export_id;  /* index into exports table */
    uint32_t dev;        /* st_dev truncated to 32 bits */
    uint32_t ino;        /* st_ino truncated to 32 bits */
} our_fhandle_t;

/* One exported directory */
typedef struct {
    char export_path[MAX_PATH]; /* NFS path as seen by clients: /export/foo */
    char host_path[MAX_PATH];   /* local path on this host: /home/user/foo  */
} export_t;

/* Entry in the file-handle path cache */
typedef struct {
    int      valid;
    uint32_t export_id;
    uint32_t dev;
    uint32_t ino;
    char     relpath[MAX_PATH]; /* path relative to export root (no leading /) */
} fh_cache_entry_t;

/* XDR encode/decode buffer */
typedef struct {
    uint8_t  *base;
    uint32_t  capacity;
    uint32_t  pos;
    int       error;  /* non-zero if any encode/decode error has occurred */
} xdr_t;

/* Parsed fields from an incoming RPC CALL message */
typedef struct {
    uint32_t xid;
    uint32_t prog;
    uint32_t vers;
    uint32_t proc;
    uint32_t auth_uid;  /* from AUTH_UNIX credentials, 0 otherwise */
    uint32_t auth_gid;
} rpc_call_t;

/* One accepted TCP connection */
typedef struct {
    int fd;
    int proto;  /* PROTO_PORTMAP, PROTO_MOUNT, or PROTO_NFS */
} conn_t;

#define PROTO_PORTMAP  0
#define PROTO_MOUNT    1
#define PROTO_NFS      2

/*
 * VFS stat: an OS-independent view of file metadata.
 *
 * Populated by vfs_stat() from struct stat on Linux.
 * For the MVS port, populate from DSCB / ISPF statistics.
 */
typedef struct {
    uint32_t ftype;        /* NF3REG, NF3DIR, NF3LNK, etc.     */
    uint32_t mode;         /* permission bits (lower 12 bits)   */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;         /* file size in bytes                */
    uint64_t used;         /* disk space consumed in bytes      */
    uint32_t rdev_maj;
    uint32_t rdev_min;
    uint64_t fsid;
    uint64_t fileid;       /* inode number (NFS fileid)         */
    uint32_t atime_sec;
    uint32_t atime_nsec;
    uint32_t mtime_sec;
    uint32_t mtime_nsec;
    uint32_t ctime_sec;
    uint32_t ctime_nsec;
    uint32_t raw_dev;      /* for file handle: st_dev as uint32 */
    uint32_t raw_ino;      /* for file handle: st_ino as uint32 */
} vfs_stat_t;

/* VFS filesystem-level statistics */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t avail_bytes;
    uint64_t total_files;
    uint64_t free_files;
    uint64_t avail_files;
    uint32_t invarsec;     /* seconds until stats may change    */
} vfs_fsstat_t;

/* Decoded sattr3: attributes to set on a file (from SETATTR or CREATE) */
typedef struct {
    int      has_mode;    uint32_t mode;
    int      has_uid;     uint32_t uid;
    int      has_gid;     uint32_t gid;
    int      has_size;    uint64_t size;
    int      set_atime;   uint32_t atime_sec;   /* SET_TO_CLIENT_TIME */
    int      set_mtime;   uint32_t mtime_sec;   /* SET_TO_CLIENT_TIME */
} sattr3_t;

/* Opaque directory iterator handle -- defined fully in vfs.c */
typedef struct vfs_dir vfs_dir_t;

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

/*
 * 8-byte write verifier: set once at startup from time().
 * Returned in WRITE replies and checked by COMMIT.
 * Declared extern here; defined in nfsd.c.
 */
extern uint8_t g_write_verifier[8];

/* Actual listening ports (set in main(); used by portmapper) */
extern int g_port_pmap;
extern int g_port_mount;
extern int g_port_nfs;

/* Verbose logging flag: set by -v flag */
extern int g_verbose;

/* ------------------------------------------------------------------ */
/* Prototypes: xdr.c                                                    */
/* ------------------------------------------------------------------ */
void     xdr_init_read(xdr_t *x, uint8_t *buf, uint32_t len);
void     xdr_init_write(xdr_t *x, uint8_t *buf, uint32_t cap);
uint32_t xdr_read_uint32(xdr_t *x);
uint64_t xdr_read_uint64(xdr_t *x);
void     xdr_read_raw(xdr_t *x, uint8_t *dst, uint32_t len);
void     xdr_read_opaque(xdr_t *x, uint8_t *dst,
             uint32_t *dstlen, uint32_t maxlen);
int      xdr_read_string(xdr_t *x, char *dst, uint32_t maxlen);
void     xdr_skip(xdr_t *x, uint32_t nbytes);
void     xdr_write_uint32(xdr_t *x, uint32_t v);
void     xdr_write_uint64(xdr_t *x, uint64_t v);
void     xdr_write_raw(xdr_t *x, const uint8_t *src, uint32_t len);
void     xdr_write_opaque(xdr_t *x, const uint8_t *src, uint32_t len);
void     xdr_write_string(xdr_t *x, const char *s, uint32_t len);
void     xdr_write_fhandle(xdr_t *x, const our_fhandle_t *fh);
uint32_t xdr_get_pos(const xdr_t *x);
void     xdr_set_pos(xdr_t *x, uint32_t pos);

/* ------------------------------------------------------------------ */
/* Prototypes: rpc.c                                                    */
/* ------------------------------------------------------------------ */
int  rpc_recv(int fd, uint8_t *buf, uint32_t maxlen, uint32_t *msglen);
int  rpc_send(int fd, const uint8_t *buf, uint32_t len);
int  rpc_parse_call(xdr_t *x, rpc_call_t *call);
void rpc_write_accept_hdr(xdr_t *x, uint32_t xid, uint32_t accept_stat);
void rpc_write_prog_mismatch(xdr_t *x, uint32_t xid,
         uint32_t lo, uint32_t hi);
void rpc_write_proc_unavail(xdr_t *x, uint32_t xid);

/* ------------------------------------------------------------------ */
/* Prototypes: exports.c                                                */
/* ------------------------------------------------------------------ */
int       exports_load(const char *config_file);
int       exports_count(void);
export_t *exports_get(int idx);
int       exports_get_id(const export_t *exp);
export_t *exports_find_by_nfs_path(const char *nfs_path);
export_t *exports_find_by_id(uint32_t id);

/* ------------------------------------------------------------------ */
/* Prototypes: fhandle.c                                                */
/* ------------------------------------------------------------------ */
void fh_init(void);
void fh_make(our_fhandle_t *fh, uint32_t export_id,
             uint32_t dev, uint32_t ino);
int  fh_decode(const uint8_t *bytes, uint32_t len, our_fhandle_t *fh);
void fh_encode(const our_fhandle_t *fh, uint8_t *bytes);
void fh_cache_insert(uint32_t export_id, uint32_t dev,
                     uint32_t ino, const char *relpath);
int  fh_cache_lookup(uint32_t export_id, uint32_t dev,
                     uint32_t ino, char *relpath, uint32_t maxlen);
int  fh_resolve(const our_fhandle_t *fh, char *abspath, uint32_t maxlen);

/* ------------------------------------------------------------------ */
/* Prototypes: vfs.c                                                    */
/* ------------------------------------------------------------------ */
int        vfs_stat(const char *path, vfs_stat_t *st);
int        vfs_pread(const char *path, void *buf, uint32_t count,
               uint64_t offset, uint32_t *nread, int *eof);
int        vfs_pwrite(const char *path, const void *buf,
               uint32_t count, uint64_t offset);
int        vfs_create(const char *path, uint32_t mode);
int        vfs_remove(const char *path);
int        vfs_rename(const char *from, const char *to);
int        vfs_truncate(const char *path, uint64_t size);
int        vfs_set_times(const char *path,
               int set_atime, uint32_t atime_sec,
               int set_mtime, uint32_t mtime_sec);
int        vfs_fsstat(const char *path, vfs_fsstat_t *fs);
uint32_t   vfs_errno_to_nfs3(int err);
vfs_dir_t *vfs_opendir(const char *path);
int        vfs_readdir_next(vfs_dir_t *d, char *name,
               uint32_t maxname, uint64_t *fileid, uint64_t *cookie);
void       vfs_seekdir_to(vfs_dir_t *d, uint64_t cookie);
void       vfs_closedir(vfs_dir_t *d);

/* ------------------------------------------------------------------ */
/* Prototypes: portmap.c                                                */
/* ------------------------------------------------------------------ */
void handle_portmap(int fd, rpc_call_t *call, xdr_t *in, xdr_t *out);

/* ------------------------------------------------------------------ */
/* Prototypes: mount3.c                                                 */
/* ------------------------------------------------------------------ */
void handle_mount(int fd, rpc_call_t *call, xdr_t *in, xdr_t *out);

/* ------------------------------------------------------------------ */
/* Prototypes: nfs3.c                                                   */
/* ------------------------------------------------------------------ */
void handle_nfs3(int fd, rpc_call_t *call, xdr_t *in, xdr_t *out);
void xdr_write_fattr3(xdr_t *x, const vfs_stat_t *st);
void xdr_write_post_op_attr(xdr_t *x, const vfs_stat_t *st, int present);
void xdr_write_wcc_data(xdr_t *x,
         const vfs_stat_t *pre, int has_pre,
         const vfs_stat_t *post, int has_post);
void xdr_read_fhandle3(xdr_t *x, our_fhandle_t *fh, int *ok);
void xdr_read_sattr3(xdr_t *x, sattr3_t *a);

#endif /* NFSD_H */
