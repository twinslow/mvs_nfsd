# Configuration File

The server takes one config file, named on the command line:

```
S NFSD                       (started task -- see jcl/nfsd.jcl)
./nfsd [-p pmap] [-m mount] [-n nfs] nfsd.conf
```

Code: `exports_load()` in `src/exports.c`. Sample: [`nfsd.conf`](../../nfsd.conf).

## 1. Format

The file is sectioned, Windows `.ini` style:

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

General rules:

- A `[Name]` line switches section. Names are **case-insensitive**;
  `[ Init ]` (with blanks inside the brackets) is accepted.
- Lines beginning with `#` are comments. Blank lines are ignored. Both are
  allowed anywhere, including between sections.
- Lines are processed **in file order**, so `[Init]` commands take effect
  before any section written below them.
- Lines appearing **before any section header** are treated as `[Exports]`.
  This keeps older, section-less config files working unchanged.
- An **unrecognised section** is reported and its lines are skipped, so a
  config written for a newer server still loads on an older one.

## 2. `[Init]` — startup commands

Every line in this section is an **operator command**, executed at startup
exactly as if it had been typed at the MVS console via MODIFY:

```
F NFSD,SET LOGLVL DEBUG PROC=WRITE
```

The text after `F NFSD,` is what you put here. The line is handed to the
**same handler** the MODIFY path uses (`log_handle_modify()`), so the two
interfaces cannot drift apart — anything valid at the console is valid here,
and vice versa.

