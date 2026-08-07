# Design: Multiple PDS Datasets per Export

Status: **Implemented** — landed on the `multipds` branch; compiles and
runs on MVS.  This document is retained as the design record; a few
implementation details differ from the original draft (noted inline).
Author: design discussion, mvs_nfsd.

## 1. Goal

Allow a single NFS export to contain **multiple PDS datasets**. Each PDS
appears as a directory under the export root, named as the **lower-case**
form of the dataset name; its members appear inside that directory exactly
as they do today.

Example, for an export mounted at `/export`:

```
/                             (export root  — virtual directory)
├── temp.testproj.jcllib/     (PDS  TEMP.TESTPROJ.JCLLIB)
│   ├── member1.jcl
│   └── member2.jcl
├── temp.testproj.cntl/       (PDS  TEMP.TESTPROJ.CNTL)
│   └── ...
└── temp.testproj.loadlib/    (PDS  TEMP.TESTPROJ.LOADLIB)
    └── ...
```

## 2. Decisions taken (from review)

1. **Config syntax:** repeated export-path lines (one PDS per line). The
   explicit block syntax already sketched in `nfsd.conf` is deferred as a
   possible future enhancement.
2. **Namespace:** the clean **3-level model for every export**, including
   single-PDS exports (no "flatten when one PDS" special case).
3. **Root iterator:** a **separate, small root-directory iterator**, kept
   distinct from the PDS member-reading code.
4. **Dataset source:** the initial dataset list is **static, from config**.
   A **dynamic (catalog-discovered) list is a likely future extension**, so
   the root iterator is designed against a small *dataset-provider*
   abstraction so a catalog-backed implementation can be dropped in later
   without touching the NFS/VFS layers. (Catalog listing is not present in
   the code today and would be a significant addition.)

## 3. Current architecture (baseline)

For context, how the server works before this change:

- **One export = one PDS.** `export_t` (`src/nfsd.h`) carries a single
  `host_path` (the PDS dsname), one `file_ext`, one `dcbinfo`.
- **The mount root *is* the PDS.** relpath `""` resolves via `fh_resolve`
  (`src/fhandle.c`) to `host_path` = dsname, which `mvs_path_type`
  (`src/mvsio.c`) classifies as `MVS_PATH_TYPE_DATASET`; readdir lists its
  members directly. Only **2 levels** exist (PDS-as-root, members).
- **Canonical internal path is dsname-based**, e.g.
  `TEMP.TESTPROJ.JCLLIB/MEMBER`; `mvs_path_type` string-matches it against
  `host_path_ebcdic`.
- **Identity:** `fsid`/`raw_dev` = `export_idx + 1`; `fileid`/`raw_ino` =
  `mvs_fid_hash(dsname, member)` (`src/mvsfid.c`). READDIR cookies encode
  the 8-character EBCDIC member name (`to_cookie`/`from_cookie`,
  `src/mvsvfs.c`).

Two baseline assumptions must change: *"an export has one host_path"* and
*"the internal path is a dsname."*

## 4. Namespace model

Three levels, identified by the **export-relative path** (the relpath already
stored in the fh cache, `src/fhandle.c`):

| relpath                              | level        | maps to                         |
|--------------------------------------|--------------|---------------------------------|
| `""`                                 | ROOT         | virtual directory (the export)  |
| `temp.testproj.jcllib`               | PDS_DIR      | one PDS in the export           |
| `temp.testproj.jcllib/member.jcl`    | PDS_MEMBER   | one member of that PDS          |

Path components are delimited by `/`. The dots inside a PDS directory name
are **part of the name**, not an extension.

## 5. Data model changes

### 5.1 New per-dataset struct (`src/nfsd.h`)

```c
typedef struct {
    char           dsname_ebcdic[45];  /* real PDS name, e.g. TEMP.TESTPROJ.JCLLIB */
    char           dsname_ascii[45];
    char           dirname[45];        /* lower-case(dsname) shown to clients        */
    char           file_ext[MAX_FILE_EXT_LEN];
    mvs_dcb_info_t dcbinfo;
} pds_dataset_t;
```

### 5.2 Revised `export_t` (`src/nfsd.h`)

```c
typedef struct {
    char          export_path[MAX_PATH];
    char          export_path_ebcdic[MAX_PATH];
    pds_dataset_t datasets[MAX_PDS_PER_EXPORT];
    int           ndatasets;
} export_t;
```

The per-PDS fields (`host_path`, `file_ext`, `dcbinfo`) logically belong in
`pds_dataset_t`. New limit `MAX_PDS_PER_EXPORT`.

