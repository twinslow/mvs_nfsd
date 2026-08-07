# MVS NFSD automated integration tests

An end-to-end test harness that drives file operations through a **real OS NFS
client** against a mounted MVS NFSD export and verifies the results
**out-of-band**. It exercises the whole stack — NFS client → TCP → MVS NFSD
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
   allocated (see below) and **exported** by MVS NFSD.

---

## Setup

### 1. Create the test datasets (MVS mode only)

Submit [`jcl/mkdsets.jcl`](jcl/mkdsets.jcl). It allocates five PDSs
(`TEMP.ITEST.FB`, `.VB`, `.FB2`, `.VB2`, `.FBSMALL`) with the right RECFM/LRECL.
`FBSMALL` is deliberately tiny so the "upload to full dataset" test fills it.
Re-run it any time for a clean slate.

Then make sure the MVS NFSD server **exports** those datasets (in `nfsd.conf`),
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
  sudo mount -t nfs -o vers=3,proto=tcp,nolock,soft,timeo=30 192.0.2.10:/exports /mnt/mvsnfsd
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
| `nfs.mount_point` | Where the export is mounted (`/mnt/mvsnfsd` or `Z:\\`). |
| `nfs.export` / `nfs.windows_export` | Only needed for `auto_mount`. |
| `ftp.host` / `ftp.port` | MVS FTP endpoint (mode `mvs`). |
| `ftp.user` / `ftp.password` | Leave `null`. Credentials come from **`MVS_USERID`** / **`MVS_PASSWORD`** (same env vars the expect scripts use); a config value overrides the env, and an interactive run falls back to a prompt. |
| `ftp.use_cwd` / `ftp.quote_dsn` / `ftp.dsn_prefix` | How a dataset is named. MVS FTP servers differ: `true`/`true`/`""` → `CWD 'DSN'` then `RETR MEMBER` (z/OS convention, the default); `true`/`false`/`"/"` → `CWD /DSN` — the **path form `mvs_upload.expect` uses**; `false`/`true`/`""` → `RETR 'DSN(MEMBER)'` with no CWD. If you get **`550 Can't cd into '<dsn>'`**, or reads work but writes fail with **`550 <mem>: Not opened`**, run the probe below. |

**Finding the right FTP dialect.** If the FTP verification fails with a `550`,
let the harness work it out:

```bash
python run_tests.py --config config.json --probe-ftp
```

