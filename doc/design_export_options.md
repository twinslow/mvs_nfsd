# Design: Read-Only Exports and Configurable Permission Bits

Status: **Proposed — not implemented.** This document is the design record;
implement after the open decisions in §10 are settled.

Author: design discussion, dino_nfs.

## 1. Goal

Two related capabilities, both configured per export line:

1. **`ro`** — a read-only dataset. Every mutating NFS operation is refused,
   and the server does not *advertise* write access either.
2. **`dirperm=` / `memperm=`** — control the POSIX mode bits reported to
   clients for a PDS directory and for its members, instead of today's
   hard-coded `0777` for everything.

Today `vfs_stat_root`, `vfs_stat_dataset`, and the member stat path all
return `0777` unconditionally (`src/mvsvfs.c`), and nothing prevents a write
to any export.

## 2. Config syntax

Two forms, both inside `[Exports]`, and both supported.

### 2.1 Flat form (today's syntax, extended)

Keywords are appended to an `[Exports]` line, after the dataset name:

```ini
[Exports]
# read-write, 0777 -- today's behaviour, unchanged
/exports    TEMP.TESTPROJ.CNTL

# read-only reference copy
/pub        SYS1.PROCLIB        ro dirperm=555 memperm=444

# writable, but conventional permissions
/exports    TEMP.TESTPROJ.C     dirperm=755 memperm=644
```

Here each line names an *(export path, dataset)* pair, so the keywords attach
to that **dataset**. Repeating an export path with different keywords is
meaningful — one mount can hold a writable directory beside a read-only one:

```ini
/exports    TEMP.TESTPROJ.C       dirperm=755 memperm=644
/exports    SYS1.MACLIB           ro
```

### 2.2 Block form (per-export)

The flat form cannot express an *export-level* setting: making a
three-dataset export read-only means repeating `ro` three times, and the
export **root** has no line of its own at all (§4.4). The block form fixes
both. It follows the brace syntax already sketched in `nfsd.conf` and
deferred in `design_multi_pds_export.md` §2.1:

```ini
[Exports]
/pub  ro rootperm=555 dirperm=555 memperm=444 {
    SYS1.PROCLIB
    SYS1.MACLIB     memperm=400
}
```

**Grammar:** everything *before* the `{` is the export path followed by
**export-level** keywords. Each line *inside* is one dataset, optionally
followed by **dataset-level** keywords. A `}` on its own line closes it.

This is deliberately unambiguous: keywords and dataset names never share a
line position, so the parser never has to guess whether `RO` is a keyword or
a (pathological) dataset name.

### 2.3 Keywords

| Keyword | Level | Meaning | Default |
|---|---|---|---|
| `ro` | export, dataset | Refuse all mutating operations | *(off — read-write)* |
| `rw` | export, dataset | Explicitly read-write (for clarity/symmetry) | *(on)* |
| `dirperm=<octal>` | export, dataset | Mode reported for a PDS directory | `777` |
| `memperm=<octal>` | export, dataset | Mode reported for its members | `777` |
| `rootperm=<octal>` | **export only** | Mode reported for the export root | `555` (§4.4) |

Values are **octal** — `755` means `0755`, not decimal 755. A leading zero
(`0755`) is accepted. Keywords are case-insensitive and order-independent.

`rootperm` is export-only because the export root belongs to the export, not
to any dataset. Using it on a dataset line is an error (§10.1).

### 2.4 Inheritance rules

Export-level keywords apply to every dataset in the block. Dataset-level
keywords then refine them, with one asymmetry:

- **`dirperm` / `memperm` are defaults** — a dataset may override them in
  either direction. They are advisory reporting, not a safety boundary.
- **`ro` is a ceiling, not a default** — a dataset may *narrow* to `ro`
  inside a read-write export, but `rw` on a dataset inside an `ro` export is
  an **error** (fail closed, §10.1). Allowing a writable dataset inside an
  export declared read-only is precisely the kind of quiet contradiction that
  makes a security setting useless.

**Inheritance is resolved once, at config-load time** — see §3.

## 3. Data model

Add to `pds_dataset_t` (`src/nfsd.h`) — the **resolved** values for one
dataset:

```c
uint8_t  readonly;   /* 1 = ro: every mutating operation fails       */
uint16_t dirperm;    /* mode reported for the PDS directory (0777)   */
uint16_t memperm;    /* mode reported for its members       (0777)   */
```