> **Implementation note:** `export_t` *retains* the legacy single
> `host_path`/`host_path_ebcdic`/`file_ext`/`dcbinfo` fields in addition to
> `datasets[]`, populated from `datasets[0]`.  This is required because
> `mockvfs.c` is compiled (though not linked) on MVS and still references
> `host_path`.  The MVS-linked code uses `datasets[]` exclusively; the
> legacy fields exist only to keep the mock compiling.  `pds_dataset_t`
> also stores both EBCDIC and ASCII forms of the dsname and dirname
> (`dsname_ebcdic`/`dsname_ascii`/`dirname_ebcdic`/`dirname_ascii`) so the
> path matcher (EBCDIC) and the readdir emitter (ASCII) each have the form
> they need.

`dirname` is computed once at load time: lower-case of the dsname, dots
retained. Lower-cased dsnames are collision-free within an export because
MVS dataset names are upper-case, so no two distinct datasets share a
`dirname`.

## 6. Configuration (`src/exports.c`, `nfsd.conf`)

Backward-compatible **repeated export path**: consecutive (or scattered)
lines with the same export path each append a dataset to that export.

```
# nfs-export-path      pds-dataset-name
/export    TEMP.TESTPROJ.JCLLIB
/export    TEMP.TESTPROJ.CNTL
/export    TEMP.TESTPROJ.LOADLIB
```

Loader behaviour:
- On each line, look up an existing export by `export_path`. If found,
  append the dataset to it; otherwise create a new export and append.
- For each dataset, derive `file_ext` from the last dsname qualifier
  (unchanged logic), and derive `dirname = lower-case(dsname)`.
- Fetch `dcbinfo` per dataset (today done once per export in
  `exports_get_dcb_info`).

A single-line export still works and simply has `ndatasets == 1`; it now
presents one PDS directory under the root (see §11, behaviour change).

**Future (not now):** the block syntax already hinted at in `nfsd.conf`
(`/export/... { DSN fileext="jcl" ... }`) would allow explicit per-dataset
`fileext`. The current design derives `file_ext` from the last qualifier, so
adopting the block form later mainly changes the parser, not the data model.

## 7. Path resolution (central refactor)

Switch the internal canonical path from **dsname-based** to
**export-relative**. `fh_resolve` (`src/fhandle.c`) stops prepending
`host_path` (there is no single one) and yields the export-relative path
instead; the fh-cache relpath semantics in §4 already support this.

### 7.1 `mvs_path_type` (`src/mvsio.c`) — reworked

Inputs: the path. Outputs: level, `export_idx`, and `dataset_idx`.

1. Match the path to an export by `export_path` prefix → `export_idx`.
2. Split the remainder on `/` into components.
3. Classify:
   - 0 components → **ROOT**
   - 1 component  → **PDS_DIR**: resolve the component against the export's
     `datasets[].dirname` → `dataset_idx` (ENOENT if no match)
   - 2 components → **PDS_MEMBER**: first component → `dataset_idx`, second
     → member name
   - more → ENOENT (no deeper nesting)

New enum value `MVS_PATH_TYPE_ROOT` alongside the existing
`MVS_PATH_TYPE_DATASET` (= PDS_DIR) and `MVS_PATH_TYPE_PDS_MEMBER`.

### 7.2 `mvs_get_pds_dsn_and_member` (`src/mvsio.c`) — reworked

Given `(export_idx, path)`:
- Resolve the PDS-dir component to `dataset_idx` and return the **real
  dsname** from `datasets[dataset_idx].dsname_ebcdic`.
- Return the member name (upper-cased, ≤ 8 chars) from the leaf component,
  applying extension stripping **only at the member component** and
  validating the extension against **that dataset's** `file_ext`.

Correctness note: extension handling must never be applied to the PDS-dir
component (which legitimately contains dots). Splitting on `/` before any
dot handling guarantees this.

### 7.3 Request flow (end to end)

The layers a request passes through, and where the 3-level decision is made:

```
                    NFS client (Linux)
                          │  RPC (fh, args)
                          ▼
   ┌─────────────────────────────────────────────────┐
   │ nfs3.c  proc_lookup / proc_readdirplus / proc_* │
   │   dir_fh ──► fh_resolve() ──► export-rel path   │
   │   dir_fh ──► fh_cache_lookup() ──► relpath      │
   └───────────────┬─────────────────────────────────┘
                   │ path (export-relative)
                   ▼
   ┌─────────────────────────────────────────────────┐
   │ mvsvfs.c  vfs_stat / vfs_opendir / vfs_readdir_*│
   │           vfs_pread                             │
   └───────────────┬─────────────────────────────────┘
                   │ classify
                   ▼
   ┌─────────────────────────────────────────────────┐
   │ mvsio.c  mvs_path_type(path)                    │
   │   split on '/'  ─►  (level, export_idx, ds_idx) │
   └───────────────┬─────────────────────────────────┘
        ┌──────────┼────────────────────────┐
        ▼          ▼                        ▼
     ROOT        PDS_DIR                 PDS_MEMBER
   (relpath="") (1 component)           (2 components)
        │          │                        │
        ▼          ▼                        ▼
  root iterator   member iterator      member read
  (dataset        mvs_read_pds_dir     mvs_pds_member_*
   provider,      + growable list      (mvsprw.c)
   §9.3)          (mvspdir.c)               │
        │          │                        │
        └──────────┴───────── real dsname ──┘
              via datasets[ds_idx].dsname_ebcdic
```

