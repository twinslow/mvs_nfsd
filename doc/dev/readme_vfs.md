# VFS Module — Implementation Reference

## Purpose

`vfs.c` is the **sole** file in the NFS server that makes operating-system-specific
filesystem calls.  Every other module (`nfs3.c`, `mount3.c`, `fhandle.c`, …) reaches
the filesystem exclusively through the functions declared here.

To port the server to a completely different storage system — whether a mainframe
partitioned dataset library, an object store, a flat-file database, or any other
non-hierarchical medium — you replace `vfs.c` in its entirety with a new
implementation that satisfies the contracts described in this document.  No other
source file needs to change.

---

## Guiding Principles for Implementors

1. **One file, one abstraction.**  The implementation file must define all functions
   listed in this document.  It must not export any additional symbols that other
   modules might accidentally depend on.

2. **errno is the error channel.**  Every function that returns `-1` on failure must
   also set `errno` to a value that `vfs_errno_to_nfs3()` can translate.  If your
   platform does not use POSIX errno, maintain a module-local integer and set it
   before returning `-1`.

3. **Paths are the external key.**  The rest of the server identifies files by
   absolute paths that it constructs from the export root and a relative path stored
   in the file handle cache.  Your implementation receives those paths and must
   translate them to whatever internal identity your storage system uses.

4. **No malloc required.**  The reference implementation uses a static pool for
   directory handles.  Your implementation may use the same pattern.  If your
   platform provides dynamic allocation, you may use it, but it is not required.

5. **Atomicity and durability.**  Write calls include an `fsync`-equivalent before
   closing.  Preserve this behaviour so that the NFS COMMIT semantics are honoured.

6. **The `vfs_dir_t` type is yours to define.**  It is opaque to all other modules.
   Define `struct vfs_dir` in your implementation file to hold whatever state your
   directory iterator requires.

7. **ASCII data** is the input and output of the VFS module interface. The absolute 
   path name passed into the functions is ASCII, on the EBCDIC host. Note that the
   structure export_t field `host_path` is in ASCII. There is a corresponding
   field `host_path_ebcdic` with the EBCDIC version of the host path.
   
---

## Data Types

### `vfs_stat_t`

Defined in `nfsd.h`.  Carries all metadata for one file that the NFS protocol may
need to report.  Your `vfs_stat()` implementation must populate every field.

```c
typedef struct {
    uint32_t ftype;       /* File type: NF3REG, NF3DIR, NF3LNK, NF3BLK,
                             NF3CHR, NF3SOCK, NF3FIFO                       */
    uint32_t mode;        /* Permission bits, lower 12 bits only (octal
                             0000–0777).  Higher bits are ignored.           */
    uint32_t nlink;       /* Hard link count.  Use 1 for systems that do
                             not support hard links.                         */
    uint32_t uid;         /* Owner user ID.  Use a fixed value (e.g. 0)
                             if your system has no user concept.             */
    uint32_t gid;         /* Owner group ID.  Same note as uid.             */
    uint64_t size;        /* File size in bytes.  For directories, some
                             clients accept 0; others expect the byte size
                             of the directory data structure.                */
    uint64_t used;        /* Actual disk space consumed, in bytes.  For
                             systems without block allocation, set equal
                             to size.                                        */
    uint32_t rdev_maj;    /* Device major number.  Set to 0 for non-device
                             files.                                          */
    uint32_t rdev_min;    /* Device minor number.  Set to 0 for non-device
                             files.                                          */
    uint64_t fsid;        /* Filesystem identifier.  Must be the same for
                             all files in one export, different across
                             exports.  Any stable non-zero value is fine.   */
    uint64_t fileid;      /* Per-file unique identifier within the export.
                             Reported to the NFS client as the inode number.
                             Must be stable for the lifetime of the file.   */
    uint32_t atime_sec;   /* Last-access time, seconds since Unix epoch.    */
    uint32_t atime_nsec;  /* Last-access time, nanosecond fraction.
                             Set to 0 if sub-second resolution unavailable. */
    uint32_t mtime_sec;   /* Last-modification time, seconds since epoch.   */
    uint32_t mtime_nsec;  /* Last-modification time, nanosecond fraction.   */
    uint32_t ctime_sec;   /* Last-metadata-change time, seconds since epoch.
                             If your system has no ctime concept, copy mtime.*/
    uint32_t ctime_nsec;  /* ctime nanosecond fraction.                     */
    uint32_t raw_dev;     /* Internal cache key: a stable 32-bit value that
                             identifies the storage volume or dataset.  Must
                             be consistent with raw_ino to form a unique pair
                             per file within this server process.            */
    uint32_t raw_ino;     /* Internal cache key: a stable 32-bit value that
                             uniquely identifies the file within raw_dev.
                             On POSIX: st_dev / st_ino truncated to 32 bits.
                             On MVS PDS: dataset index / member index.       */
} vfs_stat_t;
```

