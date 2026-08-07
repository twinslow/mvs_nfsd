# NFS File Handles

How mvs_nfsd builds, encodes, and resolves NFS file handles — and why the
design is what it is.

Code: `src/fhandle.c`, `our_fhandle_t` + `OUR_FHSIZE` in `src/nfsd.h`.

## 1. The rule that drives the design

RFC 1813 treats a file handle as a **durable name for an object**. A client
may hold one indefinitely and present it at any time, including after the
server restarts. `NFS3ERR_STALE` has one meaning:

> the object this handle refers to no longer exists, or access to it has
> been revoked.

It does **not** mean "the server could not look this up right now." That
distinction matters because a client treats STALE as final: it does not
retry, and the usual recovery is an unmount/remount.

So a handle must satisfy two properties:

1. **Stable** — the handle for an object never changes, including across a
   server restart.
2. **Always resolvable** — any handle we ever issued must resolve for as
   long as its object exists. STALE is reserved for objects that are
   genuinely gone.

## 2. How we satisfy them: the handle names its object

The handle **carries the object's name**. Resolving it is a pure function of
its own bytes plus the export config — there is no server-side state, no
cache, nothing to evict, and nothing to lose across a restart.

```
 byte   0        4        8                                 52        60
        +--------+--------+---------------------------------+---------+
        | magic  |export  |            dsname               | member  |
        |  (4)   | id (4) |         (44, ASCII)             |(8,ASCII)|
        +--------+--------+---------------------------------+---------+
```

| Field | Size | Contents |
|---|---|---|
| `magic` | 4 | `OUR_FH_MAGIC` = `'NFS3'` (0x4E465333), big-endian |
| `export_id` | 4 | **stable hash** of the export path, big-endian |
| `dsname` | 44 | MVS dataset name, ASCII, blank-padded |
| `member` | 8 | PDS member name, ASCII, blank-padded |

**60 bytes total** — within the NFS3 64-byte limit (`NFS3_FHSIZE`), and a
multiple of 4, so the XDR opaque encoding adds no padding.

44 + 8 are the MVS maxima for a dataset name and a member name, so **every
possible object fits without truncation** and no variable-length handle
support is needed.

### The three object kinds

Which name fields are set tells you what the handle refers to:

| Object | `dsname` | `member` | Resolves to |
|---|---|---|---|
| Export root (virtual dir listing the PDS dirs) | empty | empty | `<export_path>` |
| PDS directory | set | empty | `<export_path>/<dirname>` |
| PDS member (a file) | set | set | `<export_path>/<dirname>/<member>.<ext>` |

The **file extension is not in the handle** — it comes from the dataset's
config (`file_ext`). That is what buys the room to fit in 64 bytes.

## 3. Why `export_id` is a hash, not an index

`export_id` is `mvs_fid_ino32(export_path_ebcdic, NULL)` — a stable hash of
the export's path.

It was previously the export's **table index**. That is unsafe the moment
handles outlive a restart: editing `nfsd.conf` to add, remove, or reorder an
export shifts the indices, so an old handle would silently resolve to a
**different export** and return the wrong data. A hash either matches the
same export as before, or matches nothing — in which case the handle is
correctly stale.

The same reasoning applies to the dataset: `fh_resolve` finds it by
**matching `dsname`**, never by index.

## 4. ASCII, and the EBCDIC traps

The names are stored in the handle in **ASCII**, exactly as they travel on
the wire, so a handle is readable in a Wireshark trace. On MVS this needs
care, because under JCC a C character literal is **EBCDIC**:

- Padding/trimming uses `FH_PAD_CHAR` (`0x20`), **not** `' '` (EBCDIC 0x40).
- Case folding uses explicit ASCII ranges (`0x41`–`0x5A`), **not**
  `tolower()` or `'A'`/`'Z'` literals.
- Separators are converted: `ebcdic_to_ascii_c('/')`, `ebcdic_to_ascii_c('.')`.
- Format strings passed to `snprintf` when building a path contain **only
  conversion specifiers** — a literal `/` or `.` in the format would be
  EBCDIC and corrupt the ASCII path. Separators go through `%c`.
