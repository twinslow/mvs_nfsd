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
#include "types.h"
#include "mvsio.h"

#include <errno.h>
/* -------------------------------------------------------------------- */
/* errno compatibility                                                   */
/*                                                                       */
/* JCC's <errno.h> follows standard POSIX errno numbering but OMITS      */
/* several values, leaving their slots unused -- it defines 17 EEXIST    */
/* and then jumps to 20 ENOTDIR, and 28 ENOSPC / 29 ESPIPE then jumps to */
/* 33 EDOM.  So the canonical numbers for the ones we need are free and  */
/* can simply be supplied.                                               */
/*                                                                       */
/* The #ifndef defers to the platform (POSIX defines these already), and */
/* because JCC follows POSIX numbering it would add them at these very   */
/* values anyway -- so this cannot collide, now or later.                */
/*                                                                       */
/* Do NOT replace this with an "#ifdef EXDEV ... #else errno = EINVAL"   */
/* fallback: on JCC that silently always takes the fallback, and the     */
/* client then receives NFS3ERR_INVAL, which Windows renders as the      */
/* famously misleading "The volume for a file has been externally        */
/* altered".  For the same reason the errno -> NFS3 mapper (nfserr.c)    */
/* guards no case: every value it maps is defined here, so the mapping   */
/* is identical on every platform and cannot silently lose an entry.     */
/*                                                                       */
/* Defined in this shared header rather than per-module so the value a   */
/* backend SETS and the value the mapper TRANSLATES can never disagree.  */
/* -------------------------------------------------------------------- */
#ifndef EPERM
#define EPERM    1    /* Operation not permitted    -> NFS3ERR_PERM     */
#endif
#ifndef ENXIO
#define ENXIO    6    /* No such device or address  -> NFS3ERR_NXIO     */
#endif
#ifndef EXDEV
#define EXDEV   18    /* Cross-device link / rename -> NFS3ERR_XDEV     */
#endif
#ifndef ENODEV
#define ENODEV  19    /* No such device             -> NFS3ERR_NODEV    */
#endif
#ifndef EFBIG
#define EFBIG   27    /* File too large             -> NFS3ERR_FBIG     */
#endif
#ifndef EROFS
#define EROFS   30    /* Read-only file system      -> NFS3ERR_ROFS     */
#endif
#ifndef EMLINK
#define EMLINK  31    /* Too many links             -> NFS3ERR_MLINK    */
#endif

/*
 * For MVS, we use short aliases for all functions and globals to
 * avoid name mangling issues with the C runtime. The aliases are defined
 * in the block below.  On Linux, we use the full names.
 */
#if defined(__MVS__)
/* xdr.c */
#define xdr_init_read           xdrInRd
#define xdr_init_write          xdrInWr
#define xdr_read_uint32         xdrRdU32
#define xdr_read_uint64         xdrRdU64
#define xdr_read_raw            xdrRdRaw
#define xdr_read_opaque         xdrRdOpa
#define xdr_read_string         xdrRdStr
#define xdr_read_fhandle3       xdrRdFh3
#define xdr_read_sattr3         xdrRdSa3
#define xdr_write_uint32        xdrWrU32
#define xdr_write_uint64        xdrWrU64
#define xdr_write_raw           xdrWrRaw
#define xdr_write_opaque        xdrWrOpa
#define xdr_write_string        xdrWrStr
#define xdr_write_fhandle       xdrWrFh
#define xdr_write_fattr3        xdrWrFa3
#define xdr_write_post_op_attr  xdrWrPoa
#define xdr_write_wcc_data      xdrWrWcc
#define xdr_get_pos             xdrGetPo
#define xdr_set_pos             xdrSetPo
/* rpc.c */
#define rpc_parse_call          rpcParsC
#define rpc_write_accept_hdr    rpcWrAcc
#define rpc_write_prog_mismatch rpcWrPgM
#define rpc_write_proc_unavail  rpcWrPcU
/* exports.c */
#define exports_load            expLoad
#define exports_count           expCount
#define exports_get             expGet
#define exports_get_id          expGetId
#define exports_find_by_nfs_path expFndNp
#define exports_find_by_id      expFndId
#define export_dataset_count            expDsCnt
#define export_dataset_get              expDsGet
#define export_dataset_find_by_dirname  expDsFnd
#define export_dataset_touch            expDsTch
/* fhandle.c */
#define fh_from_path            fhFromPa
#define fh_resolve              fhResolv
/* vfs.c */
#define vfs_pread               vfsPread
#define vfs_pwrite              vfsPwrit
#define vfs_create              vfsCreat
#define vfs_remove              vfsRemov
#define vfs_rename              vfsRenam
#define vfs_truncate            vfsTrunc
#define vfs_set_times           vfsSetTm
#define vfs_fsstat              vfsFsSt
#define vfs_opendir             vfsOpDir
#define vfs_readdir_next        vfsRdNxt
#define vfs_seekdir_to          vfsSekTo
#define vfs_closedir            vfsClDir
#define vfs_commit              vfsCommt
/* nfserr.c */
#define vfs_errno_to_nfs3       vfsErrN3
/* handlers */
#define handle_portmap          hndPmap
#define handle_mount            hndMount
#define handle_nfs3             hndNfs3
/* globals */
#define g_write_verifier        gWrVerif
#define g_port_pmap             gPortPm
#define g_port_mount            gPortMt
#define g_port_nfs              gPortNfs
#define g_verbose               gVerbose
#endif /* __MVS__ */