**Critical fields for correct NFS operation:**

| Field | Why it matters |
|---|---|
| `ftype` | Determines whether the client treats the object as a file, directory, or symlink |
| `fileid` | Used by clients to detect renames and hard links; must not change for the life of the file |
| `size` | Controls where the client thinks EOF is |
| `mtime_sec` | Clients use this to invalidate caches; must advance when data changes |
| `raw_dev` / `raw_ino` | Used internally by `fhandle.c` as the cache lookup key; do not expose to clients |

---

### `vfs_fsstat_t`

Carries filesystem-level capacity information.

```c
typedef struct {
    uint64_t total_bytes;  /* Total capacity in bytes                        */
    uint64_t free_bytes;   /* Bytes free (including reserved space)          */
    uint64_t avail_bytes;  /* Bytes available to unprivileged users          */
    uint64_t total_files;  /* Maximum number of files the filesystem supports*/
    uint64_t free_files;   /* File slots currently free                      */
    uint64_t avail_files;  /* File slots available to unprivileged users     */
    uint32_t invarsec;     /* Seconds for which the reported values are
                              guaranteed stable.  0 = values may change at
                              any time.                                       */
} vfs_fsstat_t;
```

For systems without meaningful capacity accounting, return plausible fixed values.
NFS clients use these figures only for display (`df`) and pre-flight space checks;
they are not relied upon for correctness.

---

### `vfs_dir_t`  (opaque)

```c
typedef struct vfs_dir vfs_dir_t;
```

`struct vfs_dir` is defined **inside your implementation file only**.  No other
module sees its members.  It holds whatever state is needed to iterate through the
entries of one open directory:

- A handle or cursor into your directory structure
- The cookie counter used by `vfs_readdir_next` (see below)
- Any buffered data your iterator needs between calls

---

### Timestamp constants  (defined in `nfsd.h`)

Used as the `set_atime` / `set_mtime` arguments to `vfs_set_times()`:

| Constant | Value | Meaning |
|---|---|---|
| `SET_DONT_CHANGE` | 0 | Leave this timestamp exactly as it is |
| `SET_TO_SERVER_TIME` | 1 | Set to the current time on the server |
| `SET_TO_CLIENT_TIME` | 2 | Set to the time supplied by the client |

---

## Function Reference

---

### `vfs_stat`

```c
int vfs_stat(const char *path, vfs_stat_t *vs);
```

**Purpose:** Retrieve the metadata for the file or directory named by `path` and
store it in `*vs`.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `path` | in | Absolute path to the file.  NUL-terminated. |
| `vs` | out | Caller-allocated struct to receive the metadata. |

**Return value:** `0` on success, `-1` on error with `errno` set.

**Implementation notes:**

- On POSIX systems this wraps `lstat()`.  Use `lstat`, not `stat`, so that symlinks
  are reported as symlinks rather than being followed.
- The `fileid` field must be unique within the export and stable across server
  restarts.  On POSIX, `st_ino` satisfies this.  On non-inode systems, derive a
  stable value from the file's primary key in your storage system (e.g. member index
  within a PDS).
- If your filesystem does not distinguish access time from modification time, set
  `atime_sec = mtime_sec` and `atime_nsec = mtime_nsec`.
- If your filesystem has no concept of symbolic links, never return `ftype = NF3LNK`.
  Map everything to either `NF3REG` or `NF3DIR`.