- `ds->file_ext` is derived from the EBCDIC dsname and so is **EBCDIC**; it
  is converted before being appended to the ASCII path.
- `ds->dsname_ascii`, `ds->dirname_ascii`, and `exp->export_path` are
  already ASCII (see `exports.c`) and are used as-is.

## 5. API

```c
int  fh_from_path(const char *abspath, our_fhandle_t *fh);
int  fh_resolve  (const our_fhandle_t *fh, char *abspath, uint32_t maxlen);
int  fh_decode   (const uint8_t *bytes, uint32_t len, our_fhandle_t *fh);
void fh_encode   (const our_fhandle_t *fh, uint8_t *bytes);
```

`fh_from_path` and `fh_resolve` are inverses: a total, stateless mapping
between an NFS path and the handle naming it.

- **`fh_from_path`** classifies the path with `mvs_path_type`, takes the
  dsname from the config, and (for a member) validates/extracts the member
  name via `mvs_get_pds_dsn_and_member`. Returns −1 only if the path lies
  outside every export.
- **`fh_resolve`** finds the export by hash, the dataset by dsname, and
  rebuilds the path. Returns −1 **only when the handle is truly stale**: its
  export is gone, or the dataset is no longer exported.

There is no `fh_init`, and no cache to prime.

## 6. What callers do

| Caller | Was | Now |
|---|---|---|
| `mount3.c` MNT | `fh_make` + `fh_cache_insert(…, "")` | `fh_from_path(exp->export_path, &fh)` |
| `nfs3.c` LOOKUP | `fh_make` + relpath build + cache insert | `fh_from_path(obj_path, &obj_fh)` |
| `nfs3.c` CREATE | same | `fh_from_path(obj_path, &obj_fh)` |
| `nfs3.c` READDIRPLUS | cache insert per entry + `"."`/`".."` relpath juggling | `fh_from_path(entry_path, &efh)` |
| `nfs3.c` RENAME | cache fixup so old handles follow the file | *(nothing)* |

RENAME needs no fixup: the renamed member's handle is simply the one derived
from its new path. A handle held for the **old** name now names an object
that no longer exists, and the client correctly gets a stale/absent answer
for that name only — which is exactly right.

## 7. When STALE is now returned

Only when it is true:

- the handle's `export_id` matches no configured export (the export was
  removed or its path changed), or
- the named dataset is no longer exported.

A handle for a **deleted member** still resolves to a path; `vfs_stat` then
fails and the client gets a not-found answer for that object alone. Note
RFC 1813 would prefer `NFS3ERR_STALE` for a handle-based operation on a
deleted object; we currently return `NFS3ERR_NOENT` via
`vfs_errno_to_nfs3`. This is a known, low-impact divergence — either way the
client invalidates just that object rather than the whole mount.

## 8. History: what was wrong before

The previous handle was 16 bytes: `magic(4) + export_id(4) + reserved(4) +
id32(4)`, where `id32` was allocated from a 512-entry path cache
(`FH_CACHE_SIZE`) that mapped `id32 -> relpath`. It had three defects:

1. **`id32` was a per-run counter** (`g_id_seq++`), not a hash. Every
   restart began at 1, so **no handle survived a restart** — every one
   returned STALE. (An older design did hash `mvs_fid_ino32(dsname,
   member)`; the README documented that long after the code had moved to a
   counter.)
2. **Eviction destroyed identity.** The cache was round-robin over 512
   entries. Once an entry was evicted, the client's handle was dead
   *forever*, and re-looking-up the same file minted a **different** handle
   for it.
3. **STALE was a lie.** A cache miss is a server bookkeeping failure, not a
   missing object — but the client, correctly believing us, could only
   recover by remounting.

Symptomatically this showed up on Linux (which holds handles far longer than
Windows) as a mount that wedged until unmount/remount.

The self-describing handle removes all three: identity is the object's name,
so it cannot churn, cannot be evicted, and cannot be forgotten.

## 9. Upgrading

The handle format and size changed, so handles issued by an older server are
rejected (`fh_decode` fails the length check). **Clients must unmount and
remount once** after this upgrade. This is a one-time cost.