/*
 * Uncomment for MVS if stdint.h is absent:
 *
 * typedef unsigned char      uint8_t;
 * typedef unsigned short     uint16_t;
 * typedef unsigned int       uint32_t;
 * typedef unsigned long long uint64_t;
 * typedef signed int         int32_t;
 */

/* -------------------------------------------------------------------- */
/* Network port numbers                                                 */
/* -------------------------------------------------------------------- */
#define PORT_PORTMAP     111
#define PORT_MOUNT       20048
#define PORT_NFS         2049

/* -------------------------------------------------------------------- */
/* RPC message type constants (RFC 5531)                                */
/* -------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------- */
/* RPC program numbers and versions                                     */
/* -------------------------------------------------------------------- */
#define PROG_PORTMAP         100000u
#define PROG_NFS             100003u
#define PROG_MOUNT           100005u
#define VERS_PORTMAP         2u
#define VERS_NFS             3u
#define VERS_MOUNT           3u

/* -------------------------------------------------------------------- */
/* Portmapper procedure numbers                                         */
/* -------------------------------------------------------------------- */
#define PMAPPROC_NULL        0u
#define PMAPPROC_SET         1u
#define PMAPPROC_UNSET       2u
#define PMAPPROC_GETPORT     3u
#define PMAPPROC_DUMP        4u

/* Portmapper transport protocol codes */
#define IPPROTO_TCP_PMAP     6u
#define IPPROTO_UDP_PMAP     17u

/* -------------------------------------------------------------------- */
/* Mount protocol procedure numbers                                     */
/* -------------------------------------------------------------------- */
#define MOUNTPROC3_NULL      0u
#define MOUNTPROC3_MNT       1u
#define MOUNTPROC3_DUMP      2u
#define MOUNTPROC3_UMNT      3u
#define MOUNTPROC3_UMNTALL   4u
#define MOUNTPROC3_EXPORT    5u

/* -------------------------------------------------------------------- */
/* NFSv3 procedure numbers (RFC 1813)                                   */
/* -------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------- */
/* NFSv3 status codes                                                   */
/* -------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------- */
/* NFS file types                                                       */
/* -------------------------------------------------------------------- */
#define NF3REG    1u   /* regular file */
#define NF3DIR    2u   /* directory    */
#define NF3BLK    3u   /* block device */
#define NF3CHR    4u   /* char device  */
#define NF3LNK    5u   /* symlink      */
#define NF3SOCK   6u   /* socket       */
#define NF3FIFO   7u   /* named pipe   */

/* -------------------------------------------------------------------- */
/* ACCESS3 permission bits                                              */
/* -------------------------------------------------------------------- */
#define ACCESS3_READ     0x0001u
#define ACCESS3_LOOKUP   0x0002u
#define ACCESS3_MODIFY   0x0004u
#define ACCESS3_EXTEND   0x0008u
#define ACCESS3_DELETE   0x0010u
#define ACCESS3_EXECUTE  0x0020u

/* -------------------------------------------------------------------- */
/* WRITE stable_how values                                              */
/* -------------------------------------------------------------------- */
#define UNSTABLE     0u
#define DATA_SYNC    1u
#define FILE_SYNC    2u

/* -------------------------------------------------------------------- */
/* FSINFO property flags                                                */
/* -------------------------------------------------------------------- */
#define FSF3_LINK        0x0001u
#define FSF3_SYMLINK     0x0002u
#define FSF3_HOMOGENEOUS 0x0008u
#define FSF3_CANSETTIME  0x0010u

