# Configuration File

The server takes one config file, named on the command line:

```
S NFSD                       (started task -- see jcl/nfsd.jcl)
./nfsd [-p pmap] [-m mount] [-n nfs] nfsd.conf
```

Code: `exports_load()` in `src/exports.c`. Sample: [`nfsd.conf`](../nfsd.conf).

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

Currently recognised commands (see [readme_logging](../README.md#logging)):

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
member name in that PDS.

Limits: up to `MAX_EXPORTS` (16) export paths, each with up to
`MAX_PDS_PER_EXPORT` (32) datasets. Exceeding either logs a warning and
ignores the extra line.

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
