# nfsd — Minimal NFSv3 Server for MVS 3.8J

A minimal NFSv3 (RFC 1813) server written in pure C, targeting MVS 3.8J
(Hercules emulated mainframe) as the primary platform.  The server exposes
MVS Partitioned Datasets (PDS) as NFS-mounted directories, making PDS members
visible as ordinary files to any NFSv3 client (Linux, Windows, macOS).

## Design goals

- **No external libraries.** Standard C and POSIX sockets only.
- **Endian-agnostic.** All XDR encoding uses explicit byte manipulation —
  correct on both little-endian (Linux x86_64) and big-endian (MVS/S370)
  without relying on `htonl()`/`ntohl()`.
- **Single-threaded.** One `select()` loop, no threads, no `fork()`.
  Up to 16 concurrent TCP connections.
- **C89 compliant.** The server compiles with JCC (MVS C compiler) targeting
  C89.  No C99 features are used in the MVS-specific code. Mostly!

## Project status

**WARNING:** This is early release code. You shouldn't assume that this code won't trash
a mounted dataset. I'm sure there are plenty of bugs at this time, even we are fixing
them as we find them. I wouldn't go exporting/mounting `SYS1.PROCLIB` or `SYS1.PARMLIB`, or
other important datasets that could prevent an IPL from completing. 

### **Please have a recovery mechanmism in place if things go badly wrong.**

## Feature Status

| Feature | Status |
|---|---|
| Directory listing of a mounted PDS | Working |
| Multiple PDS under export | Working |
| Read a file (PDS member) | Working |
| File size (true text-mode size cached) | Working |
| Create / write files (PDS member) | Working |
| ISPF statistics set/updated on write and `touch` | Working |
| Remove a file (PDS member) | Working |
| Rename files | Not implemented |

## Source files

### Protocol layer (platform-neutral)

| File | Purpose |
|---|---|
| `nfsd.h` | All shared types, constants, prototypes, and MVS name aliases |
| `types.h` | Portable integer types (`uint8_t`, `uint32_t`, `uint64_t`, etc.) |
| `xdr.c` | XDR encode/decode — pure byte manipulation |
| `rpc.c` | RPC TCP framing, call parsing, reply headers |
| `exports.c` | Config file parser and export table |
| `fhandle.c` | File handle encoding and path cache |
| `portmap.c` | Portmapper protocol (port 111) |
| `mount3.c` | MOUNT protocol (port 20048) |
| `nfs3.c` | NFSv3 procedure implementations |
| `nfsd.c` | `main()`, `select()` loop, connection table |

### VFS abstraction

| File | Purpose |
|---|---|
| `vfs.c` | POSIX VFS implementation (Linux development and testing) |
| `mockvfs.c` | Stub VFS used for initial testing |
| `mvsvfs.c/h` | MVS VFS implementation — replaces `vfs.c` on MVS |

### MVS IO implementation

| File | Purpose |
|---|---|
| `mvsio.c/h` | Path classification (`mvs_path_type`), member-name validation, DCB info retrieval |
| `mvspdir.c/h` | PDS directory block parsing; ISPF statistics decode **and** encode |
| `mvspww.c/h` | Pending-member write pool — buffers WRITEs, STOWs on COMMIT, applies ISPF stats |
| `mvsdol.c/h` | Directory open-list pool — caches open PDS directory scans |
| `mvsprw.c/h` | PDS member read with sequential-read position cache |
| `mvsprf.c/h` | Performance stats tracking |
| `mvsfsz.c/h` | File-size cache — stores true text-mode sizes of PDS members |
| `mvsfid.c/h` | Stable 64-bit file ID generation from dataset + member name |
| `mvsutl.c/h` | JES2 job-id lookup (PSA→TCB→JSCB→SSIB) — seeds the write verifier |
| `ebcdic.c/h` | EBCDIC ↔ ASCII translation tables |

### Utilities

| File | Purpose |
|---|---|
| `logger.c/h` | Levelled logging (`log_debug/info/warn/error/fatal`); MVS WTO support |
| `hexdump.c/h` | Hex dump helper for debug output |
| `ressock.c` | Reserved-port socket helper |
| `asmutils.h` | C prototypes + name aliases for the MVS assembler helpers (`getcib`, `mvs_dynalloc`, `mvs_stow`) |
| `getcib.asm` | CIB (Console Information Block) reader — MVS assembler module |
| `mvsdalc.asm` | SVC 99 dynamic allocation (`mvs_dynalloc`) — MVS assembler |
| `mvsstow.asm` | BLDL / FIND / STOW-REPLACE ISPF-stats update (`mvs_stow`) — MVS assembler |
| `asmutils.h` | Contains prototypes and macro definitions for ASM modules |