- `raw_dev` and `raw_ino` are not sent to clients; they are used only by
  `fhandle.c` as a lookup key.  Choose any stable 32-bit values that form a unique
  pair per file.

---

### `vfs_pread`

```c
int vfs_pread(const char *path, void *buf, uint32_t count,
              uint64_t offset, uint32_t *nread, int *eof);
```

**Purpose:** Read up to `count` bytes from the file named by `path`, starting at
byte `offset`, into `buf`.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `path` | in | Absolute path to the file. |
| `buf` | out | Caller-allocated buffer of at least `count` bytes. |
| `count` | in | Maximum number of bytes to read.  Never exceeds `MAX_READ_SIZE` (64 KB). |
| `offset` | in | Byte offset within the file at which to start reading. |
| `nread` | out | Set to the actual number of bytes placed in `buf`. |
| `eof` | out | Set to `1` if the read reached or passed end-of-file; `0` otherwise. |

**Return value:** `0` on success, `-1` on error with `errno` set.

**Implementation notes:**

- A short read (where `*nread < count`) is not an error as long as the function
  returns `0`.  It simply means the file is shorter than requested.
- `*eof` must be set accurately.  NFS clients use it to know when to stop issuing
  READ calls.  The reference implementation determines EOF by comparing
  `offset + *nread` against `st_size` obtained with `fstat()` before the read.
- On systems without positional I/O (`pread`), open the file, seek to `offset`,
  read `count` bytes, then close.  Ensure the file size check uses the same open
  file descriptor so the size and read position are consistent.
- Opening and closing on every call is intentional.  The server is stateless; it
  does not maintain open file descriptors between NFS operations.
- If `offset` is at or beyond end-of-file, return `0` with `*nread = 0` and
  `*eof = 1`.

---

### `vfs_pwrite`

```c
int vfs_pwrite(const char *path, const void *buf,
               uint32_t count, uint64_t offset);
```

**Purpose:** Write exactly `count` bytes from `buf` into the file named by `path`,
starting at byte `offset`.  Flush to stable storage before returning.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `path` | in | Absolute path to the file. |
| `buf` | in | Data to write. |
| `count` | in | Number of bytes to write.  Never exceeds `MAX_WRITE_SIZE` (64 KB). |
| `offset` | in | Byte offset within the file at which to start writing. |

**Return value:** `0` if all `count` bytes were written and flushed successfully,
`-1` on any error with `errno` set.

**Implementation notes:**

- A partial write must be treated as an error and `-1` returned.  The NFS WRITE
  protocol guarantees that either all bytes are written or none are, from the
  client's perspective.
- The `fsync` (or equivalent) before closing is required so that NFS COMMIT
  semantics can be honoured.  If your storage system writes are inherently
  synchronous (e.g. BSAM WRITE with WAIT on MVS), the explicit flush call is
  unnecessary but harmless to omit.
- If your system uses fixed-length records, you must handle writes that cross record
  boundaries and writes that do not align to record boundaries (partial record
  writes).
- Errno must be preserved across the flush and close calls, because both can
  overwrite it.  Save errno to a local variable immediately after the write, then
  restore it before returning.
- The file must already exist.  This function does not create files; use
  `vfs_create()` for that.

---

### `vfs_create`

```c
int vfs_create(const char *path, uint32_t mode);
```

**Purpose:** Create a new, empty, regular file at `path` with the given permission
bits.  If a file already exists at `path`, truncate it to zero length.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `path` | in | Absolute path for the new file. |
| `mode` | in | POSIX permission bits (lower 12 bits, e.g. `0644`).  Ignore if your system has no permission concept. |

**Return value:** `0` on success, `-1` on error with `errno` set.

**Implementation notes:**

- On POSIX this is `open(path, O_CREAT | O_WRONLY | O_TRUNC, mode)` followed by
  `close()`.
- On systems that do not support truncation-on-create, check for the file's
  existence first and either create or truncate as appropriate.
- On MVS PDS: allocate a new member.  If the member already exists, delete the
  existing data blocks and leave the directory entry pointing to an empty member.
- The returned file descriptor (if any) must be closed before returning.  The server
  does not maintain open handles between NFS operations.