/* -------------------------------------------------------------------- */
/* CREATE mode values                                                   */
/* -------------------------------------------------------------------- */
#define CREATE_UNCHECKED  0u
#define CREATE_GUARDED    1u
#define CREATE_EXCLUSIVE  2u

/* sattr3 set-time mode values */
#define SET_DONT_CHANGE      0u
#define SET_TO_SERVER_TIME   1u
#define SET_TO_CLIENT_TIME   2u

/* -------------------------------------------------------------------- */
/* Mount protocol error codes                                           */
/* -------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------- */
/* Size and buffer limits                                               */
/* -------------------------------------------------------------------- */

/* Maximum NFS3 file handle size (wire format) */
#define NFS3_FHSIZE       64

/*
 * Our file handle is 60 bytes and SELF-DESCRIBING -- it carries the object's
 * name, so it needs no server-side state and stays valid across restarts:
 *
 *   magic(4) + export_id(4) + dsname(44, ASCII) + member(8, ASCII)
 *
 * 60 <= NFS3_FHSIZE (64) and is 4-byte aligned, so XDR adds no padding.
 * See doc/readme_filehandles.md.
 */
#define OUR_FHSIZE        60
#define OUR_FH_MAGIC      0x4E465333u  /* 'NFS3' */

/* Wire field widths inside the handle (ASCII, blank-padded). */
#define FH_DSNAME_LEN     44    /* max MVS dsname                          */
#define FH_MEMBER_LEN      8    /* max PDS member name                     */
#define FH_PAD_CHAR       0x20  /* ASCII blank -- NOT ' ' (EBCDIC 0x40!)   */

#define MAX_EXPORTS       16
#define MAX_PDS_PER_EXPORT 32   /* max PDS datasets grouped under one export */
#define MAX_PATH          256
#define MAX_NAME          256
#define MAX_FILE_EXT_LEN  16
#define MAX_DSNAME_LEN    45    /* 44-char MVS dsname + NUL */
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

/* -------------------------------------------------------------------- */
/* Core data structures                                                 */
/* -------------------------------------------------------------------- */

/*
 * Our file handle (60 bytes, within the NFS3 64-byte limit).
 *
 * The handle NAMES its object rather than referencing server-side state,
 * so it survives a server restart and never goes stale for bookkeeping
 * reasons.  RFC 1813 requires a handle to stay valid for the lifetime of
 * the object it refers to; NFS3ERR_STALE means the OBJECT is gone, not
 * that the server forgot about it.
 *
 * The three object kinds are distinguished by which name fields are set:
 *
 *   export root  : dsname == "",  member == ""   (a virtual directory)
 *   PDS directory: dsname set,    member == ""
 *   PDS member   : dsname set,    member set
 *
 * 'export_id' is a STABLE HASH of the export path (mvs_fid_ino32), not a
 * table index -- an index would silently resolve to the wrong export if
 * the config were reordered.  If no export matches the hash the handle is
 * genuinely stale.
 *
 * dsname / member are held in ASCII (the wire form too), so a handle is
 * readable in a packet trace.  The numeric fields are big-endian on the
 * wire regardless of host endianness.  See fh_encode / fh_decode and
 * doc/readme_filehandles.md.
 */
typedef struct {
    uint32_t magic;                  /* always OUR_FH_MAGIC                 */
    uint32_t export_id;              /* stable hash of the export path      */
    char     dsname[MAX_DSNAME_LEN]; /* ASCII, upper-case; "" = export root */
    char     member[FH_MEMBER_LEN + 1]; /* ASCII, upper-case; "" = PDS dir  */
} our_fhandle_t;

/*
 * A dataset's VTOC information in a form C code can use directly.
 *
 * The counterpart of mvs_dscb_info_t (src/asmutils.h), which mirrors the
 * DSCB byte for byte and therefore carries raw big-endian byte pairs and
 * blank-padded names.  This is the decoded view: real integers, terminated
 * strings, and the one derived value the space prediction needs on every
 * call (blocks_per_track).  Filled in by blkcalc_dataset_init().
 *
 * Not prefixed 'pds_' deliberately -- sequential datasets may be exported
 * later and would use the same structure.
 *
 * EVERY FIELD IS A CONFIG-TIME CONSTANT EXCEPT lstar_tt / lstar_r / trbal,
 * which move whenever any task writes a member of this dataset and are
 * re-read on every admit decision.  See doc/design_pds_full_prediction.md.
 */
