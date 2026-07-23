# dino-nfs automated integration tests

An end-to-end test harness that drives file operations through a **real OS NFS
client** against a mounted dino-nfs export and verifies the results
**out-of-band**. It exercises the whole stack — NFS client → TCP → dino-nfs
server → PDS — the way a user actually does, which unit tests cannot.

- **Stdlib-only Python 3** (3.6+). No `pip install`. Runs on **Linux and Windows**.
- **Two verification backends**, selected by config `mode`:
  - `mvs` — verify by fetching the member over **MVS FTP** (fully independent of
    the NFS path under test); FTP is also used for "server preparation" (uploads).
  - `plain` — verify by reading the member back through the mount. Lets you run
    and **pass** the whole harness against a standard NFS server (e.g. a Windows
    client mounting a Linux NFS export) with no MVS involved.
- Self-contained under `integration-test/` — nothing is placed in the main
  application folders.

---

## Layout

```
integration-test/
  run_tests.py                 CLI entry point
  config.example.json          MVS-mode config template
  config.plain.example.json    plain-mode config template
  jcl/mkdsets.jcl              allocates the FB/VB test PDSs on MVS
  itest/
    config.py                  load + validate the JSON config
    nfsmount.py                OS mount/unmount helpers (Linux + Windows)
    backend.py                 verification backends (MVS FTP / plain readback)
    context.py                 the API every test uses (paths, data, verify)
    textutil.py                data generation + tolerant text comparison
    runner.py                  test registry + runner
    cases.py                   the test cases (numbered per the outline below)
```

---

## Prerequisites

1. **Python 3.6+** on the machine that runs the tests.
2. **An OS NFS client**:
   - Linux: `nfs-common` (Debian/Ubuntu) or `nfs-utils` (RHEL).
   - Windows: *Services for NFS* → "Client for NFS" (provides `mount`/`umount`).
3. For `mvs` mode: network access to the MVS **FTP** server, and the test PDSs
   allocated (see below) and **exported** by dino-nfs.

---

## Setup

### 1. Create the test datasets (MVS mode only)

Submit [`jcl/mkdsets.jcl`](jcl/mkdsets.jcl). It allocates five PDSs
(`TEMP.ITEST.FB`, `.VB`, `.FB2`, `.VB2`, `.FBSMALL`) with the right RECFM/LRECL.
`FBSMALL` is deliberately tiny so the "upload to full dataset" test fills it.
Re-run it any time for a clean slate.

Then make sure the dino-nfs server **exports** those datasets (in `nfsd.conf`),
ideally with a matching extension so members appear as `<name>.txt`, e.g.:

```
/exports TEMP.ITEST.FB    fileext=txt
/exports TEMP.ITEST.VB    fileext=txt
...
```

The config `datasets.<key>.ext` must equal the extension the server presents.

### 2. Mount the export

The harness only ever touches `nfs.mount_point`; how it gets mounted is up to
you. Easiest is to mount it yourself and leave `auto_mount: false`:

- **Linux**
  ```bash
  sudo mount -t nfs -o vers=3,proto=tcp,nolock,soft,timeo=30 192.0.2.10:/exports /mnt/dinonfs
  ```
- **Windows** (elevated prompt, Services for NFS installed)
  ```
  mount -o "nolock,nfsvers=3,tcp,soft,timeo=10" 192.168.1.168:/iexports x:
  ```

Set `auto_mount: true` to have the harness run the platform mount command for
you before the run and unmount after (needs privileges; on Linux set
`use_sudo: true` if appropriate).

### 3. Write the config

Copy a template to `config.json` and edit it:

```bash
cp config.example.json config.json          # MVS
# or
cp config.plain.example.json config.json     # plain NFS server
```

Key fields:

| Field | Meaning |
|---|---|
| `mode` | `mvs` (verify over FTP) or `plain` (verify by readback). |
| `nfs.mount_point` | Where the export is mounted (`/mnt/dinonfs` or `Z:\\`). |
| `nfs.export` / `nfs.windows_export` | Only needed for `auto_mount`. |
| `ftp.host` / `ftp.port` | MVS FTP endpoint (mode `mvs`). |
| `ftp.user` / `ftp.password` | Leave `null`. Credentials come from **`MVS_USERID`** / **`MVS_PASSWORD`** (same env vars the expect scripts use); a config value overrides the env, and an interactive run falls back to a prompt. |
| `datasets.<key>` | `nfs_dir` (the directory under the mount = lower-cased dsname on dino-nfs), `dsname` (for FTP), `ext`, `recfm`, `lrecl`. |
| `options.large_lines` | Line count for "large" members (default 1500 ≈ 108 KB — exceeds the server's in-memory/spill threshold, exercising the spill path). |
| `options.mtime_tolerance_sec` | Slack when checking a set mtime (120 on MVS to absorb ISPF minute/second granularity; a *large* miss usually means a server timezone problem). |

The harness understands these dataset **keys**: `fb`, `vb` (primary FB/VB text
datasets), `fb2`, `vb2` (rename/copy targets), and `small` (the tiny full-dataset
target). Tests that need a key which isn't configured **skip** themselves.

---

## Running

```bash
python run_tests.py --config config.json           # run everything
python run_tests.py --config config.json --list     # list tests, don't run
python run_tests.py -c config.json -s 1 -s 2         # only sections 1 and 2
python run_tests.py -c config.json -f zip -f rename  # only matching names
```

Each test prints `PASS` / `FAIL` / `SKIP`; the run ends with a summary and full
tracebacks for any failures. Exit code is non-zero iff a test failed (skips do
not fail the run). Members created during a test are cleaned up afterward, so
reruns are idempotent.

---

## Test outline

Numbers match `--section`. "MVS-only" tests skip automatically in `plain` mode.

| # | Test | What it does |
|---|---|---|
| 1.1 | upload_small | Write a small member to FB and VB; verify content. |
| 1.2 | upload_large | Write a large member (> spill threshold) to FB and VB; verify. |
| 1.3 | upload_full_dataset *(MVS-only)* | Fill `small` until a write fails; assert it does. |
| 1.4 | upload_concurrent | Five threads write distinct members at once; verify all (catches cross-talk). |
| 1.5 | upload_invalid_name *(MVS-only)* | Create a member with a non-matching extension; assert it's rejected. |
| 2.1 | download_small_large | Prepare members out-of-band (FTP upload), read via NFS, compare. |
| 2.2 | upload_then_download | Write via NFS, immediately read back (small + large). |
| 3 | touch_create | `touch` a new path; assert an (empty) member appears. |
| 4 | update_stats | Set mtime on an existing member; assert mtime moved and content is unchanged. |
| 5.1 | delete_exists | Delete a member; assert it's gone. |
| 5.2 | delete_missing | Delete a non-existent member; assert it fails. |
| 6.1 | rename_within_pds | Rename a member inside one PDS; verify. |
| 6.2 | rename_cross_pds | Move a member to another PDS (`mv` semantics: rename, else copy+delete). |
| 7 | copy_local_to_pds | `cp` a local file onto a member; verify. |
| 8 | copy_pds_to_pds | Copy a member from one dataset to another; verify. |
| 9 | unzip_to_dataset | Extract a zip's entries as members of one dataset; verify each. |
| 10 | unzip_to_multiple | Extract a zip whose entry paths route members into several datasets. |
| 11 | zip_from_dataset | Read a dataset's members via NFS into a zip; verify the archive. |
| 12 | zip_from_multiple | Same, spanning several datasets. |

---

## How comparison works

FB members are stored fixed-width, space-padded; the NFS read path and an FTP
text retrieval both strip trailing blanks and normalise the line terminator. So
the harness compares **line by line after stripping trailing whitespace and
ignoring trailing blank lines** (`textutil.normalize`), not byte-for-byte.
Generated data never contains meaningful trailing spaces, and is always written
with `\n` line endings (even on Windows) so the server sees clean record breaks.

---

## Notes and limitations

- **Verification independence.** In `mvs` mode, FTP reads the PDS independently
  of the NFS path under test — a strong check. In `plain` mode, verification
  reads back through the same mount; that proves the operations and the harness
  work, which is the point of the plain mode ("testable and can pass on a
  standard server"), but it is not an independent oracle.
- **MVS-only tests** (`1.3`, `1.5`) assert behaviour only a real PDS shows (a
  finite dataset; member-name validation), so they skip on a plain server.
- **`update_stats` and timezones.** dino-nfs stores mtime in ISPF stats (local
  time, corrected to UTC). A set-then-read that is off by *hours* points at a
  server TZ misconfiguration; the `mtime_tolerance_sec` slack only absorbs
  seconds/minutes.
- **Auto-mount** is best-effort and privilege-sensitive; pre-mounting and
  `auto_mount: false` is the most reliable path, especially on Windows.
- This harness is **new and not yet run against a live server** here — treat the
  first run as commissioning: expect to tune the export config, extensions, and
  mount options for your environment.