- If your system requires allocation parameters (record format, block size, primary
  and secondary extents), encode those into the export configuration rather than
  deriving them from `mode`.

---

### `vfs_remove`

```c
int vfs_remove(const char *path);
```

**Purpose:** Delete the regular file at `path`.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `path` | in | Absolute path of the file to delete. |

**Return value:** `0` on success, `-1` on error with `errno` set.

**Implementation notes:**

- On POSIX this is `unlink(path)`.
- This function is called only for regular files.  Directory removal is not
  currently supported by this server.
- On systems that use reference counts or deferred deletion, the file's data must be
  considered unreachable as soon as this call returns successfully, even if the
  underlying storage reclamation is deferred.
- If the file does not exist, return `-1` with `errno = ENOENT`.

---

### `vfs_rename`

```c
int vfs_rename(const char *from, const char *to);
```

**Purpose:** Rename (or move) the file at `from` to `to`.  Both paths are within the
same export.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `from` | in | Current absolute path of the file. |
| `to` | in | New absolute path.  If a file already exists at `to`, it is replaced atomically. |

**Return value:** `0` on success, `-1` on error with `errno` set.

**Implementation notes:**

- On POSIX this is the `rename(2)` system call, which is atomic when both paths are
  on the same filesystem.
- No file-handle bookkeeping is needed after a rename.  Handles are
  self-describing (they name their object), so the renamed file's handle is simply
  the one derived from its new path, and a handle held for the old name correctly
  refers to an object that no longer exists.  See
  [readme_filehandles.md](readme_filehandles.md).
- On systems that do not support atomic rename-over (i.e. the destination must be
  deleted first), there is a window where the file temporarily disappears.  Clients
  may see `NFS3ERR_NOENT` in that window.  This is an inherent limitation of such
  systems.
- Cross-directory rename (moving a file from one directory to another within the
  same export) must be supported.  The `from` and `to` paths may have different
  directory components.
- Cross-export rename is never requested; the NFS protocol returns `EXDEV` for such
  operations and the server handles it before calling this function.

---

### `vfs_truncate`

```c
int vfs_truncate(const char *path, uint64_t size);
```

**Purpose:** Change the size of the file at `path` to exactly `size` bytes.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `path` | in | Absolute path to the file. |
| `size` | in | Target file size in bytes. |

**Return value:** `0` on success, `-1` on error with `errno` set.

**Implementation notes:**

- On POSIX this is `truncate(path, (off_t)size)`.
- If `size` is less than the current file size, the file is shortened and the excess
  data is discarded.
- If `size` is greater than the current file size, the file is extended.  The
  contents of the new region are implementation-defined; zero-fill is conventional
  and expected by most clients.
- On systems with fixed-length records or that do not support arbitrary file sizes,
  approximating the target size to a record boundary is acceptable.  Set `errno` to
  `EINVAL` if the requested size cannot be approximated.
- `size` is a `uint64_t` but values larger than what your system supports should
  result in `-1` with `errno = EFBIG`.

---

### `vfs_set_times`

```c
int vfs_set_times(const char *path,
                  int set_atime, uint32_t atime_sec, uint32_t atime_nsec,
                  int set_mtime, uint32_t mtime_sec, uint32_t mtime_nsec);
```

**Purpose:** Set the access time and/or modification time of the file at `path`.
Each timestamp is controlled independently by a mode flag.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `path` | in | Absolute path to the file. |
| `set_atime` | in | One of `SET_DONT_CHANGE`, `SET_TO_SERVER_TIME`, `SET_TO_CLIENT_TIME`. |
| `atime_sec` | in | Desired atime, seconds since Unix epoch.  Used only when `set_atime == SET_TO_CLIENT_TIME`. |
| `atime_nsec` | in | Desired atime, nanosecond fraction.  Used only when `set_atime == SET_TO_CLIENT_TIME`. |
| `set_mtime` | in | One of `SET_DONT_CHANGE`, `SET_TO_SERVER_TIME`, `SET_TO_CLIENT_TIME`. |
| `mtime_sec` | in | Desired mtime, seconds since epoch.  Used only when `set_mtime == SET_TO_CLIENT_TIME`. |
| `mtime_nsec` | in | Desired mtime, nanosecond fraction.  Used only when `set_mtime == SET_TO_CLIENT_TIME`. |