Add to `export_t` — the export-level values:

```c
uint8_t  readonly;   /* export-level ro (a ceiling on its datasets)  */
uint16_t rootperm;   /* mode reported for the export root    (0555)  */
```

### Flatten inheritance at load time

`export_t.readonly`, and the export-level `dirperm`/`memperm` defaults, are
applied to each `pds_dataset_t` **while parsing**. The VFS then only ever
reads the dataset's own resolved fields — there is no inheritance logic at
access time.

This matters: `vfs_pwrite`, `vfs_stat`, and friends are on the hot path and
already resolve `(export_idx, dataset_idx)`. Making them walk back up to the
export to compute an effective value would add cost and, worse, a second
place for the rule to be implemented slightly differently. Resolve once,
consult once.

`export_t.readonly` is retained after parsing for diagnostics and for the
load-time log line; it is not consulted per operation.

> **Trap:** `dataset_init()` does `memset(ds, 0, sizeof(*ds))`, and
> `find_or_create_export()` does `memset(&g_exports[idx], 0, ...)`. Zero is a
> *valid but catastrophic* mode (`0000` — nothing accessible to anyone), so
> the defaults **must be assigned explicitly** after each memset. A forgotten
> default here is silent at config-load and only bites at first access.

`ACCESS` also needs to know whether the object is on a read-only export
(§6.1), and it only has a `vfs_stat_t`. Add an internal field:

```c
uint32_t fs_readonly;  /* 1 = object lives on a read-only export.
                          Internal only -- NOT part of fattr3. */
```

## 4. Semantics

### 4.1 `dirperm` / `memperm`

These simply replace the hard-coded `0777` in `vfs_stat_dataset()` (PDS
directory) and the member stat path. Everything else follows for free:
`proc_access` already does `vfs_stat()` → `check_access(&st, uid, gid, …)`,
so the configured mode flows into ACCESS with no further change, and GETATTR
reports it in `fattr3.mode`.

### 4.2 `ro`

`ro` is a **filesystem property, not a permission**. This distinction is the
crux of the design:

- Permissions (`dirperm`/`memperm`) are advisory and **root may override
  them** — that is correct Unix behaviour.
- A read-only *filesystem* may **not** be overridden by anyone, including
  root.

So `ro` must be enforced independently of uid and of the mode bits. See
§6.1 — this is not a theoretical concern; it is the single most likely way to
get this feature subtly wrong.

### 4.3 Interaction: `ro` masks the reported write bits

`ro` and `memperm=666` are contradictory. The reported mode must never
advertise write on a read-only dataset, or clients will attempt writes and
fail confusingly (and `ls -l` will lie). Rule:

```
reported_mode = readonly ? (perm & ~0222) : perm
```

`ro` therefore wins over the configured bits, and `ro dirperm=755` reports
`0555`. Note this masking is **cosmetic honesty only** — enforcement does not
depend on it (§4.2).

### 4.4 The export root

The export root (`vfs_stat_root`) is a *synthetic* directory whose entries
are the PDS directories. It has no dataset, so `dirperm` does not apply — it
is a property of the **export**, which is exactly what the block form (§2.2)
gives it a home for: `rootperm=`, defaulting to **`0555`**.

`0555` is the honest default because the root **cannot be modified through
NFS at all**: MKDIR and RMDIR return `NFS3ERR_NOTSUPP`, and there is no other
way to add or remove a PDS via the protocol. Reporting `0777` for it today is
simply a lie. `rootperm=` exists so that anyone who disagrees (or who hits a
client that dislikes a non-writable mount root) can set it back without a
rebuild.

Two consequences:

- **The root is always read-only for ACCESS**, regardless of `rootperm` or of
  the export's `ro` setting: `vfs_stat_root` always reports `fs_readonly = 1`,
  so ACCESS never grants `MODIFY`/`EXTEND`/`DELETE` on it. This is not
  configurable, because it is a statement of fact about the protocol surface,
  not a policy. `rootperm` only controls the reported `r`/`x` bits.
- **This does not affect creating members.** Creating
  `/exports/temp.proj.cntl/x.cntl` is checked against the **PDS directory's**
  `dirperm`, not the root's. A `0555` root does not make a writable export
  unusable.

