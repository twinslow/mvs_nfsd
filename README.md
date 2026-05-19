# nfsd — Minimal NFSv3 Server

A minimal NFSv3 (RFC 1813) server written in pure C with no external library
dependencies beyond standard C and POSIX sockets.

## Design goals

- **No external libraries.** Only standard C socket calls.
- **Endian-agnostic.** All XDR encoding uses explicit byte manipulation,
  not `htonl()`/`ntohl()`.  Correct on both little-endian (Linux x86_64)
  and big-endian (MVS/S370) hosts.
- **Portability-first.** Every OS-specific call is isolated in `vfs.c`.
  Replace that single file for a new platform.
- **Single-threaded.** One `select()` loop, no threads, no `fork()`.
  Up to 16 concurrent TCP connections.

## Files

| File | Purpose |
|---|---|
| `nfsd.h` | All shared types, constants, and prototypes |
| `xdr.c` | XDR encode/decode — pure byte manipulation |
| `rpc.c` | RPC TCP framing, call parsing, reply headers |
| `exports.c` | Config file parser and export table |
| `fhandle.c` | File handle encoding and path cache |
| `vfs.c` | **VFS abstraction — POSIX implementation** |
| `portmap.c` | Portmapper protocol (port 111) |
| `mount3.c` | MOUNT protocol (port 20048) |
| `nfs3.c` | NFSv3 procedure implementations |
| `nfsd.c` | `main()`, `select()` loop, connection table |
| `Makefile` | Build |
| `nfsd.conf` | Sample config |
| `nfsd_test.py` | Functional test client (no root required) |

## Building

```bash
make              # debug build
make RELEASE=1    # optimised build
make clean
```

Requires: gcc, glibc headers.  No other dependencies.

## Config file

```
# nfsd.conf
# Format: <nfs-export-path>  <local-host-path>
/export/src    /home/user/src
/export/data   /home/user/data
```

Lines beginning with `#` are comments.  Blank lines are ignored.
Up to 16 exports are supported.

## Running

### Standard (root required for ports 111 and 2049)

Stop the system portmapper first:

```bash
sudo systemctl stop rpcbind   # or portmap, depending on distro
sudo ./nfsd nfsd.conf
```

Mount from a client:

```bash
# Via portmapper (automatic port discovery):
sudo mount -t nfs -o nfsvers=3,nolock server:/export/src /mnt/src

# With explicit ports (bypasses portmapper):
sudo mount -t nfs \
     -o nfsvers=3,port=2049,mountport=20048,nolock \
     server:/export/src /mnt/src


# The following command worked to mount the file system on Ubuntu 24.04.4 LTS
sudo mount -t nfs \
     -o nfsvers=3,port=12049,mountport=12048,nolock,tcp,soft,timeo=10 \
     192.168.1.168:/export/src /mnt/test
```

The following commands can be used to test connectivity to the ports:

```bash
nc -zv 192.168.1.168 12049
nc -zv 192.168.1.168 12048
nc -zv 192.168.1.168 11111
```

### Non-root (high ports — for development and testing)

```bash
./nfsd -p 11111 -m 12048 -n 12049 nfsd.conf
```

Mount using explicit ports:

```bash
sudo mount -t nfs \
     -o nfsvers=3,port=12049,mountport=12048,nolock \
     127.0.0.1:/export/src /mnt/src
```

### Port flags

| Flag | Default | Meaning |
|---|---|---|
| `-p PORT` | 111 | Portmapper port |
| `-m PORT` | 20048 | MOUNT protocol port |
| `-n PORT` | 2049 | NFS protocol port |

## Testing without root

`nfsd_test.py` is a self-contained Python 3 test client that exercises every
implemented procedure.  Start the server on high ports, then run the tests:

```bash
# Terminal 1:
./nfsd -p 11111 -m 12048 -n 12049 nfsd.conf

# Terminal 2:
python3 nfsd_test.py 127.0.0.1 11111 12048 12049
```

Expected output: 23 tests, 0 failures.

## Implemented NFS3 procedures

| Procedure | Status |
|---|---|
| NULL | ✓ |
| GETATTR | ✓ |
| SETATTR (size, times) | ✓ |
| LOOKUP | ✓ |
| ACCESS | ✓ (grants all) |
| READ | ✓ |
| WRITE (FILE_SYNC) | ✓ |
| CREATE (UNCHECKED / GUARDED) | ✓ |
| REMOVE | ✓ |
| READDIR | ✓ |
| READDIRPLUS | ✓ |
| FSSTAT | ✓ |
| FSINFO | ✓ |
| PATHCONF | ✓ |
| COMMIT | ✓ (no-op, always FILE_SYNC) |
| MKDIR | NFS3ERR_NOTSUPP |
| RENAME | NFS3ERR_NOTSUPP |
| SYMLINK / READLINK | NFS3ERR_NOTSUPP |
| LINK | NFS3ERR_NOTSUPP |
| MKNOD | NFS3ERR_NOTSUPP |

## File handle design

Each file handle is 16 bytes (well within the NFS3 64-byte limit):

```
bytes  0- 3: magic      = 0x4E465333 ('NFS3')
bytes  4- 7: export_id  (index into exports table)
bytes  8-11: dev        (st_dev, 32-bit)
bytes 12-15: ino        (st_ino, 32-bit)
```

All four fields are stored big-endian on the wire regardless of host
endianness.  See `fh_encode()` / `fh_decode()` in `fhandle.c`.