**Return value:** `0` on success, `-1` on error with `errno` set.

**Implementation notes:**

- `SET_DONT_CHANGE` means do not touch that timestamp at all.  Do not read it and
  write it back; just leave it unchanged.
- `SET_TO_SERVER_TIME` means set the timestamp to whatever "now" is on the server.
  Do not use the client-supplied `*_sec` / `*_nsec` values.
- `SET_TO_CLIENT_TIME` means set the timestamp to the exact value supplied in the
  `*_sec` and `*_nsec` parameters.
- The two timestamps are independent.  It is valid to receive
  `set_atime = SET_DONT_CHANGE` and `set_mtime = SET_TO_SERVER_TIME` in the same
  call; only mtime should be updated.
- On POSIX the reference implementation uses `utimensat(2)` with `UTIME_OMIT` and
  `UTIME_NOW`.  Older POSIX systems that only have `utimes(2)` must read the current
  timestamps with `lstat()` before calling `utimes()` if one of the timestamps must
  be preserved; this introduces a race condition that is acceptable for this server.
- If your system only records modification time and not access time, implement
  `SET_DONT_CHANGE` and `SET_TO_SERVER_TIME`/`SET_TO_CLIENT_TIME` for mtime and
  silently ignore all `set_atime` requests.
- Nanosecond precision is not required.  If your system only supports second-level
  timestamps, ignore the `*_nsec` parameters when `set_*time == SET_TO_CLIENT_TIME`.

---

### `vfs_fsstat`

```c
int vfs_fsstat(const char *path, vfs_fsstat_t *fs);
```

**Purpose:** Report capacity and usage statistics for the filesystem that contains
`path`.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `path` | in | Any absolute path within the export.  Used only to identify which filesystem to query. |
| `fs` | out | Caller-allocated struct to receive the statistics. |

**Return value:** `0` on success, `-1` on error with `errno` set.

**Implementation notes:**

- On POSIX this wraps `statvfs(path, &sv)`.
- `total_bytes`, `free_bytes`, `avail_bytes` are in bytes, not blocks.  Multiply
  block counts by block size before storing.
- `avail_bytes` and `avail_files` are the values available to an unprivileged user.
  On systems without a reserved-space concept, set them equal to `free_bytes` and
  `free_files` respectively.
- On systems where capacity accounting is unavailable or unreliable, return plausible
  fixed values.  NFS clients use these only for informational display and pre-flight
  space checks; they do not affect protocol correctness.
- For a PDS on MVS: derive `total_bytes` from the dataset allocation (primary +
  secondary extents × tracks × bytes-per-track); `free_bytes` from the unused
  directory blocks and free tracks; `total_files` from the maximum number of
  directory entries; `free_files` from unused directory slots.

---

### `vfs_errno_to_nfs3`

```c
uint32_t vfs_errno_to_nfs3(int err);
```

**Purpose:** Convert a POSIX `errno` value to the corresponding NFSv3 status code.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `err` | in | A POSIX errno value, or `0` for success. |

**Return value:** An NFS3 status code.  `NFS3_OK` (0) for success.  One of the
`NFS3ERR_*` constants for error conditions.  `NFS3ERR_IO` (5) for any unrecognised
error.

**Implementation notes:**

- The reference implementation is a `switch` statement covering the POSIX errors
  that the NFS protocol explicitly distinguishes.
- If your platform uses a different error numbering system, map your platform's error
  codes to NFS3 error codes in this function.  The rest of the server always calls
  this function before constructing an NFS reply, so the mapping is centralised here.
- The complete set of NFS3 error codes and their POSIX equivalents is defined in
  RFC 1813 Section 3.  The codes used by this server are defined in `nfsd.h` as
  `NFS3ERR_*` constants.
- Always return `NFS3_OK` when `err == 0`.  Never return `NFS3_OK` when `err != 0`.
- Unknown or platform-specific errors should map to `NFS3ERR_IO`.  This tells the
  client that a server-side I/O error occurred, which is the safest fallback.

---

### `vfs_opendir`

```c
vfs_dir_t *vfs_opendir(const char *path);
```