Changing the default from `0777` to `0555` is a visible behaviour change;
it was accepted as decision §10.2.

## 5. What `ro` must block — answering "any others?"

The three named in the request (create, rename, write) are not the full set.
The complete list of mutating paths:

| NFS proc | VFS entry point | On `ro` | Note |
|---|---|---|---|
| WRITE | `vfs_pwrite` | **fail** | named in request |
| CREATE | `vfs_create` | **fail** | named in request |
| RENAME | `vfs_rename` | **fail** | named in request; check **both** ends |
| **REMOVE** | `vfs_remove` | **fail** | **missing from the request** |
| **SETATTR** (size) | `vfs_truncate` | **fail** | **missing** — truncation is a mutation |
| **SETATTR** (times) | `vfs_set_times` | **fail** | **missing** — rewrites ISPF stats via STOW |
| **ACCESS** | *(mask)* | **mask off** `MODIFY`/`EXTEND`/`DELETE` | **missing** — see below |
| COMMIT | `vfs_commit` | allow (no-op) | nothing can be pending if writes are refused |
| MKDIR/RMDIR/LINK/SYMLINK | — | already `NOTSUPP` | nothing to do |

Two of these deserve emphasis:

- **REMOVE** is the obvious gap — a read-only export that lets you delete
  members is not read-only.
- **ACCESS** is the subtle one. It mutates nothing, so it is easy to skip —
  but if ACCESS says "you may write" and WRITE then fails, clients misbehave
  (Windows in particular reports misleading errors rather than "read-only").
  ACCESS must never grant `MODIFY`, `EXTEND`, or `DELETE` on an `ro` dataset.

**RENAME needs both ends checked.** `vfs_rename` already requires source and
target in the same PDS, so one check on the resolved dataset suffices — but
that invariant must hold. If cross-PDS rename is ever allowed, this becomes
two checks.

## 6. The two traps

### 6.1 `check_access()` grants root everything, ignoring the mode

`src/nfs3.c`:

```c
if (uid == 0) {
    /* Root: read/write/delete always; execute only if any x-bit set */
    granted = ACCESS3_READ | ACCESS3_LOOKUP | ACCESS3_MODIFY
            | ACCESS3_EXTEND | ACCESS3_DELETE;
    if (mode & 0111u) granted |= ACCESS3_EXECUTE;
    return granted & requested;
}
```

Root bypasses the mode bits entirely. Therefore **masking the mode
(§4.3) does not make ACCESS read-only for uid 0** — and this is not an edge
case: a Linux client mounting as root sends `auth_uid = 0`. (Our Windows
client sends `-2`, which is why this would look fine in Windows testing and
fail on Linux — the same asymmetry that has bitten us repeatedly.)

`check_access()` must therefore take the read-only flag and mask the write
bits **after** the root branch:

```c
static uint32_t check_access(const vfs_stat_t *st, uint32_t uid,
                             uint32_t gid, uint32_t requested)
{
    ...existing logic, unchanged...

    /* A read-only filesystem is not a permission: no uid overrides it. */
    if (st->fs_readonly)
        granted &= ~(ACCESS3_MODIFY | ACCESS3_EXTEND | ACCESS3_DELETE);

    return granted & requested;
}
```

That requires restructuring the root branch to fall through to a common exit
rather than returning early.

### 6.2 `EROFS` does not exist in JCC — **confirmed**

`vfs_errno_to_nfs3()` has:

```c
//        case EROFS:         return NFS3ERR_ROFS;
```

commented out, alongside the other errnos JCC lacks (`EFBIG` is explicitly
annotated *"EFBIG not in JCC"*). **JCC's `errno.h` does not define `EROFS`**
(confirmed). `NFS3ERR_ROFS` (30) **is** defined in `nfsd.h`, so the wire code
is available; only the errno is missing.

This rules out the obvious `#ifdef EROFS` shape used for `EXDEV` in
`vfs_rename`: on MVS it would *always* fall back, so `NFS3ERR_ROFS` would
never be emitted.

