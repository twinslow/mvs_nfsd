# mvsfsz — PDS Member File-Size Cache

## Purpose

When an NFS client asks for the size of a PDS member, the size it expects
is the number of bytes the member would produce when read as a stream of
text — not the on-disk size stored in the PDS directory or reported by the
VTOC.

The discrepancy arises because MVS stores PDS members as fixed-length
records (LRECL, typically 80 bytes).  When a JCC `fopen(...,"rt")` reads
such a member in text mode:

- Trailing spaces on each record are stripped.
- A newline (`\n`) is appended after each record.

The resulting byte count is therefore smaller than `num_records × LRECL`
and varies per member.  The ISPF statistics field called *size* contains
only the line count, not the byte count.

`mvsfsz` solves this by reading the member once in text mode, counting the
bytes, and caching the result.  Subsequent NFS requests can return the
cached value instantly without reopening the member.

---

## Cache Design

### Storage

The cache is a set of three parallel static arrays, each with
`MVSFSZ_CACHE_CAPACITY` (2000) elements:

| Array | Type | Description |
|---|---|---|
| `g_cache[]` | `mvsfsz_entry_t` | Entry data (dsname, member, file size, validity fields) |
| `g_in_use[]` | `int` | 1 = slot occupied, 0 = free |
| `g_last_used[]` | `time_t` | Wall-clock timestamp of the last access or update |

The key for each entry is the pair `(dsname, member_name)`.  Lookups are
linear scans over `g_in_use[]`; with 2000 entries this is fast enough for
normal NFS workloads.

### Validity Fields

Each cache entry stores four fields copied from the `pds_member_entry_t`
at the time the entry was created:

| Cache field | Source (`pds_member_entry_t`) | Meaning |
|---|---|---|
| `ispf_size` | `size` | ISPF line count |
| `ispf_mtime` | `chgdate` | ISPF modification date |
| `ttr_tt` | `first_block_tt` | TT component of the member's TTR |
| `ttr_r` | `first_block_rec` | R component of the member's TTR |

When `mvsfsz_get_member_size()` is called, these four fields are compared
against the live `pds_member_entry_t` supplied by the caller.  If all four
match, the cached file size is returned.  If any field differs, the entry
is evicted and the member is re-read.

The TTR uniquely identifies the starting location of a member's data on
DASD.  If a member is replaced (deleted and re-created, or saved over by
ISPF) the TTR will change, guaranteeing that a stale cached size is never
returned.

### LRU Eviction

When the cache is full and a new entry must be stored, the entry whose
`g_last_used[]` timestamp is oldest is evicted to make room.  This is the
classic Least Recently Used (LRU) policy.  `g_last_used[]` is updated:

- On every `mvsfsz_put()` call (write).
- On every validated cache hit in `mvsfsz_get_member_size()` (read).

The eviction is performed inside `find_free_slot()` and is transparent to
callers — `mvsfsz_put()` always returns 0 (never -2 as in older code).

---

## MVS File Opening

When a cache miss or stale entry forces a re-read, the member is opened
with:

```c
fopen("//DSN:<dsname>(<member>)", "rt")
```

The `//DSN:` prefix is the JCC convention for opening an MVS dataset by
name.  The member name is enclosed in parentheses.  The `"rt"` mode
selects text mode, which strips trailing spaces and appends `\n` per
record — exactly matching what an NFS client would see.

The file is consumed with a `fread()` loop counting total bytes, then
closed.  The byte count is stored in the cache via `mvsfsz_put()`.

Buffer size during the read is 512 bytes; this affects only throughput, not
the final byte count.

---

## API Reference

### Initialisation

```c
void mvsfsz_init(void);
```

Clears all cache arrays and resets the entry count to zero.  Must be
called once at program startup before any other `mvsfsz_*` function.

---

### Primary Function

```c
int mvsfsz_get_member_size(
    const char         *dsname,
    const char         *member_name,
    pds_member_entry_t *member_entry,
    uint64_t           *file_size_out);
```

Returns the NFS-visible (text-mode) byte count of a PDS member.

**Parameters:**

- `dsname` — EBCDIC PDS dataset name, NUL-terminated, at most 44 chars.
- `member_name` — EBCDIC PDS member name, NUL-terminated, at most 8 chars.
- `member_entry` — Pointer to the live PDS directory entry for this member.
  Used for cache validation and for populating validity fields when a new
  cache entry is created.
- `file_size_out` — Receives the byte count on success.

**Returns:**

- `0` — success; `*file_size_out` is valid.
- `-1` — the member file could not be opened or read (only possible on a
  cache miss or stale entry).