**Purpose:** Open the directory at `path` for sequential iteration and return an
opaque handle.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `path` | in | Absolute path to the directory. |

**Return value:** A pointer to a `vfs_dir_t` handle on success, `NULL` on error with
`errno` set.

**Implementation notes:**

- The handle must be allocated from a static pool of `MAX_OPEN_DIRS` (8) slots.
  The server never opens more than `MAX_OPEN_DIRS` directories concurrently.  If all
  slots are in use, set `errno = EMFILE` and return `NULL`.
- Do not use `malloc`.  The static pool ensures the code compiles and runs on
  platforms without a heap (or with a very restricted heap).
- Initialise the `next_cookie` field in the handle to `1`.  Cookies are 1-based;
  cookie 0 is reserved to mean "start from the beginning" in `vfs_seekdir_to`.
- The returned handle remains valid until `vfs_closedir()` is called on it.
- If the path does not exist or is not a directory, return `NULL` with an appropriate
  errno (`ENOENT`, `ENOTDIR`).
- For a PDS on MVS: open the dataset and read the PDS directory into a buffer (BSAM
  OPEN + READ of the directory track).  Store the buffer and a current-position
  pointer in the `vfs_dir` struct.

---

### `vfs_readdir_next`

```c
int vfs_readdir_next(vfs_dir_t *d, char *name,
                     uint32_t maxname, uint64_t *fileid,
                     uint64_t *cookie);
```

**Purpose:** Return the next entry from an open directory iterator.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `d` | in/out | Handle returned by `vfs_opendir()`.  Internal state is advanced. |
| `name` | out | NUL-terminated filename of the entry.  Written with at most `maxname-1` characters plus a NUL terminator. |
| `maxname` | in | Size of the `name` buffer, including the NUL terminator.  Always `MAX_NAME` (256). |
| `fileid` | out | A stable 64-bit identifier for the entry.  Corresponds to the inode number as reported in directory listings. |
| `cookie` | out | A 1-based sequential position value for this entry.  Used by `vfs_seekdir_to()` to resume a directory scan. |

**Return value:** `0` if an entry was returned, `-1` when the end of directory has
been reached or on error.

**Implementation notes:**

- The function must write into `*cookie` the current value of `d->next_cookie` and
  then increment `d->next_cookie`.  This makes cookies sequential and 1-based.
- Entries must be returned in a stable order for a given open handle.  The order does
  not need to be alphabetical, but it must be consistent within one `vfs_opendir` /
  `vfs_closedir` lifetime so that `vfs_seekdir_to` can resume correctly.
- The `.` and `..` entries should be included if your storage system models them.
  Some NFS clients depend on them; others cope without.  If your system has no parent
  concept (e.g. a flat dataset library), omitting `..` is acceptable.
- `fileid` should be the same value that `vfs_stat()` would place in `fileid` for
  this entry.  It does not need to be unique across exports, only within this export.
- Truncate names longer than `maxname-1` characters.  Do not return an error for a
  long name; just silently truncate and NUL-terminate.
- Return `-1` when there are no more entries.  This is not an error; the caller
  checks this return value to detect end-of-directory.

---

### `vfs_seekdir_to`

```c
void vfs_seekdir_to(vfs_dir_t *d, uint64_t cookie);
```

**Purpose:** Reposition the directory iterator so that the next call to
`vfs_readdir_next` returns the entry **after** the one that was assigned `cookie`.
A `cookie` of `0` repositions to the very beginning.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `d` | in/out | Handle returned by `vfs_opendir()`. |
| `cookie` | in | The cookie value of the last entry already delivered to the client, or `0` to restart. |

**Return value:** None.

**Why this function exists:**

NFS READDIR and READDIRPLUS are paginated.  The server returns as many directory
entries as fit in one reply buffer, then stops.  The client resumes by sending the
cookie from the last entry it received.  `vfs_seekdir_to` restores the iterator to
that position so the next page begins with the correct entry.

**Implementation notes:**

- Cookies are the 1-based sequential values written by `vfs_readdir_next`.  Cookie
  `N` means "the Nth entry was the last one delivered; start from entry N+1".