**The fix is simpler than it first appears.** Reading JCC's `errno.h`
(<https://github.com/mvslovers/jcc/blob/main/include/errno.h>) shows it
follows **standard POSIX/Linux errno numbering**, but with **gaps**:

```c
#define ENOSPC          28        /* No space left on device */
#define ESPIPE          29        /* Seek not available */
/* -- nothing at 30, 31, 32 -- */
#define EDOM            33
...
#define EREMOTE         71        /* highest value defined */
```

In POSIX numbering, **30 is exactly `EROFS`** (31 is `EMLINK`, 32 is
`EPIPE`). JCC simply omits them; the slot is unused. So we do not need a
private sentinel or a scattered check — just supply the canonical value:

```c
/*
 * JCC's errno.h omits EROFS.  30 is its standard POSIX value and the slot
 * is unused in JCC (it defines 28, 29, then jumps to 33), so defining it
 * cannot collide.  The #ifndef defers to JCC if it ever adds it -- and
 * since JCC follows POSIX numbering, it would add it as 30 anyway.
 */
#ifndef EROFS
#define EROFS   30
#endif
```

Then the commented-out line in `vfs_errno_to_nfs3()` is simply uncommented,
with no `#ifdef` guard needed:

```c
case EROFS:         return NFS3ERR_ROFS;
```

and the VFS layer sets `errno = EROFS` normally. This gives the exact error
*and* the single VFS choke point, with no hack:

| # | Approach | Assessment |
|---|---|---|
| 1 | `errno = EACCES` → `NFS3ERR_ACCES` | Zero machinery, but permanently reports the wrong error. |
| 2 | Check in the NFS3 procs, return `NFS3ERR_ROFS` directly | Exact error, but spreads the check across ~6 procs — one omission is a **silent hole in a security feature**. Rejected. |
| 3 | Private errno sentinel | Unnecessary now — the canonical value is free. |
| 4 | **`#ifndef EROFS` → 30** | **Recommended.** Canonical value, no collision, single choke point, defers to JCC if it ever adds it. |

**Why the exact error matters:** we have been bitten before by a
technically-defensible-but-wrong NFS error producing a baffling client
message (`NFS3ERR_INVAL` surfacing on Windows as *"The volume for a file has
been externally altered"*). `NFS3ERR_ROFS` renders as *"Read-only file
system"* / *"The media is write protected"* — which is exactly what happened.
`NFS3ERR_ACCES` ("Access is denied") sends the user hunting for a permissions
problem that does not exist.

Note the mitigation that makes this a **backstop rather than the primary
signal** regardless: ACCESS masking (§5) means a well-behaved client never
attempts the write at all. The error only surfaces for clients that try
anyway.

### 6.3 The same gap affected the shipped `vfs_rename` — **FIXED**

The same reading showed **`EXDEV` is also absent from JCC** — and 18 (its
POSIX value) is likewise a free slot (JCC defines 17 `EEXIST`, then jumps to
20 `ENOTDIR`).

This was a **live defect in already-shipped code**. `vfs_rename` had:

```c
#ifdef EXDEV
    errno = EXDEV;
#else
    errno = EINVAL;   /* JCC may lack EXDEV; INVAL is the closest we map */
#endif
```

Since JCC lacks `EXDEV`, that always compiled to `EINVAL` → **`NFS3ERR_INVAL`**
— the very code with the notorious Windows rendering. A cross-PDS rename
produced a baffling message instead of `NFS3ERR_XDEV`, which is what prompts
a client to fall back to copy+delete. It also made the README and
`design_nfs_write.md` factually wrong, since both state that a cross-PDS
rename returns `NFS3ERR_XDEV`.

**Fixed** (`src/mvsvfs.c`): `#ifndef EXDEV / #define EXDEV 18`, the `#ifdef`
fallback dropped from `vfs_rename`, and `case EXDEV: return NFS3ERR_XDEV;`
uncommented in `vfs_errno_to_nfs3()`. The docs' claim is now true.

`EROFS` (§6.2) will use the identical mechanism and join it in the same
comment block.

## 7. Where enforcement lives

**In the VFS layer**, not the NFS layer. Every mutating operation already
resolves `(export_idx, dataset_idx)` there, so the check is two lines each,
and the VFS is the choke point that cannot be bypassed by a future caller.

One helper keeps it honest:

```c
/* Returns 0 if the dataset may be modified, -1 (errno set) if it is ro. */
static int mvs_check_writable(int export_idx, int dataset_idx);
```

called at the top of `vfs_pwrite`, `vfs_create`, `vfs_remove`, `vfs_rename`,
`vfs_truncate`, and `vfs_set_times` — immediately after the dataset is
resolved and before any state is touched. In particular it must run **before**
`pww_*` is consulted, so a refused write never creates or dirties a pending
slot.

## 8. Parser changes

### 8.1 Tokenising

The current `[Exports]` parser takes the **rest of the line** as the dataset
name:

```c
rest = p;                       /* second token: PDS dataset name (rest of line) */
len = (int)strlen(rest);
while (len > 0 && isspace((unsigned char)rest[len-1])) rest[--len] = '\0';
```

With keywords this must become proper tokenisation: token 1 = export path,
token 2 = dataset name, tokens 3..n = keywords. That is safe — an MVS dataset
name cannot contain blanks, so nothing is lost by tokenising.

A shared `cfg_parse_keywords(char *toks[], int n, cfg_opts_t *out, int level)`
serves both levels; `level` rejects `rootperm` on a dataset line and `rw`
inside an `ro` export (§2.4). Per token:

- `ro` → `readonly = 1`
- `rw` → `readonly = 0`
- `dirperm=<v>` / `memperm=<v>` / `rootperm=<v>` → parse `<v>` as octal (§9)
- anything else → **fail the export** (§10.1)

### 8.2 Block state

`[Exports]` gains a small amount of state, since a line's meaning now depends
on whether a block is open:

```
in_block == 0:
    "<path> <dsname> [kw...]"   -> flat form, one dataset
    "<path> [kw...] {"          -> open a block; record the export-level opts
    "}"                         -> error: unbalanced

in_block == 1:
    "<dsname> [kw...]"          -> one dataset, inheriting the block's opts
    "}"                         -> close the block
    "<path> ... {"              -> error: nested blocks are not supported
```

Cases that must be errors rather than silently tolerated:

- **EOF with a block still open** — an unterminated `{` would otherwise
  silently swallow the remaining datasets (or, worse, subsequent sections).
- **A `}` with no open block.**
- **A section header inside an open block** — the `[Init]`/`[Exports]`
  dispatcher must not switch sections mid-block.

All three are config errors; per §10.1 they fail the affected export rather
than being warned past.

### 8.3 Interaction with the section dispatcher

Block state lives inside the `[Exports]` handler, not the section loop, so
adding a section later cannot disturb it. The `[Exports]` handler simply
needs to be told when the section ends (EOF or a new header) so it can detect
the unterminated-block case above.

## 9. Validation

**Octal values.** Parse with `strtol(v, &end, 8)`. This must reject:

- non-octal digits (`dirperm=778`, `dirperm=999`) — `strtol` base 8 stops at
  the bad digit, so **`*end != '\0'` must be checked**, or `778` silently
  becomes `07` = `0007`;
- an empty value (`dirperm=`);
- values above `0777` — no setuid/setgid/sticky bits.

**Sanity warning.** A `dirperm` with no execute bits (e.g. `644`) produces a
directory clients cannot traverse. Honour it, but warn — it is almost always
a typo.

## 10. Decisions

### 10.1 Bad keyword or bad value: fail the export — **DECIDED**

An unrecognised keyword, an unparseable/out-of-range value, a keyword used at
the wrong level (`rootperm` on a dataset), `rw` inside an `ro` export, or a
malformed block (§8.2) **fails the affected export**: log an error and do not
create it.

Rationale: the failure modes are asymmetric. A typo
(`/pub SYS1.PROCLIB read-only`) that is warned-and-ignored yields a
**writable export of data intended to be read-only** — silently, with a
warning that is easy to miss on a busy console. Refusing to export it fails
safe and is impossible to miss.

*Deliberate inconsistency:* this differs from the section handling in
`readme_config.md`, which warns-and-skips unknown sections for forward
compatibility. An unknown *section* cannot make data writable; an unknown
*keyword* can. The stakes, not the mechanism, drive the difference — and that
reasoning should be recorded in the code comment so it does not later look
like an oversight.

"Fails the export" means the whole export, not just the offending dataset: a
partially-applied export (`ro` on two of three datasets because the third
line had a typo) is a worse outcome than a missing one.

### 10.2 Export root defaults to `0555` — **DECIDED**

Accepted (§4.4). Configurable via the export-level `rootperm=`; always
read-only for ACCESS regardless.

### 10.3 Per-export block form — **DECIDED**

Accepted (§2.2), using the brace syntax already sketched in `nfsd.conf` and
deferred in `design_multi_pds_export.md` §2.1. The flat form remains
supported unchanged.

### 10.4 `EROFS` is absent from JCC — **DECIDED: define it as 30**

JCC's `errno.h` does not define `EROFS`, so the `#ifdef EROFS` shape (as used
for `EXDEV` in `vfs_rename`) would always fall back and `NFS3ERR_ROFS` would
never be emitted.

But JCC follows POSIX numbering with gaps, and **30 — the canonical `EROFS`
value — is unused** (it defines 28, 29, then jumps to 33). So we simply
supply it under `#ifndef` and uncomment the existing mapping (§6.2). No
sentinel, no scattered checks, canonical value, and it defers to JCC if that
ever adds it.

Follow-up spun out of this: `EXDEV` has the same gap and the shipped
`vfs_rename` is silently degrading to `NFS3ERR_INVAL` as a result — see §6.3.

### 10.5 `ro` at MOUNT — **DECIDED: no**

NFSv3's `MOUNTPROC3_MNT` reply has no read-only flag (that is an
NFSv2/`exports(5)` notion), so clients learn it via ACCESS and errors.
Nothing to do; log the setting at load time instead.