### JCL

| File | Purpose |
|---|---|
| `jcl/makejcc.jcl` | JCL to compile and link the server on MVS using the JCC compiler |
| `tests-jcl/testrun.jcl` | JCL to run the unit-test build on MVS |

### Tests

| File | Purpose |
|---|---|
| `tests/runall.c` | munit test runner — aggregates all test suites |
| `tests/tstubs.c/h` | Export-table and path stubs shared by all test modules |
| `tests/tmvsio.c` | Tests for `mvsio.c` — `mvs_path_type()` (path classification) |
| `tests/tmvsio2.c` | Tests for `mvsio.c` — `mvs_get_pds_dsn_and_member()` + member-name validation |
| `tests/tmvspdir.c` | Tests for `mvspdir.c` — directory-entry parsing and ISPF stats decode/encode |
| `tests/tmvsprw.c` | Tests for `mvsprw.c` — read-position cache helpers |
| `tests/tmvsdol.c` | Tests for directory open-list pool (`mvsdol.c`) |
| `tests/tmvsfsz.c` | Tests for the file-size cache (`mvsfsz.c`) |
| `tests/tmvsprf.c` | Tests for the performance stats tracking (`mvsprf.c`) |

## Building

### Linux (development) -- THIS IS DEFUNCT FOR NOW!

```bash
make              # debug build  → build/nfsd
make RELEASE=1    # optimised build
make clean
```

Requires: gcc, glibc headers.  No other dependencies.

### Linux unit tests