- The reference implementation uses rewind-and-skip: call the OS equivalent of
  `rewinddir`, then call the OS equivalent of `readdir` exactly `cookie` times to
  advance past the already-delivered entries.  This is O(n) per page but correct and
  does not require keeping directory handles open between RPC calls (which would
  conflict with the static-pool design).
- If your storage system supports O(1) random access by position (e.g. a seek into a
  sorted index), you may implement direct positioning instead of rewind-and-skip.
- After seeking, set `d->next_cookie = cookie + 1` so that the next
  `vfs_readdir_next` call issues the correct cookie value.
- If `cookie` is beyond the last entry, the next `vfs_readdir_next` call should
  immediately return `-1` (end of directory).

---

### `vfs_closedir`

```c
void vfs_closedir(vfs_dir_t *d);
```

**Purpose:** Release the resources associated with an open directory handle and
return its slot to the pool.

**Parameters:**

| Parameter | Direction | Description |
|---|---|---|
| `d` | in | Handle returned by `vfs_opendir()`.  May be `NULL` (no-op). |

**Return value:** None.

**Implementation notes:**

- Must be safe to call with `NULL` (do nothing).
- Release the OS-level directory resource (POSIX `closedir`, MVS BSAM CLOSE, etc.).
- Mark the static pool slot as free so that a subsequent `vfs_opendir` call may
  reuse it.
- Do not free any memory that was not dynamically allocated.  The static pool slots
  are global variables; they are reused, not freed.

---

## Implementing for a Non-Hierarchical Filesystem

This section provides guidance for implementing the VFS layer over storage systems
that are fundamentally different from a POSIX hierarchical filesystem.

### Conceptual Mapping

The NFS protocol exposes a hierarchical namespace of files and directories.  Your
implementation must provide that illusion to the server regardless of how the
underlying storage is actually organised.

| NFS concept | Typical mapping for a flat/non-hierarchical store |
|---|---|
| Export root directory | The dataset library, volume, or catalog being exported |
| Subdirectory | A logical grouping within the library (e.g. a PDS has no subdirectories; expose it as a single-level namespace) |
| File | A member, record, object, or entry in the store |
| Filename | A member name, key, or tag |
| inode / fileid | A sequence number, member index, or hash derived from the name |
| Permissions | Fixed values (e.g. `0644` for files, `0755` for the root directory) |
| Link count | Always `1` for files; `2` for the root directory |
| Timestamps | Record creation/modification dates if available; otherwise a fixed epoch |

### Handling the `path` Argument

Every VFS function receives an absolute path string such as
`/exports/mylib/MEMBER1`.  Your implementation must parse this path to extract the
logical address in your storage system.

A simple parser for a single-level flat store:

```c
/* Given "/exports/mylib/MEMBER1", extract "MEMBER1" as the member name.
   The export root is "/exports/mylib".  This is stored in exports[].host_path. */
static const char *member_name_from_path(const char *path, const export_t *exp)
{
    size_t root_len = strlen(exp->host_path);
    if (strncmp(path, exp->host_path, root_len) != 0) return NULL;
    if (path[root_len] == '\0') return "";        /* the root itself */
    if (path[root_len] != '/')  return NULL;      /* malformed       */
    return path + root_len + 1;                   /* the member name */
}
```

### Timestamps

If your storage system records only a creation date (not an access or modification
time), map it as follows:

```c
vs->atime_sec  = creation_date_as_unix_epoch;
vs->mtime_sec  = creation_date_as_unix_epoch;
vs->ctime_sec  = creation_date_as_unix_epoch;
vs->atime_nsec = 0;
vs->mtime_nsec = 0;
vs->ctime_nsec = 0;
```

If the store records no timestamps at all, use a fixed value such as the server
startup time.  NFS clients will not cache data aggressively if mtime never advances,
but they will function correctly.

### File Identifiers (`fileid` and `raw_ino`)

Both `fileid` and `raw_ino` must be stable for the lifetime of a file.  Choose
values derived from the file's primary key in your storage system:

- A sequential member number assigned when the member is created works well.
- A 32-bit CRC or hash of the member name is acceptable if collisions are
  sufficiently unlikely.
- Do not use the name itself (it changes on rename).  Do not use a byte offset
  within the directory structure (it changes as other members are added or removed).

