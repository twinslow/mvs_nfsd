# Design: NFS Write / Update Support (PDS member write)

Status: **Phase 1 implemented & verified**.

## 1. Goal

Implement NFSv3 `CREATE` / `WRITE` / `COMMIT` so clients can create and
write PDS members, given the constraint (proven by `jcl/t#pdswr3.jcl`) that
a PDS member **cannot** be written incrementally: `fsetpos`/`fseek` into an
open member abends, so a member must be written **sequentially, in a single
open→write→close pass**, and `fclose` performs the `STOW`.

Therefore the model is **buffer the writes as they arrive, then write the
whole member in one pass** at commit or timeout.

## 2. The impedance mismatch (why buffering is mandatory)

| NFSv3 WRITE                                   | PDS member write                         |
|-----------------------------------------------|------------------------------------------|
| Random access: `(offset, count, data)`        | Sequential records only                  |
| Chunks arrive out of order / in parallel      | One ordered pass                         |
| Chunk size = client `wsize` (e.g. 32K/64K)    | Whole member at once, then `STOW`        |
| Byte stream                                    | Fixed/variable **records** (RECFM/LRECL) |
| ASCII                                          | EBCDIC                                    |
| No `close()` is ever sent to the server        | `fclose` = `STOW` (the commit point)     |
| Durability negotiated via stability + `COMMIT` | Durable only after `STOW`                |

The server must **reassemble** the byte stream in a buffer, then on a single
flush convert it to records (inverse of the read path: split on `\n` into
LRECL records + ASCII→EBCDIC) and write the member in one `fopen("wt")` …
`fwrite` … `fclose` sequence.

## 3. Current state (baseline)

- `proc_write` (`src/nfs3.c`) parses the WRITE, calls `vfs_pwrite()`, and
  **always replies `FILE_SYNC`** with the write verifier.
- `vfs_pwrite` / `vfs_create` (`src/mvsvfs.c`) are stubs returning `EACCES`.
- `proc_commit` resolves the path and returns the verifier; it does no work.
- The write verifier `g_write_verifier` is built at startup from
  `time() + pid` (pid is a placeholder `0xDEADBEEF` on MVS — see §6).
- The main `select()` loop (`src/nfsd.c`) already wakes on a **2-second
  timeout** to poll for the MVS STOP command — the natural hook for a flush
  sweep (§7).
- The server is **single-threaded and select-driven**, so the write buffers
  need no locking.

## 4. Stability flag — the correctness hinge

Replying `FILE_SYNC` means "this data is on stable storage." With
buffer-then-flush that is **false** until the flush happens; if the server
stops before flushing, the client believes the data is safe and will never
resend it → silent data loss.

**`proc_write` must honour the client's requested stability.**  Replying
`UNSTABLE` to a `FILE_SYNC` request is a protocol violation: the client asked
for a durable write and we said "only buffered."  The Windows NFS client
(which issues `FILE_SYNC` writes for `copy`) treats that as the write failing
its durability requirement — it then `COMMIT`s, **deletes the destination,
and fails the copy** (confirmed by packet capture: `WRITE ... FILE_SYNC` →
reply `UNSTABLE` → `REMOVE`).

But we must **not** react by stowing on every write.  A PDS member rewrite
writes new blocks at the *end* of the dataset and abandons the old ones (dead
"gas"); re-stowing per write would fill the PDS and force frequent
compresses.  The member is therefore STOWed **exactly once** — at COMMIT, the
idle sweep, eviction, or shutdown — never per write.

To satisfy the client without stowing per write, we **echo the requested
stability** and make the buffered (not-yet-stowed) member fully visible:

- WRITE → buffer the bytes; reply `committed = <the stability the client
  requested>` + verifier.  An `UNSTABLE` client then issues `COMMIT` (our
  stow trigger, §7); a `FILE_SYNC` client (Windows) is satisfied and does not
  delete the file.
- Because we may reply `FILE_SYNC` before the STOW, the member must be
  visible from the buffer until it is stowed: `vfs_stat` reports the buffer's
  size (§5.1) and `vfs_pread` serves reads from the buffer.  (Directory
  listings and non-NFS tools like ISPF only see the member once it is
  stowed — at COMMIT or, failing that, within the idle-timeout window.)
- COMMIT → STOW the member if still dirty, reply verifier.

This is a write-back model: replying `FILE_SYNC` before the physical STOW is
a durability trade-off (a crash in the pre-stow window loses the data and the
client will not resend).  The idle timeout bounds the window; a client
`COMMIT` closes it immediately.  Acceptable for Phase 1; revisit for stronger
durability later.