**Example A — `ls /` (READDIRPLUS at root):**

```
proc_readdirplus(root_fh)
  └─ fh_resolve            → relpath ""              (level ROOT)
     └─ vfs_opendir        → root handle (level=ROOT)
        └─ vfs_readdir_next (root iterator, cookie = ordinal index)
           └─ export_dataset_get(export_idx, i)  →  datasets[i]
              → emit entry: name = datasets[i].dirname, ftype = DIR
              → per-entry vfs_stat("/<dirname>")  →  DIR attrs
                 (fileid = mvs_fid_hash(real_dsname, NULL))
```

**Example B — `ls /temp.testproj.jcllib` (READDIRPLUS at a PDS dir):**

```
proc_readdirplus(pdsdir_fh)
  └─ fh_resolve            → relpath "temp.testproj.jcllib"  (level PDS_DIR)
     └─ mvs_path_type      → dirname → ds_idx → real dsname TEMP.TESTPROJ.JCLLIB
        └─ vfs_opendir      → PDS handle (level=PDS_DIR), opens dsname
           └─ vfs_readdir_next (member iterator, cookie = 8-char member name)
              └─ mvs_read_pds_dir → members appended to growable list
                 → emit entry: name = generate_file_name(member, ds.file_ext)
```

**Example C — `cat /temp.testproj.jcllib/member.jcl` (LOOKUP then READ):**

```
proc_lookup(pdsdir_fh, "member.jcl")
  └─ make_child_relpath("temp.testproj.jcllib", "member.jcl")
     → relpath "temp.testproj.jcllib/member.jcl"        (level PDS_MEMBER)
  └─ mvs_get_pds_dsn_and_member
     → real dsname TEMP.TESTPROJ.JCLLIB, member MEMBER   (ext ".jcl" validated)
  └─ vfs_stat → member attrs; fh built from (export_id, dsname+member ino)

proc_read(member_fh, offset, count)
  └─ fh_resolve → same relpath → vfs_pread → mvs_pds_member_read (mvsprw.c)
```

## 8. File identity

| Object    | fileid / raw_ino source                    |
|-----------|--------------------------------------------|
| Root      | `mvs_fid_hash(export_path, NULL)` (or a reserved per-export constant) |
| PDS dir   | `mvs_fid_hash(real_dsname, NULL)`          |
| Member    | `mvs_fid_hash(real_dsname, member)`        |

- `fsid`/`raw_dev` remain per-export (`export_idx + 1`).
- Because `raw_ino` derives from the **real dsname**, `(dev, ino)` stays
  unique across all datasets and members within an export — provided the
  resolver always hashes the *real dsname*, not the `dirname`.
- The root needs its own stable, unique id distinct from any PDS.

## 9. VFS operations (`src/mvsvfs.c`)

### 9.1 `vfs_stat`

Add the ROOT case (virtual-directory attributes, reusing the stable-epoch
timestamp approach already in `vfs_stat_dataset` so the directory mtime does
not change between calls — see `doc`/the READDIRPLUS loop fix). PDS_DIR and
MEMBER as today, keyed on the resolved real dsname.

### 9.2 Directory iteration — two distinct iterators

- **PDS-dir iteration:** unchanged. `vfs_opendir`/`vfs_readdir_next` read
  members via `mvs_read_pds_dir` into the growable member list, using the
  existing 8-char EBCDIC-name cookie.

- **Root iteration (new, separate):** a small root iterator that enumerates
  the export's datasets and emits one directory entry per PDS
  (`name = dirname`, ftype DIR). It performs **no PDS I/O**. Kept in its own
  function(s) for separation of concerns.

  - `vfs_dir_t` gains a **level/kind** field so `vfs_opendir` can record
    whether the handle is a root or a PDS directory, and
    `vfs_readdir_next` dispatches accordingly.
  - **Cookie scheme for root:** dirnames (up to 44 chars) do not fit the
    8-byte name cookie, so root enumeration uses an **ordinal index** as the
    cookie (entry 0, 1, 2, …). The PDS-dir iterator keeps its name-based
    cookie. The active scheme is chosen from the handle's level.

