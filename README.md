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

### **Please have a recovery mechanism in place if things go badly wrong.**

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
| Rename files (same PDS) | Working |

## Source files

### Protocol layer (platform-neutral)

| File | Purpose |
|---|---|
| `nfsd.h` | All shared types, constants, prototypes, and MVS name aliases |
| `types.h` | Portable integer types (`uint8_t`, `uint32_t`, `uint64_t`, etc.) |
| `xdr.c` | XDR encode/decode — pure byte manipulation |
| `rpc.c` | RPC TCP framing, call parsing, reply headers |
| `exports.c` | Config file parser (sections, `[Init]`/`[Exports]`) and export table |
| `cfgopts.c/h` | Export keyword-option parsing — `ro`/`rw`, `dirperm`/`memperm`/`rootperm`, `fileext`/`nofileext`; pure and unit-tested, used by `exports.c` |
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
| `mvspww.c/h` | Pending-member write pool — buffers WRITEs, STOWs on COMMIT, applies ISPF stats; spills to `mvsspl` past the in-memory threshold |
| `mvsspl.c/h` | Write-spill store — backs a large pending member with a temporary PS dataset (random `fseek` write/read) so memory use per member stays bounded |
| `mvsblkc.c/h` | PDS space prediction — counts the blocks a pending member will occupy once stowed and compares them against the dataset's free space, so a write that cannot fit is refused before the flush abends out of space |
| `mvsdol.c/h` | Directory open-list pool — caches open PDS directory scans |
| `mvsprw.c/h` | PDS member read with sequential-read position cache |
| `mvsprf.c/h` | Performance stats tracking |
| `mvsfsz.c/h` | File-size cache — stores true text-mode sizes of PDS members |
| `mvsfid.c/h` | Stable 64-bit file ID generation from dataset + member name |
| `mvsutl.c/h` | JES2 job-id lookup (PSA→TCB→JSCB→SSIB) for seeding write verifier; CVT timezone-offset read and LOCAL ↔ UTC epoch conversions for ISPF stat times |
| `ebcdic.c/h` | EBCDIC ↔ ASCII translation tables |

### Utilities

| File | Purpose |
|---|---|
| `logger.c/h` | Levelled logging (`log_debug/info/warn/error/fatal`); MVS WTO support |
| `hexdump.c/h` | Hex dump helper for debug output |
| `ressock.c` | Reset-port socket helper -- an extended util of Jason Winters RESET that allows specific port numbers to be specified. This allows sockets to be reset without harming other apps such as FTPD |
| `asmutils.h` | C prototypes + name aliases for the MVS assembler helpers (`getcib`, `mvs_dynalloc`, `mvs_stow`, `mvs_enq`) |
| `getcib.asm` | CIB (Console Information Block) reader (`getcib`) — MVS assembler module |
| `mvsdalc.asm` | SVC 99 dynamic allocation (`mvs_dynalloc`), `DISP=SHR`, returns its ddname via `DALRTDDN` — MVS assembler |
| `mvsstow.asm` | BLDL / FIND / STOW-REPLACE ISPF-stats update (`mvs_stow`) — MVS assembler |
| `mvsenq.asm` | ENQ / DEQ / TEST on the `SPFEDIT` resource (`mvs_enq`) — serialises member writes with ISPF/EDIT — MVS assembler |
| `jccprolg.asm` | `JCCPROLG` macro — JCC stack-frame entry linkage used by the assembler modules |
| `jccretrn.asm` | `JCCRETRN` macro — JCC return linkage used by the assembler modules |

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
| `tests/tmvspww.c` | Tests for `mvspww.c` — the pending-member write pool (in-memory buffer management) |
| `tests/tmvsspl.c` | Tests for `mvsspl.c` — the write-spill store, driven with a reference-model verifier (odd offsets, holes, overwrites, reuse, large sizes, fuzz) |
| `tests/tmvsblkc.c` | Tests for `mvsblkc.c` — record counting for F/FB and V/VB (line wrap, empty lines, unterminated tails, chunk boundaries), the fit test against a synthetic DSCB, and VTOC decoding |
| `tests/tcfgopts.c` | Tests for `cfgopts.c` — export keyword parsing and option resolution |
| `tests/tlogger.c` | Tests for `logger.c` — log-level and per-procedure level handling |
| `tests/txdr.c` | Tests for `xdr.c` — XDR encode/decode primitives (byte layout, uint64 word order, opaque/string padding, bounds checks and error latching, handle framing) |
| `tests/trpc.c` | Tests for `rpc.c` — RPC CALL header parsing (AUTH_NULL / AUTH_UNIX) and the reply-header writers |
| `tests/tfhandle.c` | Tests for `fhandle.c` — file-handle wire format `fh_encode` / `fh_decode` (round-trip, magic/length rejection, pad-trim, field boundaries) |
| `tests/tmvsfid.c` | Tests for `mvsfid.c` — FNV-1a fileid hashing (determinism, distinctness, NUL domain separation, length clamps, ino32 fold) |
| `tests/tebcdic.c` | Tests for `ebcdic.c` — CP037 translation tables both ways, unmapped fallbacks, buffer translators, `ebcdic_member_to_name` |
| `tests/tmvsutl.c` | Tests for `mvsutl.c` — LOCAL ↔ UTC epoch conversions (sign, inverse property) via the offset test seam |
| `tests/texports.c` | Tests for `exports.c` — the NFSDCONF parser + export table (multiple export paths, dataset accumulation, options, fail-closed, lookups). **Standalone program** — links the real `exports.c`, so it cannot use `tstubs`; built/run by `tests-jcl/testexp.jcl`, not `runall` |