It tries all four combinations against your first configured dataset and prints
the ones that work, ready to paste into the `ftp` section of `config.json`.
| `datasets.<key>` | `nfs_dir` (the directory under the mount = lower-cased dsname on MVS NFSD), `dsname` (for FTP), `ext`, `recfm`, `lrecl`. |
| `datasets.ro` | Optional. A dataset the **server** exports read-only — add it to the test export in `nfsd.conf` with the `ro` keyword, then give it this key. Used only by test 16, which skips when it is absent. Nothing writes to it, so an existing reference PDS is a fine choice. |
| `options.large_lines` | Line count for "large" members (default 1500 ≈ 108 KB — exceeds the server's in-memory/spill threshold, exercising the spill path). |
| `options.mtime_tolerance_sec` | Slack when checking a set mtime (120 on MVS to absorb ISPF minute/second granularity; a *large* miss usually means a server timezone problem). |
| `options.readdir_members` | Members created by test 14.3 (default 40). At ~196 bytes per entry on the wire this fits in ONE reply for any normal `maxcount`, so 14.3 checks listing integrity rather than paging — use 14.5 for paging. Each extra member costs one small write. *Measured:* the Windows client asks for `maxcount` 8192, giving ~41 entries per page, so 40 sits one entry inside a single reply. Raising it past that would page on **this** client but not on one using a 32 KB budget (~167 entries), which is why paging is 14.5's job and not this test's. |
| `options.readdir_big_dir` | Directory **under the mount** with hundreds of entries, used by 14.5 to exercise multi-page READDIRPLUS for free (e.g. `"sys1.samplib"`). It must be inside the *mounted* export — a PDS exported under a different path is not reachable. Unset ⇒ 14.5 falls back to the largest configured dataset, and skips if none is big enough. |
| `options.readdir_big_min` | Entries a directory needs before 14.5 will use it (default 60). Below this it cannot span a page, so the test skips rather than passing vacuously. |
| `options.pool_members` | Members test 21 writes back-to-back to force pending-pool eviction (default 16 = 2 × the server's `PWW_MAX_PENDING`). Raise it if that constant grows. |

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
| 2.3 | update_stowed_member | Overwrite a member already stowed in the PDS; replacement is shorter. |
| 2.4 | rewrite_pending_member | Rewrite a member the server still holds pending (small + large). |
| 2.5 | append_no_data_loss | Append a large member: either refused with the member unchanged, or accepted with it intact. |
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
| 13.1 | truncate_stowed_no_silent_lie | Shrinking a stowed member must either succeed **and** shorten it, or fail **and** leave it alone — never report success while keeping the old content. Asserts the invariant, not the mechanism: Linux sends `SETATTR(size)` (refused, `NFS3ERR_IO`), Windows re-writes through the WRITE path (succeeds). |
| 13.2 | truncate_to_zero | Truncate to 0; assert the member survives as an empty one, not deleted and not stale. |
| 13.3 | preallocate_then_write | CREATE → `SETATTR(size)` → WRITE, the sequence a Linux client actually uses; assert the member holds the data, not the zero-fill. |
| 13.4 | truncate_pending | Shrink a member still buffered in the write pool — the path that *is* supported. Accepts shortened (slot live) or unchanged (slot already flushed), but never half-applied. |
| 13.5 | truncate_same_size_ok | `SETATTR(size)` to the size the member already has must SUCCEED — clients send it after every write. Guards the exemption that makes 13.1's refusal safe. |
| 14.1 | listing_reflects_changes | A create, a rename and a delete each reach the directory listing. |
| 14.2 | listing_matches_backend | The NFS listing equals the real PDS member list (cross-checked out-of-band). |
| 14.3 | listing_large_directory | A directory needing several READDIR pages: no duplicates, nothing missing, and it terminates. |
| 14.4 | root_listing | The export root lists every configured dataset directory. |
| 14.5 | listing_pages | List a directory large enough to span several READDIRPLUS replies: no duplicates, reproducible, and every name stat-able. Skipped unless such a directory is reachable. |
| 15 | long_line_wrap | Lines longer than the record length must WRAP into whole records, not truncate. |
| 16 | readonly_dataset | A `ro` dataset serves reads and listings but refuses create, overwrite and delete; content unchanged. Skipped unless a `ro` dataset is configured. |
| 17.1 | mkdir_not_supported | MKDIR inside a PDS and at the export root must both fail. |
| 17.2 | symlink_not_supported | SYMLINK must fail. Skipped on Windows (client refuses locally, so the server is never asked). |
| 17.3 | hardlink_not_supported | LINK must fail. |
| 18 | invalid_member_names *(MVS-only)* | Names over 8 chars, starting with a digit, or holding an invalid character are refused — not truncated into a collision. Includes a valid-name control. |
| 19.1 | stat_size_matches_content | `st_size` equals the bytes a read actually returns, small and large (spilled). |
| 19.2 | stat_size_while_pending | Same, for a member still buffered in the write pool — stat and read are both served from the pool and must agree. |
| 20 | concurrent_same_member | Four threads write one member at once (identical content); the member must stay coherent. Does **not** cover the SPFEDIT → `EACCES` path — see the note below. |
| 21 | pool_eviction | Write twice `PWW_MAX_PENDING` members back-to-back so the pool must evict and reuse slots; every member's content must survive. |
| 22.1 | delete_pending_new | Delete a member that was never stowed; it must not be resurrected by the queued flush. |
| 22.2 | delete_pending_rewrite | Delete a member that exists on disk and is mid-rewrite; likewise. |

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
- **MVS-only tests** (`1.3`, `1.5`, `18`) assert behaviour only a real PDS shows
  (a finite dataset; member-name validation), so they skip on a plain server.
- **The SPFEDIT → `EACCES` path is NOT covered, and cannot be from here.**
  `pww_lock()` takes that enqueue once per *slot*, so a second NFS writer for
  the same member finds the existing slot and shares it — no second enqueue and
  no conflict, which is why test 20 asserts coherence rather than a refusal.
  `EACCES` fires only when something *outside* the server holds the member.
  To check it by hand: open a member in ISPF edit, then try to write that
  member over NFS — the write must fail rather than wait or corrupt anything.
- **Tests that skip until configured.** `14.5` needs `options.readdir_big_dir`
  (a large directory under the mount), `16` needs a `datasets.ro` entry backed
  by a `ro` export, and `17.2` skips on Windows because the client refuses
  symlink creation locally, so the server is never asked. Each reports the
  reason rather than passing vacuously.
- **`update_stats` and timezones.** MVS NFSD stores mtime in ISPF stats (local
  time, corrected to UTC). A set-then-read that is off by *hours* points at a
  server TZ misconfiguration; the `mtime_tolerance_sec` slack only absorbs
  seconds/minutes.
- **Auto-mount** is best-effort and privilege-sensitive; pre-mounting and
  `auto_mount: false` is the most reliable path, especially on Windows.
- This harness is **new and not yet run against a live server** here — treat the
  first run as commissioning: expect to tune the export config, extensions, and
  mount options for your environment.