## 5. Data model — the pending-member write pool

A new module, tentatively `mvspww.c` / `mvspww.h` (PDS member **w**rite,
mirroring the `mvsprw` read cache), owns a small fixed pool of
**pending-member write buffers**, analogous to the directory open-list
pool (`mvsdol`) and read cache (`mvsprw`):

```c
typedef struct {
    uint8_t   status;              /* FREE / IN_USE */
    int       export_idx;
    int       dataset_idx;         /* resolves RECFM/LRECL + real dsname */
    char      dsname_ebcdic[45];
    char      member_name[9];
    uint8_t  *buf;                 /* the reassembled byte stream        */
    uint32_t  buf_cap;             /* allocated capacity                 */
    uint32_t  high_water;          /* highest offset+count written = size*/
    time_t    last_write_time;     /* for the idle-flush sweep           */
    int       dirty;               /* has unflushed data                 */
    /* (Phase 2) scratch-dataset handle / name for disk spill            */
} pending_member_t;
```

Keyed by (export_idx, dataset_idx, member_name).  Lifecycle:

- **CREATE** allocates a pending slot, records the file handle / relpath,
  and marks the member as existing with size 0.
- **WRITE** finds the slot (or allocates one on first write), grows the
  buffer if needed, copies `count` bytes to `buf[offset]`, updates
  `high_water` and `last_write_time`.
- **COMMIT / timeout / eviction / shutdown** flushes: convert `buf[0 ..
  high_water]` to records and write the member in one pass, then mark
  clean (and free or retain the slot briefly).

### 5.1 Pending members must be visible to `vfs_stat`

A member being written does not exist in the PDS directory until `STOW`.
So `vfs_stat` (and hence LOOKUP / GETATTR) must **check the pending pool
first** (as `vfs_stat_pds_member` already does for `mvsvfs_find_cached_member`):
if the member is pending, report `ftype=REG`, `size=high_water`, and stable
timestamps, so the client can `stat`, write more, and `COMMIT` a member that
isn't on disk yet.

### 5.2 Hole / out-of-order handling

Track `high_water` as the size.  Gaps (offset beyond current high water)
are zero-filled in the buffer.  For text members holes are unusual, but the
buffer model handles them naturally.  Out-of-order chunks just land at their
offset.

## 6. Write verifier — do NOT persist it

**Recommendation: do not save the verifier to disk. It should CHANGE on
every restart, which the current `time()+pid` scheme already does.**

Reasoning: the verifier exists precisely so a client can tell that the
server **restarted and lost uncommitted (buffered) writes** between its
WRITE and COMMIT.  On restart our in-memory pending buffers are gone — that
is exactly the "resend everything" situation.  If we persisted the verifier
so it stayed the same, we would tell clients "nothing was lost" when in fact
the buffers were lost → silent data loss.  A verifier that changes on
restart is the correct, safe behaviour.

The one real weakness is **uniqueness across a fast restart**: MVS has no
`getpid()` and `time()` has 1-second granularity, so two starts within the
same second could produce the **same** verifier and a client would miss the
restart.  Fixes, in order of preference:

1. Mix in the **JES2 job number / ASID / started-task token** (the existing
   `TODO` in `nfsd.c`) — changes every start, no persistence needed.
2. If (1) is not readily available, persist a tiny **monotonic boot counter**
   (a one-record dataset) and increment it at startup, folding it into the
   verifier.  Note this is persistence to *guarantee change*, the opposite
   of keeping the verifier stable.

So: no persistence for stability; optional tiny persisted counter only to
strengthen per-restart uniqueness.

Per §11.1, option 1 (job id) is being prototyped in `jcl/t#jobid.jcl`, with a
callable S/370 ASM routine as a fallback if the JCC-level call doesn't return
a usable id for a started task.  If the job id changes every start, the
persisted boot counter (option 2) is not needed.

## 7. Flush triggers and the select-loop hook

A member's buffer is flushed on any of:

1. **COMMIT** for that member — flush immediately, then reply.  (Also flush
   on a WRITE the client marked `FILE_SYNC`, if we choose to honour that.)
2. **Idle timeout** — no writes for N seconds (proposed 5–10s), tracked
   **per member** and polled once per select iteration (confirmed approach,
   §11.1).  Covers clients that drop the connection without COMMIT.
3. **Pool eviction** — a new pending member needs a slot and the pool is
   full: flush the least-recently-used one (as `mvsdol` evicts).
4. **Server shutdown** — on the STOP command, flush all dirty buffers
   before closing sockets (`src/nfsd.c`, after the loop breaks).

### The select-loop integration

The loop already returns every 2 seconds (STOP poll).  Add a single call
after `select()` returns — on both the timeout (`n == 0`) and the activity
paths — to a sweep function:

```c
pww_flush_idle(now);   /* flush pending members idle > timeout */
```

Because the server is single-threaded, the sweep runs between request
handling with no locking, and the 2-second select granularity bounds flush
latency to a few seconds.  This mirrors how the DOL pool and read cache
already use time-based expiry.  `COMMIT`-driven flushes happen inline in
`proc_commit` and don't wait for the sweep.

The flush itself:

```
fopen("//DSN:dsname(member)", "wt")           -- text mode, member DCB
  convert buf[0..high_water]: split on '\n' into LRECL records,
                              ASCII -> EBCDIC, pad/truncate per RECFM
  fwrite the records
fclose                                          -- performs the STOW
mark slot clean; free buffer (or retain slot for reuse)
```

### 7.1 Directory-change visibility (after a STOW)

The READDIRPLUS-loop fix (a *stable* directory mtime) has a flip side: clients
use a directory's mtime as their readdir cache-invalidation signal, so a
stable mtime means a **newly stowed member never appears** in a cached
listing.  The fix is a directory mtime that is stable between changes but
**bumps when the directory changes**.  On every STOW (`pww_flush_slot`) we
therefore, for that dataset:

- **bump `pds_dataset_t.dir_mtime`** (`export_dataset_touch`) — `vfs_stat` of
  the PDS directory reports this, so the client sees the directory change and
  refreshes its cached listing; and
- **invalidate the server's own readdir cache** for the dataset
  (`dir_openlist_invalidate`) — the DOL pool caches the member list for ~30s,
  so without this the refreshed client would still get the stale list.

Between STOWs the mtime is constant, so an in-progress READDIRPLUS does not
loop.  (mtime has 1-second granularity, so two STOWs to the same directory
within one second can coalesce — acceptable for Phase 1.)

Concurrency note: BSAM output to a PDS wants **one member open for output at
a time**.  Since a flush is a self-contained `fopen…fclose` and the server
is single-threaded, flushing one member at a time is inherent — multiple
buffers may sit in memory but are flushed serially.

## 8. Memory strategy (the 8 MB address-space constraint)

The MVS 3.8J application address space is ~8 MB for code + data, so we cannot
assume a whole file fits in memory — a large NFS file could exceed it.

### Phase 1 — in-memory buffers, capped

Start with in-memory buffers and a hard per-member cap (e.g. a tunable
`PWW_MAX_MEMBER_BYTES`, say 512 KB–1 MB) plus a pool-wide budget.  Most PDS
members (source, JCL, copybooks) are well under this.  A write that would
exceed the cap returns `NFS3ERR_FBIG` (or `NFS3ERR_NOSPC`).  Simple, correct,
and enough to get create/write working end-to-end.

### Phase 2 — spill to a temporary dataset

To lift the size limit, back the buffer with a **temporary sequential (PS)
dataset** instead of (or in addition to) memory:

- Two candidate schemes:
  - **Random-access scratch (now proven — §11.1):** a PS RECFM=F dataset
    opened for update, into which incoming chunks are written at their offset
    by `fseek`.  Confirmed working (`jcl/t#seqrw1.jcl`: random seeks before
    reads *and* writes on a RECFM=F, BLKSIZE=4096 PS dataset).  This is the
    clean way to lift the size limit — the buffer lives on disk, memory use
    per pending member is just a small block, and the offset-addressed writes
    map directly onto `fseek`+`fwrite`.
  - **Streaming contiguous prefix:** the earlier alternative (append the
    contiguous prefix to a scratch PS via QSAM, keep only gaps in memory).
    No longer needed now that random-access PS scratch works, but noted for
    completeness.
- On COMMIT/timeout, read the scratch dataset back and write the real member
  in one pass; then delete the scratch dataset.
- Scratch datasets need dynamic allocation (SVC 99 / `//DSN:&&TEMP`-style)
  and cleanup on flush and on shutdown — the remaining real work for Phase 2.

Phase 2 is kept behind the `mvspww` API so the memory vs. disk backing can
change without touching the VFS/NFS layers.  **Given the 8 MB address-space
limit and that random-access PS scratch is now proven, consider moving to the
disk-backed buffer sooner** — arguably even as the primary backing — rather
than treating it as a distant future phase.  A reasonable middle path: keep
small members in memory (fast) and spill to a scratch PS only once a member
exceeds the in-memory cap.

## 9. Record conversion (inverse of the read path)

The read path turns records into a byte stream (insert `\n` at record
boundaries, EBCDIC→ASCII).  Write does the inverse, driven by the dataset's
`dcbinfo` (RECFM/LRECL, now per-dataset in the multi-PDS model):

- **RECFM=F/FB:** in JCC text mode (`fopen(..., "wt")`) the runtime forms a
  record at each `\n` and space-pads to LRECL, so we can translate the buffer
  to EBCDIC and `fwrite` it directly rather than splitting records by hand.
  Per §11.1, a **line longer than LRECL is not truncated** — the runtime
  wraps the overflow onto an additional logical record.  Fidelity implication:
  a line longer than LRECL round-trips as *two* lines (the read path inserts
  a `\n` at the record boundary).  That is acceptable and matches how MVS
  editors treat over-length data; just be aware of it.
- **RECFM=V/VB:** each line becomes one variable-length record.
- Trailing bytes with no final `\n` become a final record.
- ASCII→EBCDIC on the data before the write (mirror of `ebcdic_to_ascii` on
  read); the EBCDIC newline the translation produces must be the one the
  text-mode runtime treats as the record delimiter (verify during
  implementation — the read path's record→`\n` mapping is the reference).

## 10. Files impacted

| File | Change |
|------|--------|
| `src/mvspww.c` / `.h` (new) | Pending-member write pool: buffer alloc/grow, place-at-offset, idle-flush sweep, flush-one (record conversion + `STOW`), flush-all |
| `src/mvsvfs.c` | `vfs_pwrite` → route into the write pool; `vfs_create` → create pending member; `vfs_stat_pds_member` → check pending pool so in-progress members are visible |
| `src/nfs3.c` | `proc_write` → reply `UNSTABLE`; `proc_commit` → flush the member then reply; `proc_create` unchanged in shape |
| `src/nfsd.c` | Call `pww_flush_idle()` after `select()`; flush-all on STOP before shutdown; strengthen the write verifier (JES job id / boot counter) |
| `src/mvspdir.c` (maybe) | A member-write/`STOW` helper if not already covered by `fopen("wt")…fclose` |
| `nfsd.conf` / README | Note write support + any new tunables (cap, idle timeout) |

## 11. Open questions / prerequisite tests

> **All six were investigated — results and decisions are in §11.1 below,
> and folded into the sections above.**

1. **`fseek` on a PS dataset** — does record positioning work on a plain
   sequential dataset (needed for the Phase 2 random-access scratch)?  Test
   before choosing the Phase 2 scheme.
2. **`STOW` replace semantics** — does `fopen("wt")` on an existing member
   replace it cleanly (directory entry updated, old data released)?  Confirm
   for the update (re-write existing member) case, not just create.
3. **Line-too-long policy** for RECFM=F (truncate vs. error).
4. **Write verifier source** on MVS — is a JES2 job number / ASID reachable
   from the JCC runtime, or do we fall back to the persisted boot counter?
5. **Idle-flush timeout value** and per-member / pool-wide memory caps.
6. **REMOVE / RENAME / SETATTR(size=0 truncate)** — out of scope here but
   related; truncate-on-open (O_TRUNC-style) interacts with the buffer model.

## 11.1 Test Results

1. **fseek on a PS dataset** works. This has been tested using a RECFM=F
   dataset with a blocksize of 4096. The test program issued random seeks
   before reads and writes. Test in `jcl/t#seqrw1.jcl`.
2. **STOW replace semantics** works. Tested with write to new and existing
   member. The test program is `jcl/t#pdswr1.jcl`.
3. **Line-too-long** will result in an additional logical record being 
   added to the PDS member. 
4. **Write verifier source** on MVS. I have some C code that I think will
   get job id. If for some reason that doesn't work for a started task, I
   can create a callable S/370 ASM routine for this purpose. 
   See `jcl/t#jobid.jcl` test program.
5. **Idle-flush timeout value** - Yes, I think this should be per member.
   We can poll the pool for timeouts every select loop to look for
   timed out buffered writes. 
6. **REMOVE / RENAME / SETATTTR(size=0 truncate)** - This can be in a future
   phase. I don't believe it is possible to delete a member from a PDS using
   the JCC library functions. Same is true for a rename. I think the 
   truncate could be implemented.  

## 12. Suggested implementation order

1. `mvspww` pool with **in-memory** buffers (Phase 1), place-at-offset, and
   a flush-one that does the record conversion + `fopen("wt")…fclose`.
2. Wire `vfs_create` / `vfs_pwrite` / `vfs_stat` (pending visibility).
3. `proc_write` → `UNSTABLE`; `proc_commit` → flush.  End-to-end create +
   write + commit of a small member; verify with `ls`, `cat`, and a real
   editor save from the Linux client.
4. Idle-flush sweep in the select loop; flush-all on shutdown.
5. Strengthen the write verifier uniqueness.
6. Phase 2 disk-backed scratch buffer — PS positioning is proven (§11.1), so
   the remaining work is dynamic scratch-dataset allocation and cleanup.
   Given the 8 MB limit, this may be worth pulling forward (see §8).

## 12.1 Phase 1 — agreed scope (plan of record)

We will build **in-memory buffering only** first, to validate the write
semantics and end-to-end flow (CREATE → WRITE → COMMIT, pending-member
visibility, record conversion, STOW) before adding disk-backed buffers.

**In Phase 1:**

- New `mvspww` module: a fixed pool of pending members, each with a
  `malloc`'d byte buffer, place-at-offset, `high_water`, and an idle
  timestamp.
- `vfs_create` → allocate a pending member (size 0).
- `vfs_pwrite` → copy bytes into the buffer at the given offset, growing the
  buffer as needed; update `high_water` and `last_write_time`.
- `vfs_stat` → check the pending pool first so an in-progress (not-yet-stowed)
  member is `stat`-able (`ftype=REG`, `size = high_water`).
- `proc_write` → reply **`UNSTABLE`** (not `FILE_SYNC`).
- `proc_commit` → flush that member (convert → EBCDIC → `fopen("wt")…fclose`
  = STOW), then reply with the verifier.
- **SETATTR must succeed** (it is part of the client's open/write/close
  sequence): `vfs_truncate` routes to the pool (truncate-to-0 starts a fresh
  member; the common `O_TRUNC` case), and `vfs_set_times` is an accepted
  no-op (PDS members carry ISPF times we cannot set, like mode/uid/gid).  A
  SETATTR error aborts the whole copy on the client.
- Select-loop sweep: flush per-member idle timeouts; flush-all on STOP.

**Deferred to Phase 2 (not in Phase 1):**

- PS scratch-dataset spill.  Consequently Phase 1 has a **hard per-member
  cap**: a write that would exceed it returns `NFS3ERR_FBIG`.  This is fine
  for validating the flow with normal-sized members (source, JCL).

**Phase 1 parameters:**

| Parameter | Value | Notes |
|-----------|-------|-------|
| `PWW_MAX_MEMBER_BYTES` | **256 KB** | per-member in-memory cap; over-cap write → `NFS3ERR_FBIG` |
| `PWW_MAX_PENDING`      | **4**      | concurrent pending members in the pool |
| Idle-flush timeout     | 5–10 s (tunable) | per member, polled each select iteration |

**Acceptance check (both clients — we test on both):**

- **Linux** and **Windows 11** NFS mounts:
  - copy a small file into a mounted PDS directory (`cp` / drag-drop);
  - edit-and-save a member from an editor (e.g. `vi`/`nano` on Linux, Notepad
    on Windows) — exercises the write-then-COMMIT path;
  - `cat` / open it back and confirm the contents;
  - `ls -l` / directory listing shows the correct size.
- On MVS, confirm the member is correctly **stowed** with proper records
  (RECFM/LRECL) and readable in ISPF.

## 13. Out of scope (for now)

- Random in-place update of existing members (impossible on PDS; a re-write
  of the whole member is the only option).
- **REMOVE / RENAME** — per §11.1, the JCC library provides no way to delete
  or rename a PDS member, so these are not implementable through JCC alone;
  they would need a callable S/370 ASM routine (STOW with the delete/rename
  option) and are deferred to a later phase.
- **SETATTR size=0 (truncate)** — believed implementable (write an empty
  member), and interacts with the buffer model (truncate-on-open before
  writes); deferred but noted as feasible.
- MKDIR / SETATTR mode/uid/gid.
- Concurrent writers to the same member (last-flush-wins is acceptable
  initially; the pool is keyed per member).
- PDSE-specific behaviour (MVS 3.8J is PDS only).