> Coverage summary: see the **test coverage matrix** in
> [doc/readme_unit_tests.md](doc/readme_unit_tests.md), which maps every
> production module to its test module with a low/medium/high rating.

Standalone diagnostic programs (each has its own `main()` and is run
individually — not part of the `runall` munit suite):

| File | Purpose |
|---|---|
| `tests/testcib.c` | Exercises the `getcib` (GETCIB) assembler helper in a loop |
| `tests/testenq.c` | Exercises the `mvs_enq` (MVSENQ) ENQ / DEQ / TEST helper |
| `tests/ispftoun.c` | Dev-host utility for ISPF-stats packed-decimal / date decoding experiments |

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

Upload the source to `TONYW.NFSD.C` (or your equivalent PDS) and submit
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

The config file is sectioned, Windows `.ini` style.  Section names are
case-insensitive; `#` starts a comment and blank lines are ignored.  Full
reference: **[doc/readme_config.md](doc/readme_config.md)**.

```ini
[Init]
set loglvl info
set loglvl debug proc=write

[Exports]
# Export some PDS datasets
/exports    TEMP.TESTPROJ.C
/exports    TEMP.TESTPROJ.CNTL
/exports    TEMP.TESTPROJ.JCLLIB
```

Lines appearing before any section header are treated as `[Exports]`, so
older section-less config files keep working unchanged.  An unrecognised
section is reported and skipped.

### `[Init]`