`fileid` (64-bit) is reported to NFS clients.  `raw_ino` (32-bit) is an internal
cache key.  They should be derived from the same source but may differ in width.

### Writes and Record-Oriented Storage

If your storage system uses fixed-length records (e.g. 80-byte card-image records
on MVS), you must implement byte-stream-to-record conversion in `vfs_pwrite`:

1. Determine which records overlap with `[offset, offset+count)`.
2. For the first and last records in that range, read the existing record, overlay
   the new bytes, and write the modified record back (read-modify-write).
3. For records entirely within the range, overwrite them completely.

Similarly, `vfs_pread` must convert from records to a byte stream by concatenating
records and optionally inserting newline characters at record boundaries.

### Directory Iteration Order

Your directory must return entries in a consistent order within one
`vfs_opendir`/`vfs_closedir` call.  The NFS protocol does not require alphabetical
order, but clients that do multi-page READDIR calls depend on the order being stable
across `vfs_seekdir_to` calls.

For a PDS, reading the directory blocks in their physical order and using the
member's position within that order as the cookie value is a reliable approach.

### Error Mapping

If your platform does not use POSIX errno values, maintain a module-local error
variable and translate your platform's error codes in `vfs_errno_to_nfs3`:

```c
/* Example: map platform error codes to NFS3 status codes directly,
   bypassing the errno layer entirely. */
uint32_t vfs_errno_to_nfs3(int err)
{
    switch (err) {
        case PLATFORM_OK:           return NFS3_OK;
        case PLATFORM_NOT_FOUND:    return NFS3ERR_NOENT;
        case PLATFORM_ACCESS:       return NFS3ERR_ACCES;
        case PLATFORM_NO_SPACE:     return NFS3ERR_NOSPC;
        case PLATFORM_READ_ONLY:    return NFS3ERR_ROFS;
        case PLATFORM_NAME_LONG:    return NFS3ERR_NAMETOOLONG;
        case PLATFORM_NOT_DIR:      return NFS3ERR_NOTDIR;
        case PLATFORM_IS_DIR:       return NFS3ERR_ISDIR;
        default:                    return NFS3ERR_IO;
    }
}
```

---

## Checklist for a New Implementation

Before declaring the port complete, verify the following:

- [ ] `vfs_stat` returns a unique, stable `fileid` for every file
- [ ] `vfs_stat` returns a unique, stable `(raw_dev, raw_ino)` pair for every file
- [ ] `vfs_stat` returns `NF3DIR` for the export root and any subdirectories
- [ ] `vfs_pread` sets `*eof = 1` when the read reaches or passes end-of-file
- [ ] `vfs_pread` returns `0` with `*nread = 0` and `*eof = 1` for reads at or beyond EOF
- [ ] `vfs_pwrite` returns `-1` on partial writes, never silently short-writes
- [ ] `vfs_pwrite` flushes to stable storage before returning
- [ ] `vfs_create` succeeds when the file already exists (truncates it)
- [ ] `vfs_rename` copes with a destination file that already exists
- [ ] `vfs_truncate` extends the file with zero bytes when `size > current size`
- [ ] `vfs_set_times` with `SET_DONT_CHANGE` does not alter the named timestamp
- [ ] `vfs_set_times` with `SET_TO_SERVER_TIME` uses the server clock, not the client value
- [ ] `vfs_opendir` returns `NULL` with `errno = EMFILE` when all pool slots are in use
- [ ] `vfs_readdir_next` issues cookies starting at `1`, incrementing by `1` each call
- [ ] `vfs_seekdir_to(d, 0)` restarts iteration from the first entry
- [ ] `vfs_seekdir_to(d, N)` causes the next `vfs_readdir_next` to return entry `N+1`
- [ ] `vfs_closedir(NULL)` is a no-op and does not crash
- [ ] All functions set errno (or the platform equivalent) before returning `-1`
- [ ] `vfs_errno_to_nfs3` never returns `NFS3_OK` for a non-zero error code
- [ ] No function calls `malloc` or `free`
- [ ] The static `g_dir_pool` holds at most `MAX_OPEN_DIRS` simultaneous handles