**Behaviour:**

1. Look up `(dsname, member_name)` in the cache.
2. If found and all four validity fields match `member_entry`: set
   `*file_size_out = g_cache[idx].file_size`, update `g_last_used[idx]`,
   return 0.
3. If found but any validity field differs: evict the entry.
4. Open `//DSN:<dsname>(<member>)` in text mode (`"rt"`).
5. If `fopen` fails: return -1.
6. Read until EOF, accumulating the byte count.
7. Call `mvsfsz_put()` with the new byte count and the validity fields
   from `member_entry`.
8. Set `*file_size_out = computed_size`, return 0.

---

### Cache Primitives

```c
int mvsfsz_put(
    const char *dsname,
    const char *member_name,
    uint64_t    file_size,
    uint16_t    ttr_tt,
    uint8_t     ttr_r,
    int32_t     ispf_size,
    int32_t     ispf_mtime);
```

Inserts or updates the entry for `(dsname, member_name)`.  If the key
already exists, all fields are updated in place.  If the cache is full,
the LRU entry is evicted first.  Always returns 0.

---

```c
int mvsfsz_get(
    const char     *dsname,
    const char     *member_name,
    mvsfsz_entry_t *entry_out);
```

Copies the cached entry into `*entry_out`.  Returns 0 if found, -1 if not.
Does **not** update `g_last_used[]` (unlike `mvsfsz_get_member_size()`).

---

```c
int mvsfsz_invalidate(
    const char *dsname,
    const char *member_name);
```

Removes the entry for `(dsname, member_name)` from the cache.  Returns 0
if found and removed, -1 if not found.

---

```c
int mvsfsz_count(void);
```

Returns the number of entries currently in the cache (0–2000).

---

```c
int mvsfsz_load(const char *filename);
```

Populates the cache from a text file.  Each data line must have the format:

```
<dsname>  <member_name>  <file_size>
```

Lines beginning with `#` are comments and are skipped.  Blank lines and
lines that do not parse as exactly three whitespace-delimited fields are
silently skipped.

All validity fields (`ttr_tt`, `ttr_r`, `ispf_size`, `ispf_mtime`) are set
to zero for every loaded entry.  This means `mvsfsz_get_member_size()` will
always treat loaded entries as stale on first access and re-read the member.
The purpose of `mvsfsz_load()` is to pre-warm the cache with approximate
sizes when a more exact recomputation is acceptable on first access.

Returns the number of entries successfully stored, or -1 if the file could
not be opened.

See [`jcl/tstfsiz.jcl`](../jcl/tstfsiz.jcl) for a sample input file.

---

## Usage Example

```c
#include "mvsfsz.h"
#include "mvspdir.h"

/* Called once at NFS server startup. */
void nfsd_startup(void)
{
    mvsfsz_init();
    /* Optionally pre-warm the cache from a saved size file. */
    mvsfsz_load("//DSN:SYS1.NFSD.SIZEFILE");
}

/* Called by the NFS GETATTR handler for a PDS member. */
int nfsd_getattr_member(
    const char         *dsname,
    const char         *member_name,
    pds_member_entry_t *dir_entry,
    uint64_t           *size_out)
{
    return mvsfsz_get_member_size(dsname, member_name, dir_entry, size_out);
}
```

---

## Unit Tests

Tests are in [`tests/tmvsfsz.c`](../tests/tmvsfsz.c) and are registered
under the `/mvsfsz` suite prefix with seven sub-suites:

| Sub-suite | What it covers |
|---|---|
| `/init` | `mvsfsz_init()` zeroes the count and makes `mvsfsz_get()` return -1 |
| `/put` | Insert, upsert, count management |
| `/get` | Field retrieval, key mismatches |
| `/invalidate` | Entry removal, count decrement, slot reuse |
| `/lru` | Cache fills to 2000; next put evicts rather than failing |
| `/get_member_size` | Cache hits (valid), stale detection for all four fields, fopen failure on cache miss |
| `/load` | Comment skipping, malformed lines, field zeroing, sample file |

Tests that require writing a temporary file (the `/load` sub-suite) use
`/tmp/tmvsfsz_load.txt`.  If `fopen("/tmp/...", "w")` fails — as it will
on MVS — those individual tests return `MUNIT_SKIP` automatically.

`/get_member_size` tests that trigger a cache miss use the dataset name
`ZZZZZ.NOTEXIST`, which does not exist on any system.  `fopen()` fails and
`mvsfsz_get_member_size()` returns -1, which is the correct result.  This
makes the stale/miss tests safe to run on both Linux and MVS without any
real DASD allocation.