Each line is an operator command, executed at startup exactly as if typed at
the MVS console via `F NFSD,<command>` — it is handed to the same handler, so
the two interfaces cannot drift apart.  See
[Per-procedure log levels](#per-procedure-log-levels) for the available
commands.  Note the built-in startup level is `DEBUG`, so `set loglvl info`
is usually the first line you want.

### `[Exports]`

Each NFS export maps to one or more MVS PDS datasets.  Repeat the export path
to group several datasets under it, one dataset per line:

```ini
[Exports]
/export/src    TEMP.TESTPROJ.C
/export/other  TEMP.TESTPROJ.CNTL
/export/other  TEMP.TESTPROJ.JCLLIB
```

For the mountpoint for /export/src, the NFS client and OS will show a single directory
of "temp.testproj.c" (a lowercase version of the dataset name). For the /export/other
mountpoint two directories will be shown -- the lower case versions of the dataset
names.

The member file extension is derived from the dataset's last qualifier,
lower-cased (members of `TEMP.TESTPROJ.CNTL` appear as `*.cntl`), unless
overridden with the `fileext=` export keyword (e.g. `fileext=jcl` to present a
`.CNTL` PDS as `*.jcl`) or suppressed entirely with `nofileext` (members shown
by their bare name) — see `doc/readme_config.md`.

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
mount -o "nolock,nfsvers=3,tcp,soft,timeo=10" 192.168.1.168:/exports x:
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
| ACCESS | ✓ |
| READ | ✓ |
| WRITE (echoes the client's requested stability) | ✓ |
| CREATE (UNCHECKED / GUARDED) | ✓ |
| REMOVE | ✓ |
| RENAME (within the same PDS only; cross-PDS → NFS3ERR_XDEV) | ✓ |
| READDIR | ✓ |
| READDIRPLUS | ✓ |
| FSSTAT | ✓ |
| FSINFO | ✓ |
| PATHCONF | ✓ |
| COMMIT | ✓ (flushes the pending member — STOW) |
| MKDIR | NFS3ERR_NOTSUPP |
| SYMLINK / READLINK | NFS3ERR_NOTSUPP |
| LINK | NFS3ERR_NOTSUPP |
| MKNOD | NFS3ERR_NOTSUPP |

## Server messages and message IDs

Every message the server writes — to STDERR and to the MVS console (WTO) —
carries a 9-character MVS-style message ID in place of the old `[LEVEL]` tag:

```
old:  [INFO ] pdsflush_slot: Starting flush for TEMP.ITEST.FB(UM1)
new:  NFSIW500I pdsflush_slot: Starting flush for TEMP.ITEST.FB(UM1)
```

On the log stream the date and time still precede it; the console line is
just the ID and the text.

### ID format — `NFS` + `AA` + `nnn` + `S`

| Part | Width | Meaning |
|---|---|---|
| `NFS` | 3 | Fixed — identifies the NFS server |
| `AA` | 2 | Functional area, from the table below |
| `nnn` | 3 | Message number, unique within its prefix |
| `S` | 1 | Severity |

Severity letters:

| Letter | Meaning | Function |
|---|---|---|
| `I` | Information | `logmsg_info()` |
| `W` | Warning | `logmsg_warn()` |
| `E` | Error | `logmsg_error()` |
| `T` | Trace | `logmsg_trace()` |
| `D` | Diagnostic / debug | `logmsg_debug()` |
| `S` | Severe | `logmsg_fatal()` |

The letter must agree with the function called — `logmsg_warn` with an ID
ending `I` is a bug, and nothing checks it at run time.

### Writing a message

```c
logmsg_info("NFSIW500I", "pdsflush_slot: Starting flush for %s(%s)",
            pds_name, member_name);
```

Level filtering, WTO routing and `log_ascii()` handling are unchanged — only
the prefix differs.  The older `log_info()` / `log_warn()` / … forms still
exist for code that has no ID yet, and print `[LEVEL]` as before.

The **same ID may appear at more than one call site only if a parameter in
the message identifies which one it is**.  Otherwise a message in a log
cannot be traced back to a single point in the code, which is the whole
purpose of carrying an ID.

### Functional area prefixes

| File | Prefix | Range in use | Notes |
|---|---|---|---|
| `nfsd.c` | `NFSDM` | 010–220 | Daemon / main |
| `xdr.c` | `NFSXD` | — | |
| `rpc.c` | `NFSRP` | 010–110 | |
| `exports.c` | `NFSCF` | 010–280 | Config |
| `cfgopts.c` | `NFSCF` | 500–590 | Config (shares the prefix) |
| `fhandle.c` | `NFSFH` | 010–020 | |
| `portmap.c` | `NFSPM` | — | |
| `mount3.c` | `NFSMN` | — | |
| `nfs3.c` | `NFSOP` | 010–310 | NFS operations |
| `vfs.c` | `NFSVF` | 700– | |
| `mockvfs.c` | `NFSVF` | 500– | |
| `mvsvfs.c` | `NFSVF` | 010–400 | |
| `mvsio.c` | `NFSIO` | 010–030 | |
| `mvspdir.c` | `NFSID` | 010–180 | PDS directory |
| `mvsdol.c` | `NFSID` | 500–520 | Directory open-list (shares the prefix) |
| `mvspww.c` | `NFSIW` | 010–340 | Member write |
| `mvspwfl.c` | `NFSIW` | 500–690 | Member flush (shares the prefix) |
| `mvsprw.c` | `NFSIR` | 010–110 | Member read |
| `mvsspl.c` | `NFSIS` | 010–100 | Write spill store |
| `mvsblkc.c` | `NFSIB` | 010–050 | Block/space calculation |
| `mvsprf.c` | `NFSST` | 010–070 | Statistics |
| `mvsfsz.c` | `NFSFZ` | — | |
| `mvsfid.c` | `NFSFI` | — | |
| `mvsutl.c` | `NFSUT` | — | |
| `ebcdic.c` | `NFSEA` | — | |
| `hexdump.c` | `NFSLH` | — | |
| `logger.c` | `NFSLG` | 010–120 | |

### Numbering conventions

Numbers are allocated **in tens** (010, 020, 030 …) so a new message can be
inserted between two existing ones without renumbering anything.

Where several files share a prefix, each file gets its own **range** — the
first file starts at 010, the second at 500, a third at 700 — so a number
still identifies one file, not just an area.  When adding a message, take the
next free ten in that file's range rather than reusing a gap, unless the gap
was left by a message that was deleted.

Filtering the console or log by message ID is a possible future addition;
nothing depends on it today.

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

### Full-dataset prediction (`mvsblkc.c`)

Writing into a PDS with no room left abends `SB14`, and an abend recovered by
`_setjmp_stae` can leave the JCC runtime's file lock held — after which the
next file operation deadlocks and the whole server hangs silently.  Rather
than try to survive that abend, the server predicts it.

`mvsblkc.c` maintains a running estimate of the blocks each pending member
will occupy once stowed — reproducing how JCC splits the byte stream into
records for F/FB and V/VB — and checks it against the dataset's free space
read fresh from the VTOC (`DS1LSTAR` / `DS1TRBAL`, via `mvsdscb.asm`) on every
decision.  All three routes into the pending pool are gated: CREATE, WRITE and
SETATTR(size).  A member that will not fit is refused with `NFS3ERR_NOSPC`
before anything is buffered, so the flush is never attempted.

Every ambiguous choice is resolved towards "predict full", because a wrong
"fits" costs an abend while a wrong "full" only costs an error the client
reports.  Exports whose DSCB cannot be read, or that are not PO with a
usable F/FB or V/VB record format, are rejected at config load.

See [`doc/design_pds_full_prediction.md`](doc/design_pds_full_prediction.md)
for the design and [`doc/analysis_io_lock_hang.md`](doc/analysis_io_lock_hang.md)
for the hang investigation that motivated it.

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

`logger.c` provides six levels, from most to least detailed: `DEBUG`,
`TRACE`, `INFO`, `WARN`, `ERROR`, `FATAL`.  `TRACE` sits between `DEBUG`
and `INFO` — more detailed than `INFO`, less than `DEBUG`.  The emit
filter is "drop anything below the current level", so setting the level
to `TRACE` emits `TRACE` and everything above it.

On MVS, lines are also sent to the operator console via WTO (`_write2op`).
The console has its **own** threshold (`WTOLVL`, default `INFO`), but it is
a *subset* of the log stream, not an independent channel: a line reaches
the console only when it clears **both** the stream level and `WTOLVL`, so
the effective console floor is `max(LOGLVL, WTOLVL)`.  Raising `WTOLVL`
quietens the console without touching the stream — e.g. stream at `DEBUG`
for the log file while the console only shows `WARN` and above — but
`WTOLVL` can never surface a line the stream level has already filtered
out.  The `log_ascii()` helper converts an ASCII string to EBCDIC for use
as a `%s` argument on MVS, where `fprintf()` expects EBCDIC.

#### Per-procedure log levels

Each NFSv3 procedure (GETATTR, SETATTR, WRITE, ...) has its own log-level
slot in a table owned by the logger.  Every slot starts out *inheriting*
the global level.  The NFS3 dispatcher installs a procedure's effective
level for the duration of the call and restores the global level on the
way out, so one chatty operation can be turned up (or a noisy one turned
down) without touching the rest.

The table is driven at runtime by the MVS `MODIFY` (`F`) command:

| Command | Effect |
|---|---|
| `F NFSD,SET LOGLVL INFO` | Set the **global** log-stream level to `INFO`. |
| `F NFSD,SET LOGLVL TRACE PROC=WRITE` | Pin the NFS **WRITE** handler to `TRACE`. |
| `F NFSD,SET LOGLVL INFO PROC=SETATTR` | Pin **SETATTR** to `INFO`. |
| `F NFSD,SET WTOLVL DEBUG` | Set the **operator-console** (WTO) level to `DEBUG`. |

`<level>` is `DEBUG|TRACE|INFO|WARN|ERROR|FATAL`; `PROC=<name>` is any
NFS3 procedure name (`GETATTR`, `SETATTR`, `LOOKUP`, `ACCESS`, `READ`,
`WRITE`, `CREATE`, `REMOVE`, `RENAME`, `READDIR`, `READDIRPLUS`,
`FSSTAT`, `FSINFO`, `PATHCONF`, `COMMIT`, `NULL`).  A global `SET LOGLVL`
changes the baseline that every *inheriting* procedure follows; a pinned
procedure keeps its level until re-pinned.  Parsing is case-insensitive.
Unrecognised or malformed commands are logged and ignored.

The MODIFY handler in `nfsd.c` simply forwards the operand text to
`log_handle_modify()`; all command grammar lives in `logger.c`.

### MVS name-length limits

The MVS linkage editor truncates external names to 8 characters.  All
functions and globals in the MVS build are mapped to short names via
`#define` blocks at the top of each header (e.g. `xdr_init_read` →
`xdrInRd`, `vfs_pread` → `vfsPread`).  The block is guarded by
`#if defined(__MVS__)` so Linux builds use the full names.

## File handle design

Each file handle is 60 bytes (within the NFS3 64-byte limit, and a multiple
of 4 so XDR adds no padding).  The handle is **self-describing** — it
carries the name of the object it refers to, so resolving it needs no
server-side state and a handle stays valid across a server restart:

```
bytes  0- 3: magic      = 0x4E465333 ('NFS3')
bytes  4- 7: export_id  = stable hash of the export path (NOT an index)
bytes  8-51: dsname     = MVS dataset name, 44 bytes, ASCII, blank-padded
bytes 52-59: member     = PDS member name,  8 bytes, ASCII, blank-padded
```

The object kind is implied by which name fields are set: both empty = the
export root; dsname only = a PDS directory; both set = a PDS member.  The
file extension is not carried — it comes from the dataset's config.

Because the handle names its object, there is **no file-handle cache** and
`NFS3ERR_STALE` is returned only when the object is genuinely unreachable
(its export or dataset is no longer exported) — never because the server
lost track of it.

The names are ASCII (so a handle is readable in a packet trace); the two
numeric fields are big-endian.  See `fh_encode()` / `fh_decode()` in
`fhandle.c`, and **[doc/readme_filehandles.md](doc/readme_filehandles.md)**
for the full rationale, the EBCDIC pitfalls, and the upgrade note (clients
must remount once).

A path cache (512 entries, round-robin eviction) maps `(export_id, raw_dev,
raw_ino)` to the file's relative path from the export root.  It is populated
on every LOOKUP and READDIRPLUS call.

## Known limitations

- File size cache (`mvsfsz`) holds a maximum of 16 entries; members that cycle
  out lose their cached size and will require a full re-read to repopulate.
  Entries are evicted on a LRU basis.
- Read cache (`mvsprw`) holds 20 entries; entries time out after 5 seconds of
  inactivity.
- SETATTR handles `size` (truncate) and `atime`/`mtime` (mapped onto the ISPF
  changed date); `mode`, `uid`, and `gid` changes are silently ignored.
- Write buffering is in-memory with a per-member cap (`PWW_MAX_MEMBER_BYTES`,
  256 KB) and a small pool (`PWW_MAX_PENDING`, 4); a write beyond the cap
  returns `NFS3ERR_NOSPC`.  Disk-backed spill for larger members is a future
  phase (see the design doc).
- No locking (`nolock` mount option recommended). The server uses ISPF style
  SPFEDIT ENQ resources for prevent corruption with ISPF/REVIEW etc.
- No authentication and no per-client access control: access is a property
  of the *export*, not the client.  An export (or a single dataset) can be
  marked read-only with the `ro` keyword, the reported permission bits
  are configurable (`dirperm` / `memperm` / `rootperm`), and the member file
  extension can be overridden (`fileext`) or suppressed (`nofileext`) — see
  [doc/readme_config.md](doc/readme_config.md).  Read-only is enforced for
  every client including one mounting as root.
- RENAME works only **within a single PDS** — a member cannot be moved to a
  different dataset (different directory).  A cross-PDS request returns
  `NFS3ERR_XDEV`, which *may* prompt the client to fall back to copy+delete.
  RENAME uses JCC's `rename()` on the member (as REMOVE uses `_unlink()`).

## Contributing

Contributions are welcome. Please see [CONTRIBUTING.md](CONTRIBUTING.md) for
the workflow and the code-style notes. We use the Developer Certificate of
Origin (DCO): sign off every commit with `git commit -s`.

## License

Released under the [MIT License](LICENSE).

Copyright (c) 2026 Tony Winslow and the mvs_nfsd contributors.