typedef struct {
    uint8_t   valid;             /* 0 = the DSCB could not be read        */
    /* DSCB bytes -- test with MVS_DSCB_DSORG_* / MVS_DSCB_RECFM_*, NOT
       with the MVS_DCB_* values in mvsio.h, which use the opposite bits
       for DSORG.  See the warning in asmutils.h. */
    uint8_t   dsorg;             /* first byte of DS1DSORG                */
    uint8_t   recfm;             /* DS1RECFM                              */
    uint8_t   nextents;          /* extents currently held on the volume  */
    uint16_t  blksize;
    uint16_t  lrecl;
    char      volser[7];         /* NUL terminated, unlike the DSCB form  */
    uint8_t   devcode;           /* devtype[3]: 0x0F=3390, 0x0E=3380 ...  */
    uint32_t  trklen;            /* bytes per track (DS4DEVTK)            */
    uint32_t  trkcyl;            /* tracks per cylinder (DS4DSTRK)        */
    uint32_t  tracks;            /* tracks allocated over all extents     */
    uint32_t  sec_qty;           /* secondary quantity, in sec_flags units*/
    uint32_t  sec_tracks;        /* the same in tracks, 0 if unconvertible*/
    uint8_t   sec_flags;         /* DS1SCAL1                              */
    int       blocks_per_track;  /* derived; 0 = geometry unusable        */
    uint32_t  create_date;       /* yyyyddd, 0 = not recorded             */
    uint32_t  ref_date;          /* yyyyddd, 0 = not recorded             */

    /* --- volatile: re-read from the VTOC on every admit decision --- */
    uint32_t  lstar_tt;          /* DS1LSTAR relative track               */
    uint8_t   lstar_r;           /* DS1LSTAR record within that track     */
    uint16_t  trbal;             /* bytes still free on the lstar track   */
} dataset_dscb_info_t;

/*
 * One PDS dataset within an export.  Each dataset appears to NFS clients
 * as a directory (named 'dirname', the lower-case form of the dsname)
 * under the export root; its members appear as files inside it.
 */
typedef struct {
    char           dsname_ebcdic[MAX_DSNAME_LEN];  /* real PDS name, EBCDIC (TEMP.TESTPROJ.JCLLIB) */
    char           dsname_ascii[MAX_DSNAME_LEN];   /* real PDS name, ASCII                         */
    char           dirname_ebcdic[MAX_DSNAME_LEN]; /* lower-case(dsname), EBCDIC (path matching)   */
    char           dirname_ascii[MAX_DSNAME_LEN];  /* lower-case(dsname), ASCII  (readdir output)  */
    char           file_ext[MAX_FILE_EXT_LEN];     /* extension appended to member file names      */

    /*
     * VTOC view of the same dataset, loaded once at config time by
     * cfg_load_dscb_info() and used by the write-space prediction.
     */
    dataset_dscb_info_t dscb;

    uint32_t       dir_mtime;   /* dir last-modified (epoch secs); bumped on STOW; 0 = unset */

    /*
     * Out-of-band change detection (see vfs_stat_dataset).  Members added,
     * removed, or replaced directly on MVS (IEBGENER, ISPF, ...) do not go
     * through the NFS write path, so they never bump dir_mtime.  On a
     * throttled schedule vfs_stat_dataset reads the PDS directory, folds it
     * into dir_sig, and bumps dir_mtime when the signature changes so
     * clients re-read the listing.
     */
    uint32_t       dir_sig;        /* last directory signature (0 = none yet)     */
    uint32_t       dir_sig_check;  /* epoch secs of last signature check (0=never) */

    /*
     * Per-dataset export options (config keywords; see
     * doc/design_export_options.md).  These are the RESOLVED values: any
     * export-level defaults have already been folded in at config-load time,
     * so the VFS reads only these and never walks back to the export_t.
     *
     * Defaults (assigned explicitly in dataset_init AFTER its memset -- 0 is a
     * valid but catastrophic 0000 mode):
     *   readonly = 0, dirperm = 0777, memperm = 0777.
     */
    uint8_t        readonly;   /* 1 = ro: every mutating op fails            */
    uint16_t       dirperm;    /* mode reported for the PDS directory        */
    uint16_t       memperm;    /* mode reported for its members              */
} pds_dataset_t;