A path cache (512 entries, round-robin eviction) maps `(export_id, dev, ino)`
to the file's relative path from the export root.  It is populated on every
LOOKUP and READDIRPLUS call.

## Porting to MVS 3.8j

### Replace `vfs.c`

Every operating-system-specific call is in `vfs.c`.  The interface is:

```c
int        vfs_stat(const char *path, vfs_stat_t *st);
int        vfs_pread(path, buf, count, offset, *nread, *eof);
int        vfs_pwrite(path, buf, count, offset);
int        vfs_create(path, mode);
int        vfs_remove(path);
int        vfs_truncate(path, size);
int        vfs_set_times(path, set_atime, atime_sec, set_mtime, mtime_sec);
int        vfs_fsstat(path, *fs);
uint32_t   vfs_errno_to_nfs3(int err);
vfs_dir_t *vfs_opendir(path);
int        vfs_readdir_next(d, name, maxname, *fileid, *cookie);
void       vfs_seekdir_to(d, cookie);
void       vfs_closedir(d);
```

For MVS, `path` will be a PDS dataset + member reference rather than a POSIX
path.  The mapping from `export_id + relpath` to `dataset(member)` happens
entirely inside the new `vfs.c`.

### Planning notes for MVS

- **Directories** → PDS datasets (each export maps to one PDS).
- **Files** → PDS members (names up to 8 chars, uppercase).
- **mtime** → ISPF statistics block (last modified date + time).
- **File size** → calculated from number of records × record length (LRECL)
  for fixed-length datasets; exact for variable-length.
- **Text files** → strip trailing spaces from each fixed-length record and
  append `\n` on read.  Reverse on write.
- **EBCDIC/ASCII** → translate in `vfs_pread` (output) and `vfs_pwrite`
  (input) using a lookup table.  The RPC/XDR layer always works in ASCII
  for filenames (clients send ASCII; translate on the way in and out).
- **No malloc** → already the case; `vfs.c` uses a static dir pool.
- **Socket calls** → `nfsd.c` and `rpc.c` use standard BSD sockets.
  Replace with the Hercules TCP/IP instruction interface.

### Compiler flags for GCCMVS

```makefile
CC     = gccmvs
CFLAGS = -std=c99 -D__MVS__
```

The `#include <stdint.h>` fallback comment in `nfsd.h` shows how to define
`uint8_t` / `uint32_t` / `uint64_t` manually if the GCCMVS environment does
not provide `stdint.h`.

## Known limitations

- File handle path cache (512 entries) can evict entries for deep directory
  trees; stale file handles will return `NFS3ERR_STALE`.
- `st_ino` and `st_dev` are truncated to 32 bits in the file handle.  On
  filesystems with 64-bit inodes this can theoretically cause collisions.
- SETATTR does not implement `mode`, `uid`, or `gid` changes (silently
  ignored).
- No locking (`nolock` mount option recommended).
- No security: all clients have full read/write access.


# MVS VFS and mapping

```
# MVS mapping format
/export/tonyw/library {
    TONYW.LIBRARY.CNTL      fileext="jcl"
    TONYW.LIBRARY.C         fileext="c"
    TONYW.LIBRARY.H         fileext="h"
}
/export/sys1/proclib {
    SYS1.PROCLIB            fileext="jclproc"
    SYS1.PARMLIB            fileext="txt"
}
/export/sys1/parmlib {
    SYS1.PARMLIB            fileext="txt"
}
```

## VFS Nodes

An array of nodes, which create the virtual file system structure (the directories) 
and how those directories relate to the real MVS DASD layout.

### Node types

* `vfs-dir` - A fixed directory node, which exists only in the VFS
* `pds-dir` - A directory node, which maps to a single MVS PDS

### Node type `vfs-dir` (type ID `1`)

This node type creates a constant and fake directory node in the VFS. It has the following attributes --

* vfs-node-number - This node's ID number
* parent-vfs-node-number - The node ID of the parent node
* next-node-number - The next sibling node number
* node-type - The type of this node (type 1)
* first-child-node-number - The first child node
* directory-name - The name of the directory this node represents

```c
struct vfs_node_vfs_dir {
    uint32_t        node_num;
    uint32_t        parent_node_num;
    uint32_t        next_node_num;
    uint8_t         node_type;
    uint32_t        first_child_node_num;
    unsigned char * directory_name;
};
```

### Node type `pds-dir` (type ID `2`)

This node maps a MVS partitioned dataset into this VFS directory. It has the following attributes --

* `vfs-node-number` - This node's ID number
* `parent-vfs-node-number` - The node ID of the parent node
* `next-node-number` - The next sibling node number
* `node-type` - The type of this node (type 2)
* `first-child-node-number` - The first child node
* `directory-name` - The name of the directory this node represents
* `file-name-ext` - The file name extension that will be applied to the translated 
  names of the PDS directory members. This is also used to locate the correct 
  MVS PDS for new files being created and existing files being updated
* `MVS PDS dataset name` - The MVS dataset name
* `MVS vol-ser` - The MVS dataset's volume serial number

```c
struct vfs_node_pds_dir {
    uint32_t        node_num;
    uint32_t        parent_node_num;
    uint32_t        next_node_num;
    uint8_t         node_type;
    uint32_t        first_child_node_num;
    unsigned char * directory_name;
    unsigned char * file_name_ext;
    unsigned char   mvs_pds_dsname[45];
    unsigned char   mvs_vol_ser[7];
};
```


##