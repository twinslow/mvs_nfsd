# MVS FTP Expect Scripts

Two Expect scripts manage the transfer of source files between the local
development machine and MVS PDS libraries over FTP.

| Script | Direction | Description |
|---|---|---|
| `mvs_upload.expect` | Local → MVS | Upload source files to MVS PDS datasets |
| `mvs_download.expect` | MVS → Local | Download PDS members back to local files |

Both scripts connect to the MVS FTP server at `localhost:2121` using passive
mode and set ASCII transfer mode before any file transfer.

---

## Prerequisites

- **Expect** must be installed (`expect --version` to confirm).
- The MVS FTP server must be reachable at the configured host and port.
- The target PDS datasets must be pre-allocated on MVS before uploading.
- Make the scripts executable once after checkout:
  ```bash
  chmod +x mvs_upload.expect mvs_download.expect
  ```

---

## Credentials

Both scripts prompt interactively for TSO userid and password.  The password
prompt suppresses echo.

### Environment variable shortcut (upload only)

`mvs_upload.expect` will use the following environment variables if they are
set, skipping the corresponding prompt:

| Variable | Used for |
|---|---|
| `MVS_USERID` | TSO userid |
| `MVS_PASSWORD` | TSO password |

Either variable can be set independently — if only one is set the script
prompts for the other.

```bash
export MVS_USERID=TONYW
export MVS_PASSWORD=secret
./mvs_upload.expect
```

> **Security note:** avoid storing credentials in shell profiles or scripts
> that are committed to version control.  Use environment variables only in
> interactive sessions or secure CI environments.

---

## mvs_upload.expect

Uploads local source files to MVS PDS libraries.

### Usage

```
./mvs_upload.expect [incremental]
```

Run from the project root directory.

| Argument | Description |
|---|---|
| *(none)* | Full upload — every file matching the job list is uploaded |
| `incremental` | Only upload files whose modification time is newer than the previous run |

> **Note:** a leading `-` is intentionally omitted from `incremental`.
> Expect intercepts single-dash flags before the script sees them; `-incremental`
> would be parsed as Expect's own `-i` flag, corrupting the argument.

### How it works

1. Parses the command-line argument and loads the last-run timestamp if
   running in incremental mode.
2. Validates that every source directory in the job list exists and counts
   the files to be transferred.
3. Prints a banner showing mode, last-run time (incremental only), and
   per-job file counts.
4. Prompts for credentials (or reads from environment variables).
5. Connects to the FTP server, logs in, and sets ASCII mode.  The raw FTP
   dialogue is **not** shown on screen — it is written to
   `.mvs_upload_ftp.log` (see [FTP dialogue log](#ftp-dialogue-log)).
6. For each upload job: CDs to the target PDS, then uploads each file.
7. Saves the run timestamp to `.mvs_upload_lastrun` for future incremental
   runs.
8. Prints a summary and exits 0 on success, 1 if any transfer failed.

### FTP dialogue log

Only the script's own progress and error lines appear on screen; the noisy
FTP client/server conversation is suppressed (`log_user 0`) and captured to a
log file instead:

- Upload writes `.mvs_upload_ftp.log`; download writes `.mvs_download_ftp.log`.
- The log is overwritten on each run and prefixed with a timestamped header.
- When a transfer fails, the summary points at the log so the full dialogue is
  available for diagnosis.
- The password is entered through the FTP client (which suppresses its echo),
  so it is not written to the log.
- Both logs are listed in `.gitignore` — they are machine-local and should not
  be committed.

### Incremental mode

When `incremental` is given the script compares each file's modification
time against the Unix epoch timestamp stored in `.mvs_upload_lastrun`.
Only files modified *after* that timestamp are sent.

- The timestamp is recorded at the **start** of the run (not the end), so
  any file modified *during* a run is caught by the next incremental run.
- On the very first run there is no timestamp file; all files are uploaded
  and the file is created.
- If the timestamp file is unreadable or corrupt a warning is printed and
  all files are uploaded.
- `.mvs_upload_lastrun` is listed in `.gitignore` — it is machine-local
  state and should not be committed.

### Member name derivation

MVS PDS member names are limited to 8 characters drawn from `A-Z 0-9 @ # $`.
The script derives a member name from the local filename as follows:

1. Strip the directory path and file extension.
2. Convert to uppercase.
3. Remove any characters not in the valid set.
4. Truncate to 8 characters (a warning is printed if truncation occurs).

| Local filename | PDS member |
|---|---|
| `nfsd.c` | `NFSD` |
| `fhandle.c` | `FHANDLE` |
| `types.h` | `TYPES` |
| `ispf_to_unix_epoch.c` | `ISPFTOUN` *(truncated — warned)* |

### Configuration

All configuration is at the top of the script.

```tcl
set ftp_host  "localhost"
set ftp_port  "2121"
```

The upload jobs list controls what is uploaded and where:

```tcl
set upload_jobs [list \
    [list $src_dir      "*.c"  "/TONYW.NFSD.C"]          \
    [list $src_dir      "*.h"  "/TONYW.NFSD.H"]          \
    [list $jcl_dir      "*.jcl" "/TONYW.NFSD.CNTL"]      \
    [list $tests_dir    "*.c"  "/TONYW.NFSD.TESTS.C"]    \
    [list $tests_dir    "*.h"  "/TONYW.NFSD.TESTS.H"]    \
    [list $tests_jcl_dir "*.jcl" "/TONYW.NFSD.TESTS.CNTL"] \
]
```

Each entry is a three-element list: `{local-directory  glob-pattern  target-PDS}`.

**Adding a new PDS:**  append one line to `upload_jobs` — no other changes
are needed:

```tcl
[list [file join $script_dir newdir] "*.ext" "/TONYW.NFSD.NEWPDS"]
```

### Example output

```
MVS source upload
=================
FTP target : localhost:2121
FTP log    : /home/tony/dino_nfs/.mvs_upload_ftp.log
Mode       : incremental
Last run   : 2026-05-20 14:32:07
Jobs       :
  *.c      /home/tony/dino_nfs/src         -> /TONYW.NFSD.C (3 of 12 file(s) changed)
  *.h      /home/tony/dino_nfs/src         -> /TONYW.NFSD.H (0 of 6 file(s) changed)
  ...
Total      : 3 of 21 file(s) changed

TSO userid : TONYW (from $MVS_USERID)
Password   : (from $MVS_PASSWORD)

Connecting to localhost port 2121 ...
Logged in as TONYW.

--- *.c -> /TONYW.NFSD.C (3 of 12 file(s)) ---
  mvsio.c                        -> MVSIO
  mvsvfs.c                       -> MVSVFS
  nfsd.c                         -> NFSD

=================
Upload complete.
  Succeeded : 3 file(s)
```

---

## mvs_download.expect

Downloads members from MVS PDS libraries to local files.

### Usage

```
./mvs_download.expect [destination-directory]
```

Run from the project root directory.

| Argument | Description |
|---|---|
| *(none)* | Download into `./src` beside the script |
| `destination-directory` | Download into the specified directory (created if it does not exist) |

Specifying an alternative directory is useful for testing without
overwriting the working source tree:

```bash
./mvs_download.expect /tmp/mvs_download_test
```

### How it works

1. Determines the destination directory (argument or default `./src`).
2. Creates the destination directory if it does not exist.
3. Prints a banner showing the FTP target, PDS names, and destination.
4. Prompts for TSO userid and password.
5. Connects to the FTP server, logs in, and sets ASCII mode.  The raw FTP
   dialogue is **not** shown on screen — it is written to
   `.mvs_download_ftp.log` (see [FTP dialogue log](#ftp-dialogue-log)).
6. For each configured PDS: CDs to it, runs `ls` to get the member list,
   then downloads each member to a local file.
7. Prints a summary and exits 0 on success, 1 if any download failed.

### Member name to filename mapping

The MVS member name (always uppercase) is lowercased and the appropriate
extension is appended.

| PDS | Member | Local file |
|---|---|---|
| `TONYW.NFSD.C` | `NFSD` | `nfsd.c` |
| `TONYW.NFSD.H` | `TYPES` | `types.h` |
| `TONYW.NFSD.CNTL` | `COMPILE` | `compile.jcl` |

### Configuration

```tcl
set ftp_host  "localhost"
set ftp_port  "2121"
set pds_c     "/TONYW.NFSD.C"
set pds_h     "/TONYW.NFSD.H"
set pds_cntl  "/TONYW.NFSD.CNTL"
```

Adjust the PDS names to match your MVS user prefix and dataset allocation.

### Member list detection

The script uses `ls` in the FTP session and identifies member names using
the regex `[A-Z@#$][A-Z0-9@#$]{0,7}` anchored to a line ending.  This
reliably distinguishes member names from FTP server status messages, which
either begin with a digit (response codes) or contain lowercase letters.

### Example output

```
MVS source download
===================
FTP target       : localhost:2121
FTP log          : /home/tony/dino_nfs/.mvs_download_ftp.log
C   PDS          : /TONYW.NFSD.C
H   PDS          : /TONYW.NFSD.H
JCL PDS          : /TONYW.NFSD.CNTL
Destination      : /home/tony/dino_nfs/src

TSO userid : TONYW
Password   :

Connecting to localhost port 2121 ...
Logged in as TONYW.

--- Getting member list from /TONYW.NFSD.C ---
  Found 12 member(s): EBCDIC, EXPORTS, FHANDLE, ...

--- Downloading .c files ---
  EBCDIC   -> ebcdic.c
  EXPORTS  -> exports.c
  ...

===================
Download complete.
  Succeeded : 21 file(s)
  Location  : /home/tony/dino_nfs/src
```

---

## FTP reliability notes

Both scripts apply the same defensive FTP interaction pattern, which was
developed to handle the way the MVS FTP server and the Unix `ftp` client
interact:

- Every FTP command drains to the `ftp>` prompt before the next command is
  sent, preventing stale responses in the expect buffer from being
  misinterpreted.
- `226 Transfer` is used as the transfer-success signal rather than `226 `
  alone, because the transfer-statistics line emitted by the FTP client can
  contain three-digit sequences that would otherwise trigger a false match.
- `exp_continue` is used after seeing `226 Transfer` so the script keeps
  reading until the `ftp>` prompt confirms the client has fully processed
  the command.