/* One exported directory (may group several PDS datasets) */
typedef struct {
    char export_path[MAX_PATH]; /* NFS path as seen by clients: /export/foo */
    char export_path_ebcdic[MAX_PATH]; /* NFS path as seen by clients, but in EBCDIC: /export/foo */

    /*
     * Legacy single-dataset fields.  Retained so the (compile-only, not
     * linked) mockvfs.c and the non-MVS dev build keep building; the MVS
     * VFS uses the datasets[] list below.  Populated from datasets[0].
     */
    char host_path[MAX_PATH];   /* local path on this host, but in ASCII: /home/user/foo or MVS PDS dataset name */
    char host_path_ebcdic[MAX_PATH];   /* local path on this host: /home/user/foo or MVS PDS dataset name */
    char file_ext[MAX_FILE_EXT_LEN]; /* optional extension to add to all files in this export */

    /*
     * Export-level options (config keywords; see doc/design_export_options.md).
     *   readonly  -- a ceiling folded into every dataset's readonly at load
     *                time; kept here only for diagnostics / the load log line,
     *                not consulted per operation.
     *   rootperm  -- mode reported for the (synthetic) export root.  Default
     *                0555 -- assigned explicitly in find_or_create_export
     *                AFTER its memset.  The root is ALWAYS read-only for
     *                ACCESS regardless of this value (it cannot be modified
     *                through NFS: MKDIR/RMDIR are NOTSUPP).
     */
    uint8_t       readonly;
    uint16_t      rootperm;

    /*
     * Set during config parsing if this export hit any error (a bad keyword,
     * bad value, wrong-level keyword, or malformed block).  Such exports are
     * dropped wholesale at the end of exports_load -- a partially-applied
     * export is a worse outcome than a missing one (design §10.1).  Never
     * true after load returns.
     */
    uint8_t       failed;

    /* Multi-PDS dataset list (source of truth for the MVS 3-level model) */
    pds_dataset_t datasets[MAX_PDS_PER_EXPORT];
    int           ndatasets;
} export_t;

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

#define CONN_PROTO_TO_STR(n)  \
    (( n == 0 ) ? "portmap" :       \
     ( n == 1 ) ? "mount"   :       \
     ( n == 2 ) ? "nfs"     :       \
                  "unknown" )

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
    /*
     * raw_dev / raw_ino were the file-handle cache key.  Handles are now
     * self-describing (see fhandle.c) and no cache exists, so nothing reads
     * these any more -- they are still filled in by the VFS layers and are
     * retained only as a debugging aid.  'fileid' is the value that matters
     * (it is reported to clients in fattr3).
     */
    uint32_t raw_dev;      /* unused: former cache key */
    uint32_t raw_ino;      /* unused: former cache key */

    /*
     * Internal only -- NOT part of fattr3, never sent on the wire.  Set by
     * the vfs_stat_* helpers: 1 if this object lives on a read-only export.
     * check_access() uses it to mask the write bits AFTER the root branch,
     * so read-only is enforced even for uid 0 (see check_access / the design
     * doc §6.1).
     */
    uint32_t fs_readonly;
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
    int      set_atime;   uint32_t atime_sec;   uint32_t atime_nsec;
    int      set_mtime;   uint32_t mtime_sec;   uint32_t mtime_nsec;
} sattr3_t;

/* Opaque directory iterator handle -- defined fully in vfs.c */
typedef struct vfs_dir vfs_dir_t;

/* -------------------------------------------------------------------- */
/* Global state                                                         */
/* -------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------- */
/* Prototypes: xdr.c                                                    */
/* -------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------- */
/* Prototypes: rpc.c                                                    */
/* -------------------------------------------------------------------- */
int  rpc_recv(int fd, uint8_t *buf, uint32_t maxlen, uint32_t *msglen);
int  rpc_send(int fd, uint8_t *frame, uint32_t len);
int  rpc_parse_call(xdr_t *x, rpc_call_t *call);
void rpc_write_accept_hdr(xdr_t *x, uint32_t xid, uint32_t accept_stat);
void rpc_write_prog_mismatch(xdr_t *x, uint32_t xid,
         uint32_t lo, uint32_t hi);
void rpc_write_proc_unavail(xdr_t *x, uint32_t xid);

/* -------------------------------------------------------------------- */
/* Prototypes: exports.c                                                */
/* -------------------------------------------------------------------- */
int       exports_load(const char *config_file);
int       exports_count(void);
export_t *exports_get(int idx);
int       exports_get_id(const export_t *exp);
export_t *exports_find_by_nfs_path(const char *nfs_path);
export_t *exports_find_by_id(uint32_t id);
export_t *exports_find_by_host_path(const char *host_path_ebcdic);