## 11. Future extensions

- **`uid=` / `gid=`** to report an owner other than `0`, which would make the
  non-root branch of `check_access()` meaningfully useful. Today everything is
  `uid 0`, so on Linux (which mounts as root) only the §6.1 branch ever runs.
- Per-dataset `fileext=` (already sketched in `nfsd.conf`), which the block
  form now has a natural place for.
- Nested/inherited defaults across exports (a `[Defaults]` section). Only
  worth it if the config grows large.

## 12. Test plan

**Unit-testable (pure, in `tests/`):**

- Keyword parsing: `ro`, `rw`, octal values, leading zero, case-insensitive,
  order-independent.
- Octal rejection: `778`, `999`, `1777`, empty, trailing junk.
- Defaults: no keywords → `readonly=0`, `dirperm=0777`, `memperm=0777`,
  `rootperm=0555` (the memset trap in §3).
- Mode masking: `ro` + `memperm=666` → reported `0444`.
- **Inheritance flattening** (§2.4): export `ro dirperm=555` + dataset
  `memperm=400` → dataset resolves to `readonly=1, dirperm=0555,
  memperm=0400`; dataset `dirperm=755` overrides the export default.
- **Ceiling enforcement**: `rw` on a dataset inside an `ro` export is an
  error, not an override.