### 9.3 Dataset-provider abstraction (for future dynamic lists)

To keep decision #4 open, the root iterator does not read the config table
directly. It goes through a tiny provider interface, e.g.:

```
int  export_dataset_count(int export_idx);
const pds_dataset_t *export_dataset_get(int export_idx, int index);
```

The initial implementation returns entries straight from
`export_t.datasets[]`. A future catalog-backed provider can implement the
same two calls (enumerating datasets under an HLQ) without changing the
root iterator, `vfs_readdir_next`, or the NFS layer.

### 9.4 `generate_file_name`

Use the **dataset's** `file_ext` (from `pds_dataset_t`), not a per-export
extension.

## 10. NFS procedure layer (`src/nfs3.c`)

LOOKUP and READDIRPLUS are already relpath-based (`make_child_relpath`,
`fh_cache_insert`, per-entry `vfs_stat`), so they largely work once the
resolver understands three levels:

- **LOOKUP:** root + `dirname` → PDS dir; PDS-dir + file → member.
- **READDIRPLUS/READDIR at root:** emits one entry per PDS dir; the existing
  per-entry `vfs_stat` supplies each PDS directory's DIR attributes.
- `fh_resolve` change (§7) is the main dependency; verify `.`/`..` handling
  at the root level.

## 11. Behaviour change to note

For an **existing single-PDS export, the client-visible layout changes**:
members move from the mount root down one level into `/<dirname>/`. Per
decision #2 this is accepted uniformly (no compatibility/flatten mode).
Anyone with existing automation that expects members at the mount root will
need to add the PDS-directory component to their paths.

## 12. Edge cases

- **Dots vs. slashes:** always split levels on `/` before any extension
  (dot) handling; PDS-dir names contain dots and must be treated whole.
- **Case:** `dirname` is stored lower-case; client lookups are expected
  lower-case. Matching may be done case-insensitively for robustness while
  storing lower-case canonical form.
- **dsname length:** up to 44 chars; `dirname` buffer sized accordingly.
- **Root cookie stability:** ordinal-index cookies are stable for a static
  list. When a dynamic provider is added later, cookie stability across a
  changing dataset set must be revisited (a cookieverf or a stable ordering
  key may be needed).
- **Empty export:** `ndatasets == 0` → root is an empty directory (valid).
- **Duplicate dsname across exports:** fine; distinct `export_id`.

## 13. Impacted files (summary)

| File | Change |
|------|--------|
| `src/nfsd.h`     | `pds_dataset_t`, revised `export_t`, `MAX_PDS_PER_EXPORT`, `MVS_PATH_TYPE_ROOT` |
| `src/exports.c`  | group repeated export paths; per-dataset `dirname`/`file_ext`/`dcbinfo` |
| `src/mvsio.c`    | `mvs_path_type` (3 levels + `dataset_idx`); `mvs_get_pds_dsn_and_member` (dirname→dsname, member-only ext) |
| `src/mvsvfs.c`   | ROOT `vfs_stat`; separate root iterator; `vfs_dir_t` level flag; dataset-provider calls; `generate_file_name` per-dataset ext |
| `src/mvsdol.h`   | `vfs_dir_t` level/kind field |
| `src/fhandle.c`  | `fh_resolve` yields export-relative path instead of dsname-based |
| `src/nfs3.c`     | verify LOOKUP/READDIRPLUS across the 3 levels; root `.`/`..` |
| `nfsd.conf`      | document repeated-path multi-PDS format |

## 14. Suggested implementation order

1. Data model + config loader (`nfsd.h`, `exports.c`) with `ndatasets == 1`
   behaving as today except for the extra directory level.
2. Path resolution (`mvs_path_type`, `mvs_get_pds_dsn_and_member`) and
   `fh_resolve` change — establishes the 3-level namespace.
3. `vfs_stat` ROOT + PDS_DIR keyed on real dsname; identity/fid wiring.
4. Root iterator + dataset-provider abstraction + `vfs_dir_t` level flag.
5. `generate_file_name` per-dataset extension.
6. End-to-end verify on MVS: `ls /` (PDS dirs), `ls /<dir>` (members),
   `cat` a member; multi-PDS and single-PDS exports.
7. Unit tests for the loader grouping and `mvs_path_type` classification.

## 15. Future work (explicitly out of scope)

- Catalog-based **dynamic** dataset discovery under an HLQ (behind the
  §9.3 provider interface).
- Explicit **block** config syntax with per-dataset `fileext=`.
- Cookie-verifier handling if the dataset set can change while mounted.