/*
 * Dataset provider: the set of PDS datasets in an export.  The root
 * directory iterator uses only these three calls, so a future dynamic
 * (catalog-discovered) implementation can replace them without touching
 * the VFS/NFS layers.
 */
int            export_dataset_count(int export_idx);
pds_dataset_t *export_dataset_get(int export_idx, int dataset_idx);
int            export_dataset_find_by_dirname(int export_idx,
                                              const char *dirname_ebcdic);
/* Mark a dataset's directory as modified now (bumps its dir_mtime), so
 * clients invalidate their cached listing after a member is added/replaced. */
void           export_dataset_touch(int export_idx, int dataset_idx);

/* -------------------------------------------------------------------- */
/* Prototypes: fhandle.c                                                */
/*                                                                      */
/* Handles are self-describing, so there is no cache and no init: the   */
/* pair below is a pure, total mapping between an NFS path and a handle. */
/* -------------------------------------------------------------------- */

/* fh_from_path: build the handle naming the object at abspath (ASCII).
 * Returns 0 on success, -1 if the path is not within an export. */
int  fh_from_path(const char *abspath, our_fhandle_t *fh);

/* fh_resolve: rebuild the ASCII NFS path the handle names.
 * Returns 0 on success, -1 if the handle is genuinely stale (its export
 * or dataset is no longer exported) -- the caller maps that to
 * NFS3ERR_STALE. */
int  fh_resolve(const our_fhandle_t *fh, char *abspath, uint32_t maxlen);

/* Wire encode / decode.  fh_decode returns -1 on a bad magic or length. */
int  fh_decode(const uint8_t *bytes, uint32_t len, our_fhandle_t *fh);
void fh_encode(const our_fhandle_t *fh, uint8_t *bytes);

/* -------------------------------------------------------------------- */
/* Prototypes: vfs.c                                                    */
/* -------------------------------------------------------------------- */

void dir_openlist_init(void);

int        vfs_stat(const char *path, vfs_stat_t *st);
int        vfs_pread(const char *path, void *buf, uint32_t count,
               uint64_t offset, uint32_t *nread, int *eof);
int        vfs_pwrite(const char *path, const void *buf,
               uint32_t count, uint64_t offset);
int        vfs_commit(const char *path);
int        vfs_create(const char *path, uint32_t mode);
int        vfs_remove(const char *path);
int        vfs_rename(const char *from, const char *to);
int        vfs_truncate(const char *path, uint64_t size);
int        vfs_set_times(const char *path,
               int set_atime, uint32_t atime_sec, uint32_t atime_nsec,
               int set_mtime, uint32_t mtime_sec, uint32_t mtime_nsec);
int        vfs_fsstat(const char *path, vfs_fsstat_t *fs);
uint32_t   vfs_errno_to_nfs3(int err);
vfs_dir_t *vfs_opendir(const char *path, uint64_t cookie);
int        vfs_readdir_next(vfs_dir_t *d, char *name,
               uint32_t maxname, uint64_t *fileid, uint64_t *cookie);
void       vfs_seekdir_to(vfs_dir_t *d, uint64_t cookie);
void       vfs_closedir(vfs_dir_t *d);

/* -------------------------------------------------------------------- */
/* Prototypes: portmap.c                                                */
/* -------------------------------------------------------------------- */
void handle_portmap(int fd, rpc_call_t *call, xdr_t *in, xdr_t *out);

/* -------------------------------------------------------------------- */
/* Prototypes: mount3.c                                                 */
/* -------------------------------------------------------------------- */
void handle_mount(int fd, rpc_call_t *call, xdr_t *in, xdr_t *out);

/* -------------------------------------------------------------------- */
/* Prototypes: nfs3.c                                                   */
/* -------------------------------------------------------------------- */
void handle_nfs3(int fd, rpc_call_t *call, xdr_t *in, xdr_t *out);
void xdr_write_fattr3(xdr_t *x, const vfs_stat_t *st);
void xdr_write_post_op_attr(xdr_t *x, const vfs_stat_t *st, int present);
void xdr_write_wcc_data(xdr_t *x,
         const vfs_stat_t *pre, int has_pre,
         const vfs_stat_t *post, int has_post);
void xdr_read_fhandle3(xdr_t *x, our_fhandle_t *fh, int *ok);
void xdr_read_sattr3(xdr_t *x, sattr3_t *a);

#endif /* NFSD_H */