Currently recognised commands (see [readme_logging](../../README.md#logging)):

| Command | Effect |
|---|---|
| `set loglvl <level>` | Global log-stream level |
| `set loglvl <level> proc=<name>` | Pin one NFS procedure's level |
| `set wtolvl <level>` | Operator console (WTO) level |

`<level>` is `DEBUG`, `TRACE`, `INFO`, `WARN`, `ERROR`, or `FATAL`.
`<name>` is an NFS3 procedure (`GETATTR`, `SETATTR`, `LOOKUP`, `ACCESS`,
`READ`, `WRITE`, `CREATE`, `REMOVE`, `RENAME`, `READDIR`, `READDIRPLUS`,
`FSSTAT`, `FSINFO`, `PATHCONF`, `COMMIT`, `NULL`).

Example — quiet overall, but full detail for writes:

```ini
[Init]
set loglvl info
set loglvl debug proc=write
set loglvl debug proc=commit
```

A command that is not recognised is logged as a warning and skipped; a
recognised-but-malformed one reports the specific fault. Neither stops the
server from starting.

**Note:** the built-in startup level is `DEBUG` (set in `main()` before the
config is read), so a config with no `[Init]` section logs verbosely. Adding
`set loglvl info` is the usual first line.

## 3. `[Exports]` — exported PDS datasets

One PDS dataset per line:

```
<nfs-export-path>    <pds-dataset-name>
```

Repeating the **same export path** groups several datasets under one export.
Each dataset appears as a directory named the **lower-case form of the
dataset name** under the export root; its members appear as files inside it.

```ini
[Exports]
/exports    TEMP.TESTPROJ.C
/exports    TEMP.TESTPROJ.CNTL
```

gives clients:

```
/exports/                        (export root -- a virtual directory)
├── temp.testproj.c/             (PDS TEMP.TESTPROJ.C)
│   ├── prog1.c
│   └── prog2.c
└── temp.testproj.cntl/          (PDS TEMP.TESTPROJ.CNTL)
    └── job1.cntl
```

The **file extension** for members is derived automatically from the
dataset's last qualifier, lower-cased — members of `TEMP.TESTPROJ.CNTL` are
seen as `*.cntl`. A file whose extension does not match is not a valid
member name in that PDS. The `fileext=` keyword (§3.1) overrides this default
— e.g. presenting a `.CNTL` or `.JCLLIB` PDS as `*.jcl`.

Limits: up to `MAX_EXPORTS` (16) export paths, each with up to
`MAX_PDS_PER_EXPORT` (32) datasets.

### 3.1 Options (keywords)

Keywords tune read-only status and the permission bits reported to clients.
They are case-insensitive and order-independent.

| Keyword | Level | Meaning | Default |
|---|---|---|---|
| `ro` | export, dataset | Refuse all mutating operations | *(read-write)* |
| `rw` | export, dataset | Explicitly read-write | *(on)* |
| `dirperm=<octal>` | export, dataset | Mode reported for a PDS directory | `777` |
| `memperm=<octal>` | export, dataset | Mode reported for its members | `777` |
| `rootperm=<octal>` | **export only** | Mode reported for the export root | `555` |
| `fileext=<ext>` | export, dataset | File extension shown for members, overriding the dsname-derived one | *(last qualifier, lower-cased)* |
| `nofileext` | export, dataset | Show members with **no** extension at all | *(off)* |

Values for the `perm` keywords are **octal** (`755` = `0755`); a leading zero
is accepted; the range is `0`–`777` (no setuid/setgid/sticky bits).

`fileext=` takes a bare extension without a dot (e.g. `fileext=jcl`, shown as
`member.jcl`); it is stored lower-cased and must be 1–15 characters. It is
useful when the dataset's last qualifier is not the extension you want clients
to see — for example presenting a `.CNTL` PDS as `*.jcl` so Windows opens its
members with a JCL-aware editor.

`nofileext` suppresses the extension entirely, so members appear (and are
created/renamed) by their bare name — e.g. members of a `nofileext`
`SYS1.SAMPLIB` show as `iefbr14`, not `iefbr14.samplib`. It is the counterpart
to `fileext=`, and the two cannot both appear on one line. With no extension, a
filename containing a `.` is not a valid member name and is rejected (member
names can't contain a dot).

`ro` never advertises write: the reported directory/member mode has its write
bits stripped, so `ls -l` and the NFS `ACCESS` check agree with reality. Read-
only is enforced independently of the mode and of the client's user id — even
a client mounting as root cannot write a `ro` export. A refused write returns
`NFS3ERR_ROFS` ("read-only file system").

The **export root is always read-only** (it can't be modified through NFS —
MKDIR/RMDIR are unsupported), so its default mode is `555`. `rootperm=` only
tunes the read/execute bits.

### 3.2 Flat form

Append keywords after the dataset name; they apply to that **dataset**:

```ini
[Exports]
/pub      SYS1.PROCLIB                    ro
/exports  TEMP.TESTPROJ.C                 dirperm=755 memperm=644
/exports  TEMP.TESTPROJ.CNTL   fileext=jcl dirperm=777 memperm=666
/exports  SYS2.JCLLIB          fileext=jcl ro
/exports  SYS1.SAMPLIB         nofileext   ro
```

One mount can mix read-only and read-write datasets this way, and give each
its own file extension (or none).

### 3.3 Block form (per-export)

To set an option once for a whole export — or to set `rootperm`, which is
export-level — use a brace block. Keywords **before** `{` are export-level
and inherited by every dataset inside; keywords after a dataset name refine
them:

```ini
[Exports]
/pub  ro rootperm=555 dirperm=555 memperm=444 {
    SYS1.PROCLIB
    SYS1.MACLIB     memperm=400
}
```

Inheritance:

- `dirperm` / `memperm` and the extension (`fileext` / `nofileext`) are
  **defaults** a dataset may override — a dataset's `fileext=`/`nofileext`
  overrides an export-level one either way.
- `ro` is a **ceiling**: a dataset may narrow to `ro` inside a read-write
  export, but `rw` on a dataset inside an `ro` export is an error.

### 3.4 Errors fail the export (fail-closed)

Unlike an unknown *section* (warned and skipped, §1), a bad **export**
keyword — an unknown keyword, an unparseable/out-of-range value, a keyword at
the wrong level (`rootperm` on a dataset), `rw` inside an `ro` export, or a
malformed `{ }` block — **drops the whole export**, with an error logged. The
reasoning is asymmetric risk: an unknown section cannot make data writable, a
mistyped keyword can (a typo'd `read-only` silently leaving an export
writable is exactly the outcome to avoid). A missing export is safe and
obvious; a partially-applied one is neither.

## 4. Adding a section

The parser is table-driven so new sections are cheap. In `src/exports.c`:

1. Add a `CFG_SECT_*` id.
2. Add a `{ "NAME", CFG_SECT_* }` entry to `g_cfg_sections[]`.
3. Add a `case` to the dispatch `switch` in `exports_load()`, calling a
   `cfg_do_<name>_line()` handler.

## 5. MVS note: brackets and EBCDIC

The config file is read as raw bytes, so on MVS its text is **EBCDIC** — and
so are C character literals under JCC. That is why the `#` comment test
works, and the same applies to `[` and `]`.

Be aware that `[` and `]` are the **most code-page-variable characters in
EBCDIC** (they differ between CP037, CP1047, and CP500). This server assumes
CP037 throughout (see `src/ebcdic.h`). If a section header is ever not
recognised on MVS, that is the first thing to check: the delimiters are
centralised as `CFG_SECT_OPEN` / `CFG_SECT_CLOSE` in `exports.c`, and the
fix would be to compare against `ascii_to_ebcdic_c('[')` instead of the
literal.
