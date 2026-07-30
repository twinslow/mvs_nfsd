# Design: NFS Write / Update Support (PDS member write)

Status: **Implemented & verified on MVS 3.8J** (Linux + Windows 11 clients).

Shipped: CREATE / WRITE / COMMIT with buffer-then-STOW (§5, §7); stability
echoed to the client (§4); pending members visible to `stat`/`read` before the
STOW (§5.1); ISPF statistics set on write and refreshed on `touch`/SETATTR
(§9.1); SETATTR size (truncate) and time handling; and a restart-unique write
verifier seeded from the hashed JES2 job id (§6). The member is written through
a **`DISP=SHR` dynamic allocation** (not JCC's `DISP=OLD` `//DSN:` open) and
serialised against ISPF/EDIT with the **`SPFEDIT` enqueue**, both taken at
CREATE / first WRITE and held for the slot's lifetime (§7.2) — so a member open
in an editor fails the write cleanly rather than corrupting or blocking. Phase 1
keeps write buffers **in memory** with a per-member cap; disk-backed spill to a
temporary dataset (§8, Phase 2) is **designed but not yet built**. The sections
below record the design and rationale; §3 is the original pre-implementation
baseline and is retained for context only.

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

## 3. Starting baseline (before this work — historical)

> This section describes the pre-implementation state and is kept for context;
> every item below has since been built out. See the status note above.

- `proc_write` (`src/nfs3.c`) parsed the WRITE, called `vfs_pwrite()`, and
  **always replied `FILE_SYNC`** with the write verifier. *(Now echoes the
  client's requested stability — §4.)*
- `vfs_pwrite` / `vfs_create` (`src/mvsvfs.c`) were stubs returning `EACCES`.
  *(Now route into the pending-member write pool — §5.)*
- `proc_commit` resolved the path and returned the verifier; it did no work.
  *(Now flushes the member — STOW.)*
- The write verifier `g_write_verifier` was built at startup from
  `time() + pid`, with `pid` a placeholder on MVS. *(Now `pid` is the hashed
  JES2 job id — §6, §11.1.)*
- The main `select()` loop (`src/nfsd.c`) already woke on a **2-second
  timeout** to poll for the MVS STOP command — the natural hook for the flush
  sweep (§7), which is now wired in.
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

**Locked member → the write fails (not the commit).**  Because the member is
allocated and its `SPFEDIT` enqueue taken at CREATE / first WRITE (§7.2), a
member currently open in ISPF/EDIT (or REVIEW) fails that first operation with
`NFS3ERR_ACCES` — the client reports "access denied".  This is deliberately at
WRITE, not COMMIT: **every** client issues WRITE, whereas the Linux NFS client
never issues COMMIT, so failing at commit time would let a locked-member write
silently succeed-then-lose on Linux.  Once the editor releases the member, a
retried write proceeds normally.

## 5. Data model — the pending-member write pool

A new module, tentatively `mvspww.c` / `mvspww.h` (PDS member **w**rite,
mirroring the `mvsprw` read cache), owns a small fixed pool of
**pending-member write buffers**, analogous to the directory open-list
pool (`mvsdol`) and read cache (`mvsprw`):

```c
/* Spill state -- owned by mvsspl.c (§8.6). */
typedef struct {
    FILE     *fp;                  /* open scratch dataset, or NULL       */
    uint32_t  size;                /* bytes on disk (== high_water spilled)*/
    int       slot;                /* slot index -> &&PWWSP<nn>, to reopen */
    uint8_t   dirty;               /* writes pending since last commit     */
} pww_spill_t;

typedef struct {
    uint8_t   status;              /* FREE / USED                        */
    int       export_idx;
    int       dataset_idx;         /* resolves RECFM/LRECL + real dsname */
    char      dsname_ebcdic[45];
    char      member_name[9];
    uint8_t  *buf;                 /* the reassembled byte stream        */
    uint32_t  buf_cap;             /* allocated capacity                 */
    uint32_t  high_water;          /* highest offset+count written = size*/
    time_t    last_write_time;     /* for the idle-flush sweep           */
    uint8_t   dirty;               /* has unflushed data                 */

    /* Serialisation / allocation state (§7.2), acquired at CREATE / first
       WRITE and held until the slot is released.  Each flag reflects a
       resource currently held and drives exactly what to clean up on
       release or error, so the flags never lie. */
    uint8_t   enq_held;            /* 1 = SPFEDIT enqueue is held         */
    uint8_t   allocated;           /* 1 = the DSN(member) is allocated    */
    char      ddname[9];           /* ddname of that allocation           */

    /* Phase 2 spill (§8.1): once the byte stream passes PWW_SPILL_THRESHOLD it
       lives in a temporary dataset and buf is freed.  spill.fp != NULL is the
       "this member is spilled" flag.  Grouped into its own sub-struct so the
       ownership boundary is visible: mvsspl.c is the ONLY module that assigns
       to these fields; mvspww.c reads spill.fp to choose a path and otherwise
       goes through the spill_* API (§8.6). */
    pww_spill_t spill;
} pending_member_t;
```

Keyed by (export_idx, dataset_idx, member_name).  Lifecycle:

- **CREATE** allocates a pending slot and, on MVS, **takes the `SPFEDIT`
  enqueue and dynamically allocates `DSN(member)` `DISP=SHR`** (§7.2), holding
  both for the slot's lifetime; marks the member as existing with size 0.  If
  the enqueue or allocation fails, the CREATE fails and the slot is released.
- **WRITE** finds the slot; on the **first** write it initialises the slot and
  takes the enqueue + allocation as for CREATE.  Then it grows the buffer if
  needed, copies `count` bytes to `buf[offset]`, and updates `high_water` and
  `last_write_time`.  Follow-on writes just buffer — the enqueue/allocation are
  already held.
- **COMMIT / timeout / eviction / shutdown** flushes: convert `buf[0 ..
  high_water]` to records and write the member in one pass **through the held
  allocation's ddname**, then mark clean.  COMMIT keeps the slot (more writes
  may follow); the idle sweep / eviction / shutdown **release** the slot, which
  drops the enqueue and allocation (§7.2).

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

> **Implemented:** the verifier is `time()` (high 4 bytes) + a 32-bit hash of
> the **JES2 job id** (low 4 bytes), built once at startup in `src/nfsd.c`; the
> job id comes from `get_jes2_jobid()` in `src/mvsutl.c`. It is not persisted —
> the job id changes on every start, so option 1 below was taken and the boot
> counter (option 2) was not needed.

**Recommendation: do not save the verifier to disk. It should CHANGE on
every restart, which the `time()+pid` scheme already does.**

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

1. **COMMIT** for that member — flush immediately, then reply.  (A WRITE is
   never flushed per write, whatever its stability — see §4; we echo the
   stability instead and STOW once, at one of the triggers below.)
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

The flush itself (`pww_flush_slot`) — note it neither enqueues nor allocates;
both are already held from first write (§7.2), so it opens the held allocation
by its ddname:

```
fopen("//DDN:ddname", "wt")                   -- held DISP=SHR alloc, text mode
  convert buf[0..high_water]: split on '\n' into LRECL records,
                              ASCII -> EBCDIC, pad/truncate per RECFM
  fwrite the records
fclose                                          -- performs the STOW
set ISPF stats (§9.1); bump dir mtime + invalidate readdir cache (§7.1)
mark slot clean; buffer + allocation + enqueue retained until slot release
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

### 7.2 Serialisation and allocation — the SPFEDIT enqueue, held from first write

Two problems the naive `fopen("//DSN:dsname(member)", "wt")` flush did not
solve, both about **sharing the PDS with TSO/ISPF while it is up**:

1. **Whole-dataset lock.**  JCC's `//DSN:` open allocates the dataset
   `DISP=OLD`, which takes an *exclusive* dataset-level `SYSDSN` enqueue — it
   fails (or waits) if anyone else, including ISPF, has the PDS allocated.  We
   instead **dynamically allocate `DISP=SHR`** (SVC 99, `mvs_dynalloc` in
   `src/mvsdalc.asm`) and open the returned ddname with `fopen("//DDN:ddname",
   "wt")`, so the server shares the PDS the way TSO users do.
2. **Member-level races with an editor.**  `DISP=SHR` shares the dataset but
   does nothing to stop us rewriting a member somebody is editing — their
   later save would silently overwrite ours (or vice-versa).  ISPF/EDIT (and
   REVIEW) serialise a member with an **exclusive enqueue** on
   `QNAME=SPFEDIT`, `RNAME = dsname(44) + member(8)` (blank-padded), scope
   `SYSTEMS`.  We take the **same** enqueue (`mvs_enq` in `src/mvsenq.asm`,
   `RET=USE` so it *fails fast* instead of waiting — never hanging the
   single-threaded server) before touching the member.  A conflict means an
   editor holds it, and we fail the operation.

**Acquire at first write, hold for the slot's lifetime.**  Both the enqueue
and the allocation are taken together at CREATE / first WRITE by `pww_lock`
(enqueue first, then allocate; on allocation failure the enqueue is backed
out), and released together by `pww_unlock` (unallocate, then DEQ).  They are
held across all the buffered writes *and* the flush *and* the ISPF-stats STOW,
and are dropped only when the slot is released.  The two `pending_member_t`
flags (`enq_held`, `allocated`) record exactly what is held; `pww_unlock` is
flag-driven, so it is correct after a partial acquire and safe to call
unconditionally.  `pww_slot_release` calls it, so **every** release path — idle
sweep, pool eviction, shutdown, and REMOVE's discard — frees the enqueue and
allocation.

Doing this at first write (rather than in the flush, where change history first
put it) is what makes a locked member fail at WRITE — the operation every
client issues — instead of at COMMIT, which the Linux client never sends (§4).

**Getting the ddname right.**  `mvs_dynalloc` returns the system-assigned
ddname via a `DALRTDDN` text unit *on the allocation request itself*, so it is
guaranteed to be the ddname of the allocation we just made (dsname **+**
member).  An earlier design fetched it with a separate info retrieval keyed on
**dataset name alone**, which — once the allocation was held long enough to
overlap the directory-read allocation of the same PDS — could return a
different, *memberless* allocation's ddname; `fopen("//DDN:...")` then opened
the PDS *directory* and failed with `EISDIR`.  Retrieving it from the ALLOC
removes that ambiguity (and saves an SVC 99 call).

**Stats-only updates serialise too.**  `pww_touch_stats` (from
`vfs_set_times`, §12.1) rewrites the directory user data, so it holds the same
`SPFEDIT` enqueue while it does: if a pending slot already holds it, that is
reused; otherwise it takes its own and drops it after.  If an editor holds the
member, the stats touch is **skipped** (the editor's own save will set the
changed date) rather than failing the SETATTR.

### 7.3 Abend protection during the flush — the STAE exit

**The problem.**  The flush is the one place NFSD writes to a PDS, and PDS
writes abend.  The common case is **running out of space**: `D37` (primary
extent full, no secondary), `B37` (secondary extents / volume exhausted),
`E37` (no more extents or volumes).  Today an abend in the flush path
terminates the NFSD task outright — a single user filling a PDS takes the
whole server down for everyone, with pending members in other slots lost.  That
is unacceptable for a long-running service; an out-of-space condition must
degrade to an error on *one* request.

**The mechanism.**  JCC exposes MVS recovery through `_setjmp_stae()`, which
establishes a STAE and behaves like `setjmp`:

| Return | Meaning |
|---|---|
| `0` | STAE established; fall through and run the protected work. |
| `1` | An abend was intercepted and control resumed here; the SDWA copy holds the diagnostics. |
| other | The STAE could **not** be established. |

`_setjmp_canc()` cancels the most recently established STAE and **must** be
called on the normal path once the protected work completes — establish/cancel
are LIFO and must balance.  The caller supplies a 104-byte buffer that receives
a copy of the System Diagnostic Work Area (mapped by `IHASDWA`); the system
completion code is:

```c
#define ABEND_SYSCODE(sdwa)   (((sdwa)[1] & 0x00FFF000u) >> 12)   /* e.g. 0xD37 */
```

The shape is proven by the prototype in `jcl/t#stae1.jcl` (`protected_write`
traps an abend raised inside `write_lines`).  Needs `<mvsutils.h>` and
`<setjmp.h>`.

**Where the boundary goes — two regions, not one.**  Both PDS-mutating steps of
a flush need protection, but they fail with *different meanings*, so each gets
its own STAE rather than sharing one:

| Region | Covers | An abend here means |
|---|---|---|
| **A — member write** | `fopen` → translate/`fwrite` loop → `fclose` (the runtime's STOW) | the member content is lost or partial — a **real** flush failure |
| **B — ISPF stats** | `mvs_stow` STOW REPLACE adding the 30-byte user data (§9.1) | the member is **intact and stowed**; only its statistics are missing |

Protection is per region, never per record: the establish is an SVC call,
negligible against a member write.  `pww_flush_slot` splits into unprotected
workers plus a guard — region A is shown below; region B has the same shape but
the weaker recovery described further down:

```c
static int pww_flush_guarded(pending_member_t *pm)
{
    jmp_buf      env;
    unsigned int sdwa[26];          /* 104-byte SDWA copy */
    long         rc;
    int          frc = -1;

    rc = _setjmp_stae(env, (unsigned char *)sdwa);
    switch (rc) {
    case 0:                                     /* armed - do the work */
        frc = pww_flush_worker(pm);
        if (_setjmp_canc() != 0)
            log_warn("pww_flush: _setjmp_canc failed");
        break;

    case 1:                                     /* abend intercepted */
        frc = pww_flush_recover(pm, ABEND_SYSCODE(sdwa));
        break;

    default:                                    /* could not establish */
        log_error("pww_flush: STAE not established (rc=%ld); "
                  "flushing UNPROTECTED", rc);
        frc = pww_flush_worker(pm);
        break;
    }
    return frc;
}
```

On the `default` path we still attempt the flush.  Refusing would guarantee
data loss, whereas an unestablished STAE is rare and usually means the system
is already in trouble; it is logged at ERROR (and WTO'd) so it is visible.

**What the application actually sees — `B14`, not `D37`.**  The abend chain
observed in `jcl/t#stae1.jcl` is two-stage:

1. The data write exhausts the dataset and MVS raises **`D37`**.  That is
   written to the system console / job log but **does not reach the
   application** — the JCC runtime absorbs it.
2. The runtime then performs its **own internal `fclose()`** of the member,
   whose STOW cannot complete.  *That* failure abends with **`B14`**, and `B14`
   is the completion code the application's STAE actually receives in the SDWA.

Two consequences.  First, recovery must key off **`B14`** — the code we can
really see — as well as the `x37` family; a design that recognised only `D37`
would fall through to the generic `EIO` path and mis-report the condition.
Second, **we do not control the close**: the runtime has already issued it by
the time we regain control, so there is no "abandon the stream without STOWing"
option available to us at the C level.

**A partial member is left behind.**  The prototype confirms it — after the
out-of-space abend the PDS contains a **partially written member**.  We cannot
prevent that from C, so the design must accept it and make it *visible* rather
than silent:

- Log **and WTO** the failure prominently (step 1 below), so the operator knows
  both that the dataset is full and that a member is suspect.
- *Optional follow-on (not required for the first cut):* because the flush
  reads the member's existing directory entry up front for ISPF stats (§9.1),
  we hold its **original TTR**.  A `STOW REPLACE` with that saved TTR would
  restore the previous content, and a `STOW DELETE` would remove a member that
  did not exist before — both are directory-only operations needing no data
  space.  That would turn "silently truncated" into "unchanged", and is the
  natural second iteration once the basic trap is proven.

**Recovery steps** (`pww_flush_recover`), in order:

1. **Log and WTO** the failure with the decoded code, e.g.
   `pww_flush: TEMP.ITEST.FB(LARGE) abended S0D37 - dataset out of space`.
   An out-of-space condition needs operator attention, so it goes to the
   console, not just the log.
2. **Drop our reference to the member stream** — the runtime has already closed
   it (that is what raised the `B14`), so recovery simply clears the saved
   `FILE *` and must **not** touch it again.  Because the abend unwinds out of
   `pww_write_member`, keep it in a module-scope `static FILE *g_flush_fp` (the
   server is single-threaded and flushes one slot at a time), set on open and
   cleared on close or recovery.
3. **Unallocate the member and DEQ the enqueue — mandatory, and in that
   order.**  `pww_lock` takes the `SPFEDIT` enqueue **first** and then
   allocates, so release must run in the **exact reverse**: unallocate, then
   DEQ.  Releasing in the same order as acquiring risks a **deadly embrace**
   with another task doing the same thing, so this order must not be "tidied".

   Neither step may be skipped: a retained allocation leaks a ddname for the
   life of the task, and a retained enqueue locks the member against ISPF
   **indefinitely** — the worst residue, because it outlives the failed request
   and blocks a human.  Critically, each step therefore runs under its **own**
   STAE rather than sharing one: after a `B14` the dataset's DCB has just been
   closed badly, making the unallocate the likeliest thing to fail, and with a
   shared guard its failure would unwind past the DEQ and strand the enqueue.
   Separate guards mean both are always attempted; if either abends it is
   reported and the fatal flag is set, so the task ends and MVS reclaims what
   we could not.

   Release is issued by the **caller** (`pww_flush_idle` / `pww_flush_all` /
   the eviction path), not by the flush: an earlier version released inside
   `pww_flush_slot`, which zeroed the slot underneath the caller — it then
   logged an empty `flush failed for ()` and released a second time.
4. **Mark the slot clean and free** so neither the idle sweep nor a later
   COMMIT retries it.  Retrying an out-of-space write only abends again; the
   buffered data is discarded and the client has been told.
5. **Set `errno`** from the abend code and return −1.

**Abend → errno → NFS status.**

| Abend | Meaning | `errno` | NFS3 status |
|---|---|---|---|
| `B14` | Member CLOSE/STOW failed — **in practice the visible face of an out-of-space `D37`** (see above), and also what a directory-full STOW surfaces as | `ENOSPC` | `NFS3ERR_NOSPC` |
| `B37`, `D37`, `E37` | Out of space / no extents, if ever surfaced directly | `ENOSPC` | `NFS3ERR_NOSPC` |
| anything else | **Not recovered** — probable program error; loud message + shutdown | `EIO` | `NFS3ERR_IO` |

**Only the out-of-space family is recovered — deliberately.**  The guard spans
more than the PDS writes (it also covers the spill read-back and the in-place
ASCII→EBCDIC translation), so it *can* intercept abends that have nothing to do
with running out of space.  Recovering from those would be actively harmful: an
`S0C4` from a bad pointer or a buffer overrun would be dressed up as a soft
"I/O error", repeated silently, and the underlying bug hidden — the exact
opposite of what you want while chasing a corruption bug.

So `pww_abend_recoverable()` whitelists `B14`/`B37`/`D37`/`E37` and nothing
else.  Anything outside that set is reported with unmistakably different wording
("UNEXPECTED abend ... probable PROGRAM ERROR") and sets a **fatal flag**
(`pww_fatal_abend()`).  The main `select()` loop polls it alongside the operator
STOP check and shuts the server down cleanly.  Continuing would mean serving
from state we no longer trust; ending the task also makes MVS reclaim any
allocation or enqueue the cleanup could not release.  The narrower alternative —
hoisting the read/translate out of the guarded region so only `fwrite`/`fclose`
are covered — was considered and rejected: it costs a STAE per 4 KB chunk, and
a real bug in that code will surface as an `S0C4` or as plainly wrong output
anyway.

`vfs_errno_to_nfs3` already maps both, so `proc_commit` returns a proper error
and the client reports "no space left on device" — exactly the right diagnosis.
**Region B — a stats failure must NOT fail the flush.**  There is a real edge
case here.  A *new* member can be written and stowed successfully by the
runtime's `fclose` **without** user data; our follow-up `STOW REPLACE` then
rewrites that entry *with* the 30-byte ISPF stats, which makes the directory
entry longer.  If the PDS has run out of **directory blocks**, that replace
cannot be absorbed and fails — by abend, or by a non-zero `mvs_stow` return
code.  Both must be handled, since a directory-full can surface either way.

The crucial point is that this is **not** a data failure:

- the member's content is complete and already visible in the directory;
- only its ISPF statistics are missing.

So region B's recovery is deliberately *weaker* than region A's: **log a
warning and report the flush as successful.**  Treating it as a failure would
return `NFS3ERR_NOSPC` for a write that genuinely worked — a false negative,
which is worse for the client than absent metadata.  The buffered data must not
be discarded-and-retried either: it is already on disk.

**It degrades gracefully, and it does not repeat.**  A directory entry with no
user data is exactly what the read path already expects from a member created
by IEBGENER — `mvs_pds_member_entry_set` sees fewer than 30 bytes of user data
and falls back to `mvs_set_no_ispf_stats`, so the member stays fully readable
over NFS, just with synthetic timestamps.  Better still, the condition is
self-limiting: `mvs_build_write_stats` returns "no stats wanted" for an existing
member that lacks the ISPF-stats flag (§9.1), so **later flushes of that member
skip the stats STOW altogether** instead of re-abending on every write.

**What each trigger sees.**  A COMMIT-driven flush propagates the error to the
client.  An **idle-sweep** flush has no client to answer, so it logs/WTOs and
drops the slot; the data is lost, which is unavoidable once the dataset is
full, but the server keeps running and every other slot is unaffected.

**The reporting gap — silent loss on an asynchronous flush.**  A **COMMIT**-driven
flush can answer the client: `proc_commit` returns `NFS3ERR_NOSPC` and the
failure is reported properly.  Every other trigger — the **idle sweep**, pool
**eviction**, and **shutdown** — runs with no client request in flight, so the
error has nowhere to go and the loss is silent.  That is an accepted constraint
for the first cut; the ways out, in rough order of value:

1. **Make the client COMMIT (the strongest lever).**  RFC 1813 requires a client
   that receives `UNSTABLE` on a WRITE to COMMIT before it may discard its copy
   — and COMMIT is synchronous, so it carries our error.  Today the server
   *echoes* the client's requested stability (§4), so a `FILE_SYNC` client
   (Windows) is told the data is already safe and never commits: precisely the
   case that loses data silently.  Always answering `UNSTABLE` would force a
   COMMIT and close most of the gap.  It re-opens the §4 trade-off that led to
   echoing, so it is a deliberate decision, not a free win.
2. **Sticky per-member error (the POSIX writeback model).**  Linux reports a
   deferred writeback error to the *next* `fsync`/`close` rather than dropping
   it.  The same works here: on flush failure record `(dsname, member, errno)`
   in a small fixed table, and have the next NFS operation resolving to that
   member — COMMIT, WRITE, SETATTR or GETATTR — return the error once and clear
   the entry.  Clients nearly always touch the file again (a final GETATTR at
   close is near-universal), so this converts most silent losses into a reported
   one for the cost of a few table entries.  It composes with (1).
3. **Fail fast on the next write — IMPLEMENTED.**  Testing turned this from a
   nice-to-have into a necessity.  A full PDS abends on *every* flush, and each
   abend is expensive (D37 + B14 + STAE recovery + console messages): a client
   copying 300 files into a full dataset produced ~600 abends and minutes of
   console spam, with the server merely surviving each one.  So `mvspww` now
   **remembers the dataset**: an out-of-space abend records the dsname, and
   `pww_create` / `pww_write` refuse writes to it up front with `ENOSPC`.

   That collapses the abend storm to a single abend, and — because CREATE and
   WRITE are **synchronous** — it also closes the reporting gap *for this case*:
   the client genuinely sees the error on every subsequent file instead of
   losing them silently.  Only the very first member is lost quietly.

   The memory expires (`PWW_FULL_EXPIRY_SEC`, 60s) so the server recovers on its
   own once an operator adds space, and any successful flush to that dataset
   clears it immediately.  A small fixed table (`PWW_FULL_REMEMBER`) holds the
   recent offenders.  The predicate is `pww_dataset_is_full()` — named for the
   *dataset*, never the slot pool (§5), which is a separate concern.
4. **Check space before accepting.**  Consult the dataset's free extents at
   CREATE / first write and refuse early with `ENOSPC` on the (synchronous)
   WRITE.  Preventative rather than reactive, and it cannot know the eventual
   size, but it would cheaply catch "this dataset is already essentially full".
5. **Operator visibility.**  Some loss will always be un-reportable to the
   client, so the WTO in step 1 is not optional — it is the backstop that makes
   the condition discoverable at all.

Recommendation: ship the trap first (this section), then add (2), and treat (1)
as a separate, explicit revisit of §4.

**Residual risks.**

- A **partially written member** remains in the PDS after an out-of-space abend
  (proven by the prototype) until the optional TTR-restore follow-on above is
  implemented.  The WTO is what stops that being silent.
- The release steps themselves can abend.  Each has its own STAE so one cannot
  skip the other, but a step that abends genuinely does strand its resource — a
  leaked ddname is survivable, a **stranded `SPFEDIT` enqueue is not** (it
  blocks ISPF on that member for the life of the task).  That is why such a
  failure sets the fatal flag: **the task shuts down and MVS reclaims both the
  allocation and the enqueue at address-space termination.**

  *Decision (agreed):* no orphan-retry table.  An earlier proposal was to record
  unreleased resources and retry them on each idle sweep.  It is not worth the
  complexity — MVS reclamation at task end already covers the case completely,
  and it only fails for a task that is hung rather than ended, which is exactly
  what the fatal flag prevents.
- A STAE cannot recover from every failure mode (e.g. a system-forced
  termination); this reduces the blast radius, it does not make NFSD
  unkillable.
- Protection is applied to the flush path only.  The same pattern should later
  be extended to the PDS **directory read** and **member read** paths, which
  can also abend on a damaged dataset.

**Testing hook.**  The automated integration suite drives this: test
**1.3 `upload_full_dataset`** writes members into a deliberately tiny PDS
(`TEMP.ITEST.FBSMALL`, 1 track).  Before this change that test killed the
server.  The assertion is deliberately **not** "a write failed" — per the
reporting gap, a flush landing on the idle sweep has no request in flight to
fail — but "**the server survived**", proven by writing and verifying a member
in a healthy dataset afterwards.

The first run taught two lessons, both now folded in: the original 300 attempts
cost ~600 real abends and minutes of console spam for no extra signal (the
attempt count is now 12), and the abend storm itself motivated the fast-fail in
(3) above.  With that in place the expected shape is: **one** abend for the
first member, then every later attempt refused synchronously with `ENOSPC`.

## 8. Memory strategy (the 8 MB address-space constraint)

The MVS 3.8J application address space is ~8 MB for code + data, so we cannot
assume a whole file fits in memory — a large NFS file could easily exceed it.
**Think JES2 source**.

### Phase 1 — in-memory buffers, capped

Start with in-memory buffers and a hard per-member cap (a tunable
`PWW_MAX_MEMBER_BYTES`) plus a pool-wide budget.  Most PDS members (source,
JCL, copybooks) are well under this.  A write that would exceed the cap returns
**`NFS3ERR_NOSPC`** (the pool sets `errno = ENOSPC`; JCC has no `EFBIG`, so
`NOSPC` is used rather than `FBIG`).  Simple, correct, and enough to get
create/write working end-to-end.

### Phase 2 — spill to a temporary dataset

> **Status: designed, not yet implemented.**  Phase 1 keeps the whole member in
> memory (`PWW_MAX_MEMBER_BYTES`).  Phase 2 keeps only a small prefix in memory
> and moves the byte stream to a **temporary PS dataset** once it grows past a
> threshold, so member size is bounded by scratch DASD, not by the 8 MB address
> space.  Feasibility is proven by §11.1 test 1 (random `fseek` read/write on a
> RECFM=F BLKSIZE=4096 PS dataset).  It stays entirely behind the `pww` API — the
> VFS/NFS layers do not change.

#### 8.1 Threshold and transition

A slot starts in memory exactly as today.  As soon as a WRITE would take the
member's logical size (`high_water`) past `PWW_SPILL_THRESHOLD` (**16 KB**), the
slot *spills*:

1. open the slot's temporary dataset (`spill_open`);
2. copy the in-memory buffer `[0 .. high_water]` to it at offset 0
   (`spill_write`);
3. `free()` the in-memory buffer and mark the slot spilled (`spill.fp != NULL`).

From then on every WRITE for that member goes straight to disk — the server
never holds more than ~16 KB of member data in memory, whatever the member's
final size.  (A large *first* WRITE — e.g. a 32 KB `wsize` chunk at offset 0 —
crosses the threshold immediately and is written straight through, never
buffered in full.)

#### 8.2 The temporary dataset

One PS dataset per slot: `DSORG=PS RECFM=FB LRECL=4096 BLKSIZE=4096`, opened in
**binary** mode.  The 4 KB fixed block lets JCC map any byte offset to a single
block, so an `fseek` to an arbitrary offset costs one block's I/O.  JCC's
`&&`-prefixed name is a temporary dataset, created on first open and
auto-deleted when the server task ends.

A single `"w+b"` + full-DCB open does everything the slot needs — it **creates**
(or, on slot reuse, **truncates**) the dataset and then serves the random-access
phase the slot holds for the member's life, all on one handle (proven on MVS,
§8.7):

```c
/* slot index n -> its own reusable scratch dataset */
sprintf(name, "//DSN:&&PWWSP%02d", slot_index);
fp = fopen(name, "w+b,pri=15,sec=15,rlse,unit=sysda,"
                 "dsorg=ps,recfm=fb,blksize=4096,lrecl=4096");
```

- **binary** (`b`): the stream is stored untranslated — ASCII→EBCDIC conversion
  happens only at flush (§9).  JCC zero-pads the trailing partial 4 KB block,
  which is harmless: the slot tracks the logical length (`spill.size`) itself
  and never reads past it.
- `"w+b"` is create/truncate **and** read/write: the full DCB is required to
  create (`dsorg`/`recfm`/`blksize`/`lrecl`, `unit=sysda`, and `pri`/`sec` TRK
  space with `rlse`); on slot reuse the same open truncates the previous
  member's content.  The handle serves the write phase (`fseek`+`fwrite`); to
  read the content back reliably it is closed and reopened `"r+b"` first (§8.3)
  — reopening by the same `&&PWWSP<nn>` name, which is why the slot index is
  kept in the slot.

#### 8.3 Writing a segment — full-block read/modify/write

The obvious implementation — `fseek` to the offset and `fwrite` the bytes — does
**not** work on JCC for a *partial-block* write.  A few bytes written into the
middle of a 4 KB block are lost once the handle seeks to another block and back:
the read returns the OLD block content (confirmed on MVS by an early
`tmvspww` spill test).  JCC keeps only whole-block modifications durable — which
is exactly what `t#seqrw2`'s full-block random write/read proved works.

So `spill_write` does its own **full-block read/modify/write**: for every 4 KB
block the write touches, it loads the block (reading it if it already exists,
else a zero block), overlays the bytes that fall in that block, and writes the
**complete** block back.  JCC therefore only ever sees full-block writes at
block-aligned offsets.

```
spill_write(pm, off, data, len):                 /* len==0 = zero-extend to off */
    new_size = max(spill.size, off+len or off)
    for each 4 KB block b that the write or the fill-to-new_size touches:
        load block b            /* fread if b already on disk, else zero-fill */
        overlay data bytes that land in block b
        store block b           /* full 4096-byte fwrite */
    spill.size = new_size
```

Two useful consequences fall out for free:

- **Holes are zeros.** A block that never held data is loaded as zeros, and any
  bytes of an in-extent block past the logical end were written as zero padding,
  so a gap left by an out-of-order WRITE reads back as zeros (§5.2) with no
  special case.
- **No `fseek` past EOF.** Blocks are written in increasing order up to the new
  extent, so each store `fseek`s at most *to* EOF (a new block appended at the
  end), never beyond it — the one thing JCC would reject.

Reads stay byte-granular (`fseek` + `fread`); only writes need the block RMW.

**Committing before a read (the hard-won part).** On JCC the *readable* EOF of an
open, still-extending temp dataset does **not** advance past ~one track
(12 × 4096 = 49 152 bytes) until the dataset is **closed** — a block written
beyond that reads back as `0`, even after `fflush`.  (First seen as a ~146 KB
copy corrupting where a smaller one didn't; then caught deterministically by
`tmvsspl`'s reference model, which failed with a zero-length read at exactly
byte 49 152.)  So before a read that could reach committed-but-beyond-EOF data,
`spill_sync()` **closes and reopens** the scratch `"r+b"` (no truncate) to commit
the EOF.  It runs **lazily** — a `spill.dirty` flag, set by every block write and
cleared by the reopen — so a run of consecutive reads (the flush's read pass)
pays a single reopen, and the sequential-append RMW of just the tail block
(still buffer-resident, so it reads fine after a plain `fflush`) pays none.

A `SETATTR` truncate (§12.1) on a spilled member needs no special file surgery:
growing it zero-extends through the same `spill_write` path, and shrinking it
goes through `spill_shrink()`, which only lowers the recorded extent — the
flush reads only `[0 .. high_water]`, so bytes beyond it on disk are ignored
(and overwritten if the member grows again, because `spill_write` zero-fills
any hole ahead of the current extent).

#### 8.4 Reading it back — pending reads and the flush

Two paths read a pending member: `vfs_pread` (a member is readable from the
buffer *before* it is STOWed, §5.1) and the flush itself (§7).  Both must work
whether the member is in memory or spilled, so `pww` grows one accessor:

```
pww_read_range(pm, off, dst, len)   /* from pm->buf, or spill_read() if spilled */
```

`vfs_pread` and `pww_flush_slot` call it instead of touching `pm->buf`
directly.  At flush the member is streamed in 4 KB blocks — read block →
ASCII→EBCDIC → `fwrite` to the text-mode member (§9).  Because the text-mode
runtime forms a record at each EBCDIC newline regardless of how the bytes are
`fwrite`n, block-at-a-time streaming produces exactly the same records as the
current one-pass write.

The flush reads the content **once**: `pww_write_member` counts the ISPF records
(one per LF, plus a trailing partial line — §9.1) off each ASCII chunk as it
goes, so there is no separate line-counting read pass over the spill.  (An early
version read the spill twice — count then write — which not only doubled the I/O
but interleaved a second full read pass with a backward seek on the open stream,
a fragile pattern on JCC.)

#### 8.5 Lifecycle — one scratch dataset per slot, reused

JCC deletes `&&` datasets only at program termination and allows at most 1000
per run, so we must **not** mint a fresh one per member on a long-running
server.  Instead each **pool slot** owns one scratch dataset named from its
index (`&&PWWSP00`…): when a slot spills, the `"w+b"` open (§8.2) truncates any
leftover content from the slot's previous member.  The scratch file is closed
(`spill_close`) when the slot is released — the flush/idle/evict/discard/
shutdown paths that already free the buffer and drop the enqueue + allocation
(§7.2).

So at most `PWW_MAX_PENDING` (4) scratch datasets ever exist: created lazily,
reused for the life of the server (the `"w+b"` open truncates the previous
member's content — confirmed on MVS, §8.7), and cleaned up by JCC at shutdown.
No explicit delete is needed and the 1000/256 JCC limits are never approached.

#### 8.6 New module and naming

The temp-file mechanics live in their own module, `src/mvsspl.c` / `mvsspl.h`,
with a distinct `spill_` prefix so they are never confused with the
member-write path (`pww_write` / `pww_write_member`, which write the *real* PDS
member):

| Function | Role |
|---|---|
| `spill_open(pm)` | Create/truncate + open the slot's scratch dataset in one `"w+b"`+DCB `fopen`; set `spill.fp`, `spill.size = 0` |
| `spill_write(pm, off, data, len)` | Place a segment at `off`, zero-extending past EOF first (§8.3) |
| `spill_shrink(pm, size)` | Lower the logical extent (a shrink only; grow via `spill_write`).  Cannot fail |
| `spill_read(pm, off, dst, len)` | Read `len` bytes from `off` (for `pww_read_range` and the flush) |
| `spill_close(pm)` | `fclose` the scratch dataset; clear `spill.fp` / `spill.size` |

`mvspww.c` calls these when a slot is (or is becoming) spilled.  The state they
maintain lives in the `pww_spill_t spill` sub-struct of `pending_member_t`
(§5), which means `mvspww.h` gains `#include <stdio.h>`; the scratch *name* is
derived from the slot index at `spill_open` time and need not be stored.

**This API is the boundary.**  `mvsspl.c` is the only module that assigns to
`pm->spill.*`; `mvspww.c` reads `spill.fp` to decide whether a write or read
takes the memory or the disk path, and changes the state only by calling the
functions above.  The pairing to keep in mind is `pww_store_range()` /
`pww_read_range()` in `mvspww.c` — between them they are the only two places
that care where a member is backed, so nothing else has to.

#### 8.7 Parameters and open items

| Parameter | Value | Notes |
|---|---|---|
| `PWW_SPILL_THRESHOLD` | **16 KB** | in-memory prefix; a write past this spills the slot |
| Scratch DCB | `DSORG=PS RECFM=FB LRECL=4096 BLKSIZE=4096`, `unit=sysda` | 4 KB block = one-block `fseek` granularity |
| Scratch space | `pri=15,sec=15` TRK, `rlse` | raise `sec` if large members are common |
| Scratch name | `&&PWWSP<nn>` | `nn` = slot index; one per pool slot, reused |
| `PWW_MAX_MEMBER_BYTES` | *(raised)* | absolute member cap, now bounded by scratch space not memory; an over-cap WRITE still returns `NFS3ERR_NOSPC` |

**Confirmed on MVS** (`jcl/t#seqrw2.jcl`, and the earlier `"wb"` truncate test):

- A single `"w+b"`+DCB open (`unit=sysda`, `pri=15`/`sec=15` TRK,
  `dsorg=ps recfm=fb blksize=4096 lrecl=4096`) **creates** the scratch dataset
  and then supports random `fseek`/`fwrite`/`fread` — including byte-granular
  (non-block-aligned) offsets — on that one handle.
- Re-opening a live `&&` scratch with the `"w+b"` open **truncates** it cleanly
  (the `"w"` truncate semantics), so a pool slot reuses its scratch dataset for
  successive members (§8.5) — no `_unlink`/re-create dance is needed.

Still to confirm during implementation:

- `pri=15`/`sec=15` TRK is enough headroom for the member sizes you expect
  (bump `sec=` otherwise).

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

## 9.1 ISPF statistics (set at STOW)

A member written through NFS should look native in ISPF — with version/mod
level, creation/changed dates, line counts and a "changed by" id in the member
list (ISPF edit, browse, REVIEW, RPF etc.) These live in the PDS **directory 
user data**, which is exactly what the read path already decodes in 
`mvs_extract_ispf_stats` (`src/mvspdir.c`). Write is the inverse: build that 
user data and hand it to `STOW`.

**Mechanism.** JCC's `__setstow(handle, buffer, length, ttrn)` is *meant* to
record directory user data for the `STOW` that `fclose` performs, but it does
**not work for an FB PDS member opened through JCC's stdio path** — it returns
`-1`/`EINVAL`. This was ruled out empirically on MVS 3.8J: with the handle from
`_fileno()` confirmed (via `__get_ddndsnmemb`) to be the correct member, the
DCB open (post-`fflush`), and every argument inside its documented range
(`length` = 30, even, 0–62; `ttrn` = 0, 0–3), it still rejected the call. This
matches the JCC doc's warning that "not all file types allow user-data"; the
routine appears intended for load-library members, not FB source members.

So the stats are applied by two small **assembler helpers** (prototypes in
`src/asmutils.h`), called from `pww_flush_slot` **after** `fclose` has stowed
the member (the member must already exist in the directory), and while the
slot's `SPFEDIT` enqueue is still held (§7.2), so the directory rewrite is
serialised against an editor just like the content STOW:

1. `mvs_dynalloc(ALLOC, FREE=CLOSE, dsname, NULL, ddname)` (`src/mvsdalc.asm`)
   — the same SVC 99 `DISP=SHR` allocator used for the member write (§7.2),
   here called **dataset-level** (member = `NULL`), because `STOW`'s
   `BLDL`/`FIND` work on the directory. It returns the system-assigned ddname
   (via a `DALRTDDN` text unit on the allocation itself — §7.2). `FREE=CLOSE`
   means the allocation is freed when the dataset is closed, so no explicit
   unallocate is needed (this stats allocation is *separate* from, and
   short-lived relative to, the member allocation the slot holds).
2. `mvs_stow(ddname, member, userdata, len)` (`src/mvsstow.asm`) — opens that
   ddname (`DSORG=PO`), does `BLDL` to get the member's `TTR`, **`FIND` by that
   TTR to position the DCB on the existing member**, then `STOW TYPE=REPLACE`
   with the 30-byte user data, and closes (which also frees the allocation).

The `FIND` step is the crux: `STOW ADD/REPLACE` stamps the directory entry with
the DCB's *current* position, not the TTR in the list. Positioning to the
member's own TTR first means the entry is rewritten **in place** — same TTR, no
member rewrite, no orphaned blocks — while the user data is replaced. (`STOW
TYPE=C` is a rename, not a user-data edit, so it can't be used here.)

A failure is logged once and is not fatal — the member content is still valid,
it just lacks stats. The MVS external names are
`MVSDALC`/`MVSSTOW` (aliased from the C names in `asmutils.h`).

**Format** — 30-byte non-extended ISPF user data, the strict inverse of
`mvs_extract_ispf_stats`, produced by `mvs_encode_ispf_stats`:

| Off | Len | Field | Encoding |
|----|----|----|----|
| 0  | 1 | version         | 1-byte **binary** |
| 1  | 1 | mod level       | 1-byte **binary** |
| 2  | 1 | flags           | `0x00` (non-extended) |
| 3  | 1 | changed seconds | packed BCD |
| 4  | 4 | created date    | packed decimal `0CYYDDDF` |
| 8  | 4 | changed date    | packed decimal `0CYYDDDF` |
| 12 | 2 | changed time    | BCD `HH MM` |
| 14 | 2 | current size (lines) | native `uint16` |
| 16 | 2 | init size       | native `uint16` |
| 18 | 2 | mod count       | native `uint16` |
| 20 | 8 | userid          | EBCDIC, blank-padded |
| 28 | 2 | reserved        | `0x0000` |

Dates are encoded from the stored `time_t` via `gmtime` (the codebase treats
MVS time as UTC throughout); the size fields are stored native-endian to
mirror the decoder's `*(unsigned short *)` read, so encode/decode round-trip on
both the EBCDIC target and the little-endian test host.

**Update rules** (`mvs_build_write_stats`), applied on each flush:

- The member's current directory entry is read via `mvs_pds_get_member_entry`
  **before** the member is opened for output, so the PDS is never open for
  input and output at the same time.
- **New member** (not yet on disk): version/mod `01`, created = now, changed =
  now, init size = current size = line count, mod count 0, id `NFSD`.
- **Existing member that has ISPF stats:** keep version, created date, init
  size, mod count and **id**; set changed = now and current size = line count;
  **increment the mod level, capped at 99**.
- **Existing member with no ISPF stats:** left as-is — no stats are fabricated
  (the member is stowed without user data).

**Line count** is the number of records the member will have: one per LF, plus a
final record for a trailing partial line.  The flush accumulates it in
`pww_write_member` as it streams the content through its single read pass (§8.4)
— for both in-memory and spilled members — rather than making a separate pass.
It tests for `0x0A` — not `'\n'`, which is the EBCDIC newline under JCC.  (The
standalone `mvs_ispf_count_lines` in `mvspdir.c`, and its unit tests, remain as
the reference for the same rule.)

**Decisions (confirmed):**

- Version and mod level are stored as **binary**, not packed BCD (identical for
  values < 10; verify against a real member with mod ≥ 10).
- On rewrite of an existing member the **original id is kept** (ISPF's field is
  conventionally the last modifier, but the requirement is to preserve it).

**Limitations / future:**

- Only **non-extended** (16-bit) stats are written; a line count over 65,535 is
  clamped. Such members are outside the Phase 1 256 KB cap for realistic line
  lengths; emitting extended (32-bit) stats is a later option.
- Each flush does one directory read to fetch prior stats. Flushes are
  infrequent (COMMIT / idle / evict / shutdown), so this is acceptable.

Helpers live in `src/mvspdir.c` (next to the decoder) with unit tests in
`tests/tmvspdir.c` (`/count_lines`, `/encode` round-trip, `/build_stats`).

## 10. Files impacted

| File | Change |
|------|--------|
| `src/mvspww.c` / `.h` (new) | Pending-member write pool: buffer alloc/grow, place-at-offset, idle-flush sweep, flush-one (record conversion + `STOW`), flush-all. `pww_lock`/`pww_unlock` take + release the `SPFEDIT` enqueue and `DISP=SHR` allocation, held from first write to slot release (§7.2). After flush, sets ISPF stats via `mvs_dynalloc()` + `mvs_stow()` (§9.1). **Phase 2 (§8):** spills to a temp dataset past `PWW_SPILL_THRESHOLD`; adds `pww_read_range()` and calls into `mvsspl`. **Abend protection (§7.3):** two STAE-guarded regions (`_setjmp_stae` / `_setjmp_canc`, `<mvsutils.h>`) — **A** the member write (abend ⇒ real failure; decode SDWA, `B14`→`ENOSPC`; mandatory unallocate **and** `SPFEDIT` DEQ under nested protection; slot dropped, not retried) and **B** the ISPF-stats STOW REPLACE (abend *or* non-zero `mvs_stow` rc ⇒ warn only, flush still reports success) |
| `src/mvsspl.c` / `.h` (new — **Phase 2**, §8) | Temp-dataset spill store: `spill_open` / `spill_write` (zero-extending past EOF) / `spill_read` / `spill_close`, over one reusable `&&PWWSP<nn>` scratch PS per pool slot (`DSORG=PS RECFM=FB BLKSIZE=4096`, binary; one `"w+b"` open per spill). Distinct `spill_` prefix keeps it separate from the member-write path |
| `src/mvsdalc.asm`, `src/mvsstow.asm`, `src/mvsenq.asm`, `src/asmutils.h` (new) | Assembler helpers: SVC 99 `DISP=SHR` dynamic allocation returning its ddname via `DALRTDDN` (`mvs_dynalloc`); BLDL/FIND/STOW-REPLACE ISPF-stats update (`mvs_stow`); ENQ/DEQ/TEST on `SPFEDIT`, exclusive, `RET=USE`, scope `SYSTEMS` (`mvs_enq`, §7.2); C prototypes + `MVSDALC`/`MVSSTOW`/`MVSENQ` name aliases in `asmutils.h` |
| `src/mvsvfs.c` | `vfs_pwrite` → route into the write pool; `vfs_create` → create pending member; `vfs_stat_pds_member` → check pending pool so in-progress members are visible; `vfs_errno_to_nfs3` maps the write path's `EACCES` (member locked → `NFS3ERR_ACCES`) and `EROFS`/`EXDEV` |
| `src/nfs3.c` | `proc_write` → echo the client's requested stability; `proc_commit` → flush the member then reply; `proc_create` unchanged in shape |
| `src/nfsd.c` | Call `pww_flush_idle()` after `select()`; flush-all on STOP before shutdown; strengthen the write verifier (JES job id / boot counter) |
| `src/mvspdir.c` / `.h` | ISPF-stats encode + update helpers (§9.1): `mvs_ispf_count_lines`, `mvs_encode_ispf_stats`, `mvs_build_write_stats` |
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
6. **REMOVE / RENAME / SETATTR(size=0 truncate)** — all now implemented.
   SETATTR(size) routes to the pool; REMOVE uses JCC's `_unlink()` on
   `//DSN:dsname(member)` (the earlier belief that JCC could not delete a
   member was wrong); RENAME uses JCC's `rename()` between two
   `//DSN:dsname(member)` paths, restricted to the same PDS (a cross-PDS
   request returns `NFS3ERR_XDEV`) — the earlier belief that JCC had no
   in-place member rename was also wrong.

## 12. Suggested implementation order

1. `mvspww` pool with **in-memory** buffers (Phase 1), place-at-offset, and
   a flush-one that does the record conversion + `fopen("wt")…fclose`.
2. Wire `vfs_create` / `vfs_pwrite` / `vfs_stat` (pending visibility).
3. `proc_write` → echo requested stability (§4); `proc_commit` → flush.  End-to-end create +
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
- `proc_write` → **echo the client's requested stability** (§4), not a fixed
  `UNSTABLE`/`FILE_SYNC`.
- `proc_commit` → flush that member (convert → EBCDIC → `fopen("wt")…fclose`
  = STOW), then reply with the verifier.
- **SETATTR must succeed** (it is part of the client's open/write/close
  sequence): `vfs_truncate` routes to the pool (truncate-to-0 starts a fresh
  member; the common `O_TRUNC` case; a truncate to the member's current size is
  a no-op that does **not** re-dirty the slot, avoiding a redundant second STOW
  after WRITE→COMMIT→SETATTR).  `vfs_set_times` maps the requested modification
  time onto the member's ISPF **changed** date via `pww_touch_stats`: a
  stats-only STOW (no content rewrite, no mod-level change) that fires only when
  the member already has ISPF stats and the client's time actually differs (to
  the second) from the stored date — so a plain copy does nothing, a
  timestamp-preserving copy or a `touch` updates the date.  Mode/uid/gid are
  still ignored.  A SETATTR error aborts the whole copy on the client, so this
  path always returns success.
- Select-loop sweep: flush per-member idle timeouts; flush-all on STOP.

**Deferred to Phase 2 (not in Phase 1):**

- PS scratch-dataset spill.  Consequently Phase 1 has a **hard per-member
  cap**: a write that would exceed it returns `NFS3ERR_NOSPC` (JCC has no
  `EFBIG`).  This is fine for validating the flow with normal-sized members
  (source, JCL).

**Phase 1 parameters:**

| Parameter | Value | Notes |
|-----------|-------|-------|
| `PWW_MAX_MEMBER_BYTES` | **256 KB** | per-member in-memory cap; over-cap write → `NFS3ERR_NOSPC` (`ENOSPC`; JCC has no `EFBIG`) |
| `PWW_MAX_PENDING`      | **4**      | concurrent pending members in the pool |
| `PWW_IDLE_TIMEOUT_SECONDS` | **3 s** (tunable) | per member, polled each select iteration; flushes any uncommitted writes and **releases the slot** — dropping the SPFEDIT enqueue + allocation (§7.2) — this long after the last write/commit |

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
- Cross-PDS **RENAME** (moving a member to a different dataset): not possible
  as a rename; `vfs_rename` returns `NFS3ERR_XDEV` so the client falls back to
  copy+delete.  (**Same-PDS RENAME is now implemented** via JCC's `rename()`
  between two `//DSN:dsname(member)` paths — `vfs_rename` in `src/mvsvfs.c`.
  It flushes any pending write for the source first, and
  **REMOVE is implemented** via JCC's `_unlink()` — `vfs_remove` in
  `src/mvsvfs.c`.  It discards any pending write for the member first so a
  later flush can't recreate it, then bumps the directory mtime / invalidates
  the readdir cache like the STOW path.)
- MKDIR / SETATTR mode/uid/gid.
- Concurrent writers to the same member (last-flush-wins is acceptable
  initially; the pool is keyed per member).
- PDSE-specific behaviour (MVS 3.8J is PDS only).