The test suite uses [munit](https://github.com/nemequ/munit).  Place
`munit.h` and `munit.c` in the `tests/` directory, then:

```bash
cc -std=c99 -Wall -I src -I tests \
   tests/runall.c tests/tstubs.c \
   tests/tmvsio.c tests/tmvsio2.c tests/tmvspdir.c tests/tmvsprw.c \
   tests/tmvsdol.c tests/tmvsfsz.c \
   src/mvsio.c src/mvsdol.c src/mvsfsz.c tests/munit.c \
   -o tests/runall

tests/runall
```

### MVS (JCC compiler)

Upload the source to `TONYW.DINONFS.C` (or your equivalent PDS) and submit
`jcl/makejcc.jcl`.  The JCL compiles each module with JCC and links them
into a load module.

Key JCC flag: `-o` (lowercase) produces object code.  `-O` (uppercase)
produces assembler source — do not confuse the two.

```
PARM='-I//DDN:JCCINCL //DDN:SYSIN -o -LIST=//DDN:SYSPRINT -D__MVS__'
```

### MVS unit tests

You can run the unit tests on MVS. They use a ported version of `munit`. 
Running the job `tests-jcl/testrun.jcl` compiles the modules under test
along with the main test harness and executes the tests. 

## Config file

### Linux / POSIX format

```
# nfsd.conf
# <nfs-export-path>  <local-host-path>
/export/src    /home/user/src
/export/data   /home/user/data
```

Lines beginning with `#` are comments.  Blank lines are ignored.
Up to 16 exports are supported.

### MVS format

Each NFS export maps to one or more MVS PDS datasets. The export line may
be specified multiple times, with each line representing a single dataset.

```
# nfsd.conf
# <nfs-export-path>  <local-host-path>
/export/src    TEMP.TESTPROJ.C
/export/other  TEMP.TESTPROJ.CNTL
/export/other  TEMP.TESTPROJ.JCLLIB
```

For the mountpoint for /export/src, the NFS client and OS will show a single directory
of "temp.testproj.c" (a lowercase version of the dataset name). For the /export/other
mountpoint two directories will be shown -- the lower case versions of the dataset
names. 

## Running on MVS

Once the started task JCL procedure is available to JES2, then it can be 
started with `S NFSD`. Running NFSD as a started task has the benefit of allow
it to be stopped by the MVS STOP (P) comamdn `P NFSD`.

An example started task JCL procedure is provided in `jcl/nfsd.jcl`.

## Running on Linux (currently broken)

**As the Linux build is currently broken, this is not applicable at this time.**

Standard (root required for ports 111 and 2049).

Stop the system portmapper first:

```bash
sudo systemctl stop rpcbind   # or portmap, depending on distro
sudo ./build/nfsd nfsd.conf
```

## NFS Mounts

Standard (root required for ports 111 and 2049)

### Mounting an export from Linux

Mount from a client:

Note that from Linux, the readirplus operations perform much better
than a readdir operation. Also, the server does not implement
Sun's ACL sideband protocol and this should be disabled to avoid
many getacl operations from being sent to the server following a 
directory read.

Use the following options (tested on Ubuntu 26.04)

`rdirplus=force,noacl,nfsvers=3,nolock,tcp,acregmin=30,timeo=150`

| Option         | Description |
|----------------|-------------|
|rdirplus=force  | Force the client to use readdirplus operations |
|noacl           | Stop the client from performing get ACL operations |
|nfsvers=3       | Force NFS v3 protocol -- which is the only version the server supports |
|nolock          | No file locking |
|tcp             | Use the TCP protocol -- the server does not support UDP |
|acregmin=30     | Cache access permissions for 30 seconds |
|timeo=150       | Timeout operations after 15 seconds (150 deci-seconds) |

```bash
# Via portmapper (automatic port discovery):
sudo mount -t nfs \
     -o rdirplus=force,noacl,nfsvers=3,nolock,tcp,acregmin=30,timeo=150 \
     server:/exports /mnt/src

# With explicit ports:
sudo mount -t nfs \
     -o port=2049,mountport=20048,rdirplus=force,noacl,nfsvers=3,nolock,tcp,acregmin=30,timeo=150 \
     server:/exports /mnt/src

# Tested on Ubuntu 24.04.4 LTS:
sudo mount -t nfs \
     -o port=12049,mountport=12048,rdirplus=force,noacl,nfsvers=3,nolock,tcp,acregmin=30,timeo=150 \
     192.168.1.168:/export/src /mnt/test
```

Test port connectivity:

```bash
nc -zv 192.168.1.168 12049
nc -zv 192.168.1.168 12048
nc -zv 192.168.1.168 11111
```

### Windows NFS client

After installing "Services For NFS" / "Client for NFS" on Windows 11 Pro:

```
mount -o "nolock,nfsvers=3,tcp,soft,timeo=10" 192.168.1.168:/exports/jcllib x:
```

### Non-root (high ports — development and testing)

```bash
./build/nfsd -p 11111 -m 12048 -n 12049 nfsd.conf
```

Mount using explicit ports:

```bash
sudo mount -t nfs \
     -o nfsvers=3,port=12049,mountport=12048,nolock \
     127.0.0.1:/export/src /mnt/src
```

### Using tcpdump to look at packets

```bash
sudo tcpdump -i any -n port 2049 2>/dev/null
```

### Turning on NFS debug and viewing logs

Turn on NFS debug

```bash
sudo rpcdebug -m nfs -s all
```

To view logs --

```bash
sudo journalctl -kf
```

### Port flags

| Flag | Default | Meaning |
|---|---|---|
| `-p PORT` | 111 | Portmapper port |
| `-m PORT` | 20048 | MOUNT protocol port |
| `-n PORT` | 2049 | NFS protocol port |

## Implemented NFS3 procedures

| Procedure | Status |
|---|---|
| NULL | ✓ |
| GETATTR | ✓ |
| SETATTR (size truncate; atime/mtime → ISPF changed date) | ✓ |
| LOOKUP | ✓ |
| ACCESS | ✓ (grants all) |
| READ | ✓ |
| WRITE (echoes the client's requested stability) | ✓ |
| CREATE (UNCHECKED / GUARDED) | ✓ |
| REMOVE | ✓ |
| READDIR | ✓ |
| READDIRPLUS | ✓ |
| FSSTAT | ✓ |
| FSINFO | ✓ |
| PATHCONF | ✓ |
| COMMIT | ✓ (flushes the pending member — STOW) |
| MKDIR | NFS3ERR_NOTSUPP |
| RENAME | NFS3ERR_NOTSUPP |
| SYMLINK / READLINK | NFS3ERR_NOTSUPP |
| LINK | NFS3ERR_NOTSUPP |
| MKNOD | NFS3ERR_NOTSUPP |

## MVS architecture

This is in need of review and updating.

### VFS mapping

The NFS path-to-PDS mapping is performed by `mvsio.c`:

- `mvs_path_type()` classifies an incoming path as a PDS dataset reference
  (`MVS_PATH_TYPE_DATASET`) or a PDS member reference (`MVS_PATH_TYPE_PDS_MEMBER`).
  It matches against `host_path_ebcdic` in the export table.
- `mvs_get_pds_dsn_and_member()` extracts the dataset name and member name
  from a path.

### VFS node types

`mvsvfs.c` builds a virtual directory tree from the config file.  Two node
types are used:

**`vfs-dir` (type 1)** — a synthetic directory that exists only in the VFS,
with no corresponding MVS dataset.

```c
typedef struct vfs_node_vfs_dir {
    uint32_t        node_num;
    uint32_t        parent_node_num;
    uint32_t        next_node_num;
    uint8_t         node_type;
    uint32_t        first_child_node_num;
    unsigned char * directory_name;
} vfs_node_vfs_dir_t;
```

**`pds-dir` (type 2)** — a directory node that maps to a single MVS PDS.

```c
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
```

### Directory open-list pool (`mvsdol.c`)

PDS directory scans are expensive.  `mvsdol.c` maintains a static pool of
`vfs_dir_t` handles, each caching up to 250 sorted `pds_member_entry_t`
records from a recent directory read.

- Pool entries time out after **5 seconds** of inactivity and are reclaimed
  by the next `dir_openlist_find_free()` call.
- `dir_openlist_find_by_dsname()` looks up a live, non-expired entry by
  dataset name so a second `READDIR`/`READDIRPLUS` for the same PDS reuses
  the cached scan.
- Binary search with full operator support (`LT`, `EQ`, `GT`, `LE`, `GE`)
  is provided by `dir_openlist_search_members()` for cookie-based resumption.

### ISPF statistics (`mvspdir.c`)

ISPF stores a statistics block in the PDS directory user-data field of each
member it has touched — 30 bytes for standard stats, 40 for the extended
(32-bit) form.  `mvs_extract_ispf_stats()` decodes it and populates
`pds_member_entry_t` with:

- Version / modification level
- Creation and last-change dates (last-change to the second)
- Current, initial, and modified line counts
- User id of the last modifier
- `info_flags |= MVS_PDSDIR_IFLG_ISPFSTATS` to record that ISPF stats
  were present

Members without ISPF stats get synthetic values from `mvs_set_no_ispf_stats()`.

The reverse — `mvs_encode_ispf_stats()` — produces the 30-byte block from a
`pds_member_entry_t`, and is used by the write path (below) so members created
or edited over NFS show correct VV.MM, dates, size, and id in ISPF.

### Write support (`mvspww.c`, `mvsdalc.asm`, `mvsstow.asm`)

Because a PDS member can only be written sequentially in one open→write→close
pass, `mvspww.c` reassembles each member's WRITEs in an in-memory buffer and
STOWs the whole member once — at COMMIT, an idle sweep, eviction, or shutdown.
After the member is stowed, its ISPF statistics are applied **in place**
(without rewriting the member) by two assembler helpers: `mvs_dynalloc`
(SVC 99, allocates the PDS) and `mvs_stow` (BLDL to find the member, FIND to
position on it, then STOW REPLACE with the encoded stats).  A `touch` (SETATTR
time change) refreshes just the ISPF changed date the same way.  The full
design, including the record-conversion and durability model, is in
[`doc/design_nfs_write.md`](doc/design_nfs_write.md).

### File-size cache (`mvsfsz.c`)

Computing the true byte size of a PDS member requires opening the file and
reading it in text mode (stripping trailing spaces and converting fixed-length
records to newline-terminated lines).  This is expensive.

`mvsfsz.c` caches the result in a static table of up to 16 entries
(configurable via `MVSFSZ_CACHE_CAPACITY`), keyed on dataset name + member
name.  Each entry stores:

| Field | Type | Meaning |
|---|---|---|
| `file_size` | `uint64_t` | true text-mode size in bytes |
| `ttr_tt` | `uint16_t` | PDS TTR track address — detects member replacement |
| `ttr_r` | `uint8_t` | PDS TTR record number |
| `ispf_size` | `int32_t` | ISPF line count — detects content change |
| `ispf_mtime` | `int32_t` | ISPF modification timestamp — detects edits |

The caller is responsible for validity: retrieve the entry with `mvsfsz_get()`,
compare the validity fields against the current PDS directory entry, and call
`mvsfsz_invalidate()` if any field differs before re-reading.

`mvsfsz_put()` has upsert semantics — updating an existing key does not
increment the count or require a free slot.

### Read cache (`mvsprw.c`)

`mvsprw.c` maintains a small cache (`MVS_RCACHE_ENTRIES = 20`) of read state
for open PDS members.  Each entry stores the last read offset and the
corresponding `fpos_t` so that sequential NFS READ calls can continue from
the previous file position without rewinding to the start of the member.

`mvs_pds_member_pread()` and `mvs_pds_member_read()` both return a
`uint64_t *real_file_size` output parameter that is set to the true file size
when EOF is reached during a read.  This value is passed up to `vfs_pread()`
and used to populate the file-size cache via `mvsfsz_put()`.

### File identifiers (`mvsfid.c`)

NFS requires a stable 64-bit file ID (inode number) for each file and
directory.  On MVS there are no POSIX inodes, so `mvs_fid_hash()` computes
a stable 64-bit hash from the dataset name and member name:

- `mvs_fid_hash(dsname, member)` → 64-bit fileid for `vfs_stat_t.fileid`
- `mvs_fid_ino32(dsname, member)` → 32-bit fold for the file-handle cache key

Domain separation ensures that a dataset-only hash never collides with a
dataset+member hash.

### EBCDIC/ASCII translation

All internal strings (dataset names, member names, paths) are kept in ASCII
on Linux and in EBCDIC on MVS.  The `export_t` struct carries both variants:

| Field | Contains |
|---|---|
| `host_path` | ASCII host/dataset path |
| `host_path_ebcdic` | EBCDIC copy (== ASCII on Linux test builds) |
| `export_path` | ASCII NFS export path |
| `export_path_ebcdic` | EBCDIC copy |

`mvs_path_type()` always compares incoming paths against `host_path_ebcdic`.

### Logging

`logger.c` provides five levels: `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`.
On MVS, `INFO` and above are also sent to the operator console via WTO
(`_write2op`).  The `log_ascii()` helper converts an ASCII string to EBCDIC
for use as a `%s` argument on MVS, where `fprintf()` expects EBCDIC.

### MVS name-length limits

The MVS linkage editor truncates external names to 8 characters.  All
functions and globals in the MVS build are mapped to short names via
`#define` blocks at the top of each header (e.g. `xdr_init_read` →
`xdrInRd`, `vfs_pread` → `vfsPread`).  The block is guarded by
`#if defined(__MVS__)` so Linux builds use the full names.

## File handle design

Each file handle is 16 bytes (well within the NFS3 64-byte limit):

```
bytes  0- 3: magic      = 0x4E465333 ('NFS3')
bytes  4- 7: export_id  (index into exports table)
bytes  8-11: raw_dev    (export_id cast to uint32_t, on MVS)
bytes 12-15: raw_ino    (mvs_fid_ino32(dsname, member), on MVS)
```

On the Linux/POSIX build `raw_dev` and `raw_ino` hold `st_dev` and `st_ino`
from `stat()`.

All fields are stored big-endian on the wire.  See `fh_encode()` /
`fh_decode()` in `fhandle.c`.

A path cache (512 entries, round-robin eviction) maps `(export_id, raw_dev,
raw_ino)` to the file's relative path from the export root.  It is populated
on every LOOKUP and READDIRPLUS call.

## Known limitations

- File handle path cache (512 entries) can evict entries under heavy load;
  stale file handles return `NFS3ERR_STALE`.
- File size cache (`mvsfsz`) holds a maximum of 16 entries; members that cycle
  out lose their cached size and will require a full re-read to repopulate.
  An LRU eviction policy is planned.
- Read cache (`mvsprw`) holds 20 entries; entries time out after 5 seconds of
  inactivity.
- SETATTR handles `size` (truncate) and `atime`/`mtime` (mapped onto the ISPF
  changed date); `mode`, `uid`, and `gid` changes are silently ignored.
- Write buffering is in-memory with a per-member cap (`PWW_MAX_MEMBER_BYTES`,
  256 KB) and a small pool (`PWW_MAX_PENDING`, 4); a write beyond the cap
  returns `NFS3ERR_NOSPC`.  Disk-backed spill for larger members is a future
  phase (see the design doc).
- No locking (`nolock` mount option recommended).
- No authentication: all clients have full read/write access.
- RENAME is not implemented — JCC provides no way to rename a PDS member in
  place; it would need a dedicated assembler STOW routine.  (Create, write, and
  REMOVE are implemented — REMOVE uses JCC's `_unlink()` on the member.)

## Contributing

Contributions are welcome. Please see [CONTRIBUTING.md](CONTRIBUTING.md) for
the workflow and the code-style notes. We use the Developer Certificate of
Origin (DCO): sign off every commit with `git commit -s`.

## License

Released under the [MIT License](LICENSE).

Copyright (c) 2026 Tony Winslow and the dino_nfs contributors.