- **Level enforcement**: `rootperm` on a dataset line is an error.
- **Block parsing** (§8.2): unterminated `{` at EOF, stray `}`, nested `{`,
  and a section header inside an open block are each errors.
- **Fail-closed scope** (§10.1): a bad keyword on the third dataset of a
  block fails the *whole* export — assert the export is absent, not
  partially applied.
- Flat form unchanged: existing `[Exports]` lines with no keywords produce
  exactly today's table.

**On-MVS integration:**

- `ro` export: WRITE, CREATE, REMOVE, RENAME, SETATTR(size), SETATTR(times)
  each rejected; READ, LOOKUP, READDIR, GETATTR still work.
- **ACCESS as root from Linux** (`auth_uid=0`) on an `ro` export must not
  grant `MODIFY`/`EXTEND`/`DELETE` — this is the §6.1 trap and Windows
  testing will *not* catch it.
- `dirperm=755 memperm=644` reflected in `ls -l` and in ACCESS.
- A read-write export still behaves exactly as before (default path).

## 13. Out of scope

- Per-client access control (host/netgroup rules). There is still no
  authentication; `ro` is a property of the export, not of the client.
- Enforcing permissions against the *actual* MVS security product
  (RACF/TopSecret). `dirperm`/`memperm` are what the server *reports*; the
  underlying dataset access is whatever the started task's authority allows.
  A dataset can be `ro` here and still writable by the STC through TSO/ISPF —
  and out-of-band changes are still picked up by the directory-signature
  refresh, which `ro` does not disable.
- Changing mode/uid/gid via SETATTR (still silently ignored on read-write
  exports; on `ro` the whole SETATTR is refused).
