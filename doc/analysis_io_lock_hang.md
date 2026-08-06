# Intermittent server hang after a recovered abend

Status: mechanism identified, root cause in JCC not fully explained, mitigation
selected but not yet implemented.
Investigation dates: 2026-07-30 .. 2026-08-02.
Related: `doc/design_nfs_write.md` §7.3, §7.3.1.

---

## 1. Symptom

The server stops. Precisely:

* No further log output, on the console (WTO) or on STDERR.
* No CPU consumption.
* No response to `MODIFY` — the operator command is accepted by JES2
  (`$HASP000 OK`) but the server never acts on it.
* TCP stays healthy: the stack continues to ACK client packets, so the NFS
  client does not see a dead peer and retries indefinitely. Clients hang rather
  than fail.
* `CANCEL` is the only way out. A `SYSUDUMP` DD is not sufficient to obtain a
  dump — the task never abends on its own — so `C <job>,DUMP` is required.

The hang is always preceded, in the same run, by at least one recovered `SB14`
abend (dataset out of space) inside the pending-write flush path.

### Variance

The hang is **not** deterministic. Across the runs recorded below the same
abend, on the same code path, with the same following work, sometimes leads to
a hang and sometimes does not. Within a single run, several abends were
recovered cleanly and one was not.

---

## 2. Reproduction context

All observations come from the integration test suite (`integration-test/`),
which contains a test (`1.3 upload_full_dataset`) that deliberately fills a
small PDS. Filling the PDS causes `IEC032I E37-04` / `IEC217I B14-10` and the
`SB14` abend that the flush path traps with `_setjmp_stae`.

Server: dino_nfs on MVS 3.8J under Hercules, single-threaded, one TCB.

---

## 3. Windows vs. Linux

The two clients produce markedly different exposure.

| run | client | scope | abends | sweep abends | hang |
|---|---|---|---|---|---|
| STC02307 | Windows | full suite | 6 | 5 | yes |
| STC02309 | Windows | full suite | 3 | 2 | yes |
| STC02312 | Windows | test 1.3 only, `PWW_FULL_EXPIRY_SEC=5` | 10 | 0 | no |
| STC02313 | Windows | full suite | 5 | 1 | yes |
| STC02314 | Windows | `_setjmp_canc` experiment (§7) | 5 | not counted | yes |
| STC02315 | Linux | full suite × 10 (220 tests) | 10 | 0 | no |
| STC02316 | Windows | full suite × 10, hung on pass 9 | not counted | ≥ 1 | yes |
| STC02317 | Windows | empty PDS, clean start | 3 | 3 | yes |
| (unnumbered) | Linux | full suite × 100 (2200 tests, 3h05m) | not counted | not counted | no |

"Sweep abend" = an abend during a flush driven by the idle sweep
(`pww_flush_idle`), as opposed to one driven by an NFS COMMIT
(`pww_flush_member`).

### Why the clients differ — SUPERSEDED, see below

The original explanation was that Linux's request pattern never leaves members
pending: once the dataset fills, `pww_write` refuses each write before a slot
is created (24 refusals against 10 abends in STC02315), so the idle sweep has
nothing to flush. Windows, by contrast, leaves five or six members pending
simultaneously.

That was the right description of those runs but the wrong cause.

### Linux does reproduce it — but the reason it is rarer is UNEXPLAINED

**STC02322 hung on Linux** with the identical dump signature (§5). Linux is
not immune in kind, and the old pattern-based explanation above was never
shown to be causal.

`TEMP.ITEST.FBSMALL` at STC02322's server start was at its 16 extent limit
(`tracks=16 ext=16 blk/trk=6 lstar=15.14 trbal=102`) — the single-volume
maximum, unable to extend. No FBSMALL member was stowed in the whole run, and
six (FULL000..FULL005) piled up before anything revealed it was full.

**A candidate explanation that was tested and FAILED.** "The differentiator is
whether the dataset is already full when the burst starts" fits STC02322
exactly, and is wrong: in the 100-pass Linux run the dataset was equally full
from pass 2 onward — 99 consecutive passes of the same starting condition —
with no hang. A condition that holds constantly cannot explain an event that
happened once. Recorded here so it is not proposed again.

**The one identified but untested difference:** STC02322 was a *fresh server
start*, abending about 68 seconds after `exports_load`. The clean Linux runs
were a single server up for hours, whose `pdsflush_dataset_is_full` table had
been armed since its first abend. Whether that changes how many members reach
the sweep is unknown.

**What would settle it:** the abend and sweep-abend counts from the 100-pass
job log. If that run produced ~99 abends with zero hangs against this run's
one abend and one hang, the per-abend leak rate differs by two orders of
magnitude and needs a cause. If it produced almost none — because the
out-of-space guard stayed armed across passes and refused the writes before
any flush was attempted — then the two runs never tested the same thing, and
STC02322 is simply the first Linux run that actually reached an abend. Those
counts were not captured.

---

## 4. What the logs show

Console logging at `loglvl debug` + `wtolvl debug`. The WTO is issued *before*
the STDERR stream write (`vlog_msg`) precisely so a message escapes a deadlock
it is about to enter.

### 4.1 A recovered abend

```
IEC032I E37-04,IFG0554T,NFSD,NFSD,SYS00512,293,WORK04,TEMP.ITEST.FBSMALL
IEC217I B14-10,IGG0201Z,NFSD,NFSD,SYS00512,293,WORK04,TEMP.ITEST.FBSMALL
[ERROR] pdsflush_slot: TEMP.ITEST.FBSMALL(FULL000) ABENDED SB14 (dataset out of space) ...
[ERROR] pww_flush_idle: flush failed for TEMP.ITEST.FBSMALL(FULL000)
```

### 4.2 The hang (STC02317)

```
[DEBUG] pww_unlock: released TEMP.ITEST.FBSMALL(FULL004)
[DEBUG] spill_close: closing spill for TEMP.ITEST.FBSMALL(FULL004) ...
spill_close: entering fclose
<nothing further — 60 minutes until CANCEL>
```

### 4.3 The hang (STC02316)

```
[DEBUG] mvs_open_pds_dir: Opening PDS TEMP.ITEST.FBSMALL for directory read
[DEBUG] mvs_open_pds_dir: Calling fopen on ... //DSN:TEMP.ITEST.FBSMALL ...
<nothing further>
```

Two different file operations, same outcome.

### 4.4 A caveat the console cannot resolve on its own

`log_debug` writes to STDERR, which is itself a file operation. So the last
console line before silence identifies only *a point past which execution did
not proceed* — it cannot say whether the hang was in the operation named on
that line or in that line's own stream write.

This was resolved by adding `_write2op` markers (a supervisor call, which never
touches the C runtime's file layer, so it escapes a deadlock the stream write
would not) either side of the two candidate operations:

* `spill_close`, around the `fclose` — `src/mvsspl.c`
* `mvs_open_pds_dir`, around the `fopen` — `src/mvspdir.c`

Both are marked TEMPORARY in the source and should be removed once this is
closed out.

---

## 5. Dump analysis

A hung task produces no dump by itself at the moment it hangs. There are two
ways to obtain one, and the completion code says which was used:

| completion code | how the dump arose | informative? |
|---|---|---|
| `S122` | operator `C <job>,DUMP` | no — it is just the cancel |
| `S522` | the task exceeded the job wait time limit (1 hour in WAIT) and was terminated automatically | **yes** — an `S522` is independent confirmation that the task really was in a wait for the whole hour, not looping or making slow progress |

The remaining informative parts are the interrupt identification and the
registers at entry to abend.

### 5.1 STC02317 header

```
COMPLETION CODE      SYSTEM = 522
PSW AT ENTRY TO ABEND  078D1000 00150806        ILC 2   INTC 0001
```

This dump was **not** operator-requested. The `S522` means the task was left
alone and hit the wait time limit, which the log timeline confirms: last output
`spill_close: entering fclose` at 14.10.57, `IEF450I NFSD ... ABEND S522` at
15.11.04 — one hour later, to the minute.

`ILC 2` + `INTC 0001` = the last interrupt was `SVC 1` — **WAIT**. The task is
parked in a wait, not looping.

### 5.2 Registers at the WAIT

```
REGS 0-7   00000001  001C9998  001653FC  00000000  00182628  00000000  001507C8  FD000000
```

| register | value | meaning |
|---|---|---|
| R0 | `00000001` | WAIT count = 1 |
| R1 | `001C9998` | ECB address |
| R2 | `001653FC` | lock word address |

Storage at the lock word:

```
1653E0   00000061 00000060 0000005C 00000063    00000062 0000005D 00000009 FF000000
                                                                           ^^^^^^^^
                                                                           = 1653FC
```

`FF000000` — the value a `TS` (test-and-set) instruction writes. **The lock is
held.**

### 5.3 Where the code is

Resolving the PSW address against the linkage-editor **CSECT map**
(`NAME/ORIGIN/LENGTH`, *not* the cross-reference listing, whose `LOCATION`
column is where an adcon lives), and mapping the generated `ST000nnn` names via
the **pre-link** listing (JES2 dsid 170):

* Parked at `ST000157 +0x3C` = **`athreadlock`**.
* `athreadlock` implements a lock using `CS` (compare-and-swap) and `TS` on a
  word, issuing `SVC 1` (WAIT) on an ECB when both fail, and looping back to
  retry after it wakes. `athreadpost` immediately following issues the `POST`.
  This identification rests on the instruction pattern, not on the CSECT name.
* The lock word lives at **`@IO +0xD2CC`**. `@IO` is the CSECT that owns
  `open`/`close`/`read`/`write`; `fopen`/`fclose`/`fread`/`fwrite` are in a
  separate CSECT and call down into it. **One lock therefore serialises every
  file operation the program makes**, including every STDERR write.

### 5.4 Why this is a self-deadlock

| fact | source |
|---|---|
| the lock is held | lock word = `FF000000` |
| the ECB is not posted | ECB at `0x1C9998` = `809BCD10` — waiting, waiter = our own PRB |
| there is only one task | ASCB `TCBS = 1` |

With one TCB, the holder of the lock can only be this task. It has re-entered
`athreadlock`, failed both `CS` and `TS`, and issued `WAIT` on an ECB that only
the lock's releaser will post. **It is waiting for a lock it is itself
holding.** Even a spurious `POST` would not free it: the wake path clears the
ECB and retries the still-held lock.

The ECB address `001C9998` was identical in every dump taken.

---

## 6. Frequency and timing measurements

### 6.1 The leak is occasional, not invariable (STC02313)

Counting successful file operations after each recovered abend:

| abend | successful file operations afterwards |
|---|---|
| 1 | 397 |
| 2 | 361 |
| 3 | 393 |
| 4 | 391 |
| 5 | **0 — the next one hung** |

Four abends left the file layer perfectly healthy. One did not, and then the
very first file operation afterwards deadlocked. Each abend is an independent
event.

### 6.2 The decisive run (STC02317)

Empty PDS, clean server start. Three abends, all sweep abends. Operation mix
between each abend and the next file operation:

| abend | time | first file operation after it | outcome |
|---|---|---|---|
| 1 | 14.07.14 (FULL002) | 3 × `mvs_open_pds_dir` fopen, a readdir, then an `fclose` | all succeeded |
| 2 | 14.09.06 (FULL003) | `spill_close` → `fclose` | **returned** |
| 3 | 14.10.56 (FULL000) | `spill_close` → `fclose` | **hung** |

Between abend 3 and the hang the log records 18 refused `proc_write`s, ~30
`vfs_stat`s served from the pending-write slot, four guard-refused flushes, and
four `unalloc`/`DEQ` pairs (SVC 99 / SVC 48) — **not one file operation**.

Abends 2 and 3 are structurally identical: same sweep, same guard-refusals,
same release order, same first-file-operation-afterwards, nothing in between in
either case. One returned; one deadlocked.

This is the strongest single result in the investigation. It means the outcome
is not determined by anything in dino_nfs's own sequencing.

### 6.3 The victim is whatever comes next

| run | victim | why that one |
|---|---|---|
| STC02316 | `fopen` in `mvs_open_pds_dir` | the released slot had no spill file, so `spill_close` did nothing; the next file operation came from an unrelated client GETATTR |
| STC02317 | `fclose` in `spill_close` | the released slot had spilled |
| STC02322 | `_unlink` in `vfs_remove` | nothing did any I/O for 47 log lines; the first was the test suite's cleanup REMOVE |

The first two were confirmed by `_write2op` markers rather than inferred from
the last log line. The third is located by elimination: the last message is
`mvs_get_pds_dsn_and_member`, and the only steps between it and `_unlink()` in
`vfs_remove` are `pww_discard` and string building, none of which touch a file.

Three different calls, one mechanism. The victim is simply whichever file
operation happens next.

---

## 7. Red herrings — investigated and dismissed

### 7.1 STAE control block accumulation

Every trapped abend leaves driven SCBs chained (`USEREXIT` = the exit
`_setjmp_stae` establishes, in `@@MVSUT`). Flag bytes distinguish DRIVEN
(`TCB/RB` starts `10`, `SCBDATA` starts `08`) from a still-ACTIVE one (`00`/
`00`). In STC02317: 3 abends → 6 driven SCBs + 1 active, i.e. **2 driven SCBs
per abend**, consistent with `pdsflush_slot` establishing two `_setjmp_stae`
regions.

This looked like a leak we could close. It is not, and it is unrelated:

* **The documented contract** (https://github.com/mvslovers/jcc/blob/main/help/mvsutils.htm)
  states `_setjmp_stae` returns 0 = normal, 1 = "Something was caught — the
  STAE has been cleaned up", -1 = OS failure. The abend path is documented to
  tear the exit down by itself, and the vendor example calls `_setjmp_canc()`
  only on the normal path. dino_nfs does exactly that.
* **Experiment STC02314**: `_setjmp_canc()` was added to the `rc == 1` path to
  test documentation against observation. It returned **8 = "nothing to
  cancel"** on every one of 5 abends, the SCBs accumulated anyway, and the run
  hung in the identical place. The change was backed out; a comment in
  `pdsflush_write_member_guarded` records the measured return code.
* Conclusion: the spent exits are MVS's own bookkeeping — most plausibly
  because JCC recovers by `longjmp` rather than a proper MVS retry, leaving the
  used exit chained but marked. SCB count does not predict the hang.

**Do not propose adding a cancel to the `rc == 1` path.** It was proposed twice
and measured once.

### 7.2 Spill slot reuse

Hypothesis: the hang correlates with a spill scratch dataset being reused
across members. Tested by tracing scratch usage in the two hang runs available
at the time. `&&PWWSP04` was opened for the first and only time in both, while
slots 0 and 1 were recycled dozens of times without incident. Denied.

### 7.3 "The hang is always the spill `fclose`"

Held for several rounds and falsified by STC02316, where the released slot had
no spill file at all and the hang occurred in a directory-read `fopen` in an
unrelated later request. The correct statement is "the next file operation,
whatever it is".

### 7.4 "Linux sends no COMMIT, so it will have more sweep abends"

A prediction made from a single packet capture of test 2.3 that happened to
contain no COMMIT. STC02315 contained 14 COMMITs. COMMIT frequency is not the
variable; whether members pile up dirty is.

### 7.5 Deferring the slot release after a failed flush

Proposed as a mitigation while the "spill `fclose`" framing was current. Dead:
in STC02316 the release completed cleanly and the hang came afterwards in an
unrelated client request, so deferring it would have changed nothing.

### 7.6 Inserting a delay in the post-abend path

Proposed on the theory that the leak might be transient. Dead on two counts:
STC02317's abends 2 and 3 took an identical path with identical intervening
work and only one hung; and with a single TCB there is no other task that could
release the lock, so a held lock cannot self-heal with time.

---

## 8. Theories

### Theory A — the abend leaves `@IO`'s lock held (current working theory)

An `SB14` raised inside a C library routine and recovered via `_setjmp_stae` +
`longjmp` can leave the runtime holding `@IO`'s lock, because the `longjmp`
unwinds past the code that would have released it.

**Evidence for**

* The dump shows exactly this state and nothing else: `SVC 1` in
  `athreadlock`, lock word `FF000000`, ECB unposted, one TCB.
* Reproduced with an identical signature in every dump taken (five as of
  2026-08-01, plus STC02317).
* The hang is always preceded by a recovered abend, and never occurs without
  one.
* `longjmp`-based recovery is a plausible mechanism for skipping a release, and
  is consistent with §7.1's finding that JCC is not performing a proper MVS
  retry.

**Evidence against / unexplained**

* It does not explain why only *some* abends leak (§6.1). A supporting but
  unverified explanation: `fclose` flushes, writes the last block, closes the
  DCB and issues `STOW`; if `@IO` takes and releases its lock per operation
  rather than across the whole call, then *where the `SB14` lands relative to
  those windows* decides whether the lock is held at that instant. Confirming
  this requires JCC source, which has not been examined.
* It does not by itself explain the sweep/COMMIT asymmetry in Theory B.

### Theory B — sweep-driven abends are more dangerous than COMMIT-driven ones

**Evidence for**

Aggregating the runs in §3 that were counted:

| abend type | abends | hangs |
|---|---|---|
| sweep-driven | 8 | 3 |
| not sweep-driven | 20 | 0 |

Under a null hypothesis of equal per-abend risk, the probability that all three
hangs fall in the sweep group by chance is C(8,3)/C(28,3) = 56/3276 ≈ **0.017**.

**Evidence against / unexplained**

* **Mechanistically incoherent with Theory A.** If the lock is left held, the
  *next* file operation deadlocks whenever it comes — including the next log
  write. A COMMIT-driven abend should therefore be exactly as fatal. It is not
  obvious what the sweep path contributes beyond timing.
* **Confounded.** Both zero-sweep runs (STC02312, STC02315) differ from the
  Windows full-suite runs in more than sweep-ness: STC02312 ran a single test
  with `PWW_FULL_EXPIRY_SEC=5`, and STC02315 used a different client whose
  whole request pattern differs. The comparison is not controlled.
* **Censored sampling.** A run terminates at its first hang, so abends
  occurring after a hang cannot be observed. This biases abend counts in
  hanging runs downwards.

**Discriminating experiment (not yet run):** a Windows full-suite run with the
idle sweep disabled. Same abend exposure, zero sweep abends. If it stops
hanging, Theory B is real and needs a mechanism; if it still hangs, Theory B is
an artifact and Theory A stands alone.

### Theory C — transient lock hold

Considered and rejected. With `TCBS = 1` there is no other task to release the
lock or post the ECB, so a held lock cannot clear itself. This also rules out
any mitigation based on waiting.

### Theory E — the abend on an EMPTY member leaks the lock (CONFIRMED — this is the cause)

**The decisive pair: STC02346 and STC02347.** Identical scripts, identical
probe, one variable.

```
ls -l /mnt/itest/temp.itest.fbsmall
<<< echo XXX >  ...yyy.txt   |   touch  ...yyy.txt >>>     <-- the ONLY difference
sleep 5                                    # idle sweep flushes -> E37/B14
cat /mnt/itest/temp.itest.fb/small.txt     # probe: a DIFFERENT PDS, no cache can serve it
```

| | STC02346 (`echo`) | STC02347 (`touch`) |
|---|---|---|
| member at flush | `pm->high_water = 4` | `pm->high_water = 0` |
| flush driver | idle sweep | idle sweep |
| abend | `E37-04` / `B14-10` | `E37-04` / `B14-10` |
| slot released, SVC 99 unallocate | yes | yes |
| probe -> `mvs_open_pds_dir` | **`fopen OK`, three times** | **`Calling fopen ...` then nothing** |
| outcome | server continues, clean shutdown | **HANG** |

Everything is held constant -- same server build, same dataset, same sweep
path, same release, same abend, same probe, same call site. **Four bytes of
content is the entire difference, and it decides whether the runtime survives.**

STC02347's dump carries the usual signature exactly: `ILC 2 / INTC 0001`
(SVC 1 WAIT), R0 = 1, R2 -> lock word `00170CDC` = **`FF000000`**, R1 -> ECB
`809BCD10` (the same value in every dump taken), `TCBS = 1`. The eight words
around the lock word are byte-identical to STC02317's and STC02322's.

### For the record — a small reproduction was not found

**The obvious minimal case does NOT reproduce it.** `jcl/t#wrfull` does exactly
this and the task does not hang:

```c
rc = _setjmp_stae(b, sdwa104);
if (rc == 0) {
    ofh = fopen("//DSN:TEMP.ITEST.FBSMALL(XXX)", "wt");   /* full PDS */
    fclose(ofh);                                          /* nothing written */
}
/* then a write to a DIFFERENT PDS as the probe -- it succeeds */
```

So "empty member + abend + STAE" is not sufficient on its own.

**And the abend messages say why -- the invariant is the EOV count, not
emptiness.** Counting `IEC032I E37-04` in every run where the outcome is
known:

| run | member | `IEC032I E37` | `IEC217I B14` | hang |
|---|---|---|---|---|
| STC02342 | empty | **1** | 1 | **HANG** |
| STC02347 | empty (`hw=0`) | **1** | 1 | **HANG** |
| STC02345 | 4 bytes | **2** | 1 | no |
| STC02346 | 4 bytes | **2** | 1 | no |
| JOB02788 (`t#wrfull`) | **empty** | **2** | 1 | **no** |
| JOB02790 (`t#wrfull`) | 33 bytes | 1 | 0 | no |

Inside NFSD the correlation with emptiness is 4/4 -- but `t#wrfull`'s EMPTY
case produced TWO E37s and behaved like NFSD's non-empty case. So emptiness is
not the underlying condition; it is the lever that, **in the NFSD open path**,
produces a single trip through EOV. Across all six runs **"exactly one
`IEC032I E37-04`" predicts the hang without exception.**

That gives the small reproduction a measurable target: aim for a run logging
exactly one `IEC032I E37-04`, rather than guessing at differences and waiting
to see whether a probe hangs.

(Six data points, and the E37 count may be a symptom of where the abend landed
rather than a cause. It is used here as a signal to aim at, not as a
mechanism.)

The differences between `t#wrfull` and the NFSD flush path were, most
structural first:

1. ~~**How the member is opened.**~~ **TESTED, does NOT reproduce.** NFSD never
   opens by DSN: `pww_lock` allocates `DSN(member)` with SVC 99 and the flush
   opens the existing DD by ddname, `fopen("//DDN:SYS00002", "wt")`
   (`mvspwfl.c`). `t#wrfull` was changed to do the same and the task still did
   not hang. The open path is not the missing ingredient.
2. ~~**The same PDS was opened for a directory read moments before.**~~
   **TESTED, does NOT reproduce.** `t#wrfull` was given the same
   `mvs_open_pds_dir`-style read by DSN (`recfm=u,force`, blocks walked,
   closed) between the SVC 99 allocation and the `//DDN:` open, outside the
   STAE exactly as `pdsflush_slot` has it. Still no hang. So the
   double-allocated state is not the missing ingredient either.
3. **An exclusive SPFEDIT ENQ is held** on the member throughout. Untested,
   and now the only difference left that anyone has identified.

There is also a measurable difference nobody has explained (see the EOV table
above): the server's empty-member case drives EOV **once**, while
`t#wrfull`'s drives it **twice** — and one E37 is what every hang has in
common. Whatever ingredient is missing, its signature is that count.

**CLOSED 2026-08-05. Do not resume this on reasoning alone.**

Two plausible, well-argued hypotheses were built from a careful reading of
the server path -- the `//DDN:` open and the preceding directory read -- and
BOTH were tested and BOTH failed to reproduce anything. The remaining
candidates are of the same kind, and the record above is now mostly a list of
intelligent guesses that turned out to be wrong.

JCC has no effective support channel either, so a small reproduction has no
destination beyond this file.

Resume only on **new concrete information**: a hang whose log or dump shows
something not already recorded here, or a change in the failure's behaviour.
Not on a new theory about what `t#wrfull` might be missing.

The practical position never depended on finding the small case: the space
prediction (doc/design_pds_full_prediction.md) stops the abend happening at
all, and no abend means no exposure.

Added 2026-08-03, and it **supersedes Theories B and D**, both of which turn
out to have been measuring this variable indirectly.

An empty member flush is `fopen(target, "wt")` -> **no `fwrite` at all** ->
`fclose`. The abend lands in the close/STOW path with nothing ever having been
written through the DCB. A non-empty flush issues `fwrite` calls first, and
the `E37` fires during those instead.

**CONFIRMED BY CONTROLLED EXPERIMENT (STC02345).** The decisive run: a 4 byte
member (`echo XXX > file`), written FILE_SYNC so the client sent NO COMMIT,
left dirty and flushed by the IDLE SWEEP. It abended `E37`/`B14`, was fully
released (including the SVC 99 unallocate), and the server did **not** hang —
it served the following GETATTR and READDIRPLUS, took the STOP and ended
cleanly. The abend message carries `pm->high_water = 4`, so the member's
content is on the record.

| | **empty member** | **non-empty member** |
|---|---|---|
| **sweep-driven abend** | STC02317 FULL000, STC02322, STC02342 -> **HANG x3** | **STC02345 -> no hang** |
| **COMMIT-driven abend** | impossible (no writes => no COMMIT) | STC02317 x2, STC02321 x100 -> no hang |

The top row is a single-variable comparison: same sweep path, same release,
same unallocate, same abend -- only the member content differs, and it flips
the outcome. Emptiness is the cause; the flush driver is not.

This also refutes Theory D. The decisive half is documented: **STC02345 and
STC02346 both performed the SVC 99 unallocate after the abend** (the
`pww_unlock: unalloc / DEQ / released` lines are in each log) **and neither
hung.** So the unallocate does not cause the leak.

The converse was also tried -- a `continue` added to the failed-flush path of
`pww_flush_idle`, removing the release and therefore the unallocate, with the
hang unchanged -- but that is on the author's report rather than the logs: the
runs from that period were captured at INFO level, and `pww_unlock` logs at
DEBUG, so which builds carried the change cannot be established after the
fact. The `continue` has since been removed; `pww_flush_idle` now releases on
both the success and failure paths.

**STC02346 closes the last gap.** In STC02345 no `mvs_open_pds_dir` ran after
the abend (the READDIRPLUS was a directory-cache hit), so there was no direct
"the next file operation succeeded" line. STC02346 repeated the run with a
probe chosen to force one -- a read of a member in a DIFFERENT PDS, which no
cache can serve and which creates nothing that could abend on its own:

```
ls -l /mnt/itest/temp.itest.fbsmall
echo XXX > /mnt/itest/temp.itest.fbsmall/yyy.txt   # 4 bytes, FILE_SYNC, no COMMIT
sleep 5                                            # idle sweep flushes -> E37/B14
cat /mnt/itest/temp.itest.fb/small.txt             # the probe
```

Abend logged with `pm->high_water = 4`, released with the unallocate, and then:

```
mvs_open_pds_dir: Calling fopen on //DSN:TEMP.ITEST.FB ...
mvs_open_pds_dir: fopen OK for TEMP.ITEST.FB          (three times)
```

`small.txt` does not exist in that PDS, so the command reported ENOENT -- but
answering that correctly required real directory reads, which is exactly what
the probe was for. **The same call site that parks after an empty-member abend
completed three times after a non-empty one.**

**Supporting evidence — the separation over 105 earlier abends**

| run | abending member | content at flush | outcome |
|---|---|---|---|
| STC02317 14.07.14 | FULL002 | 109,500 bytes | recovered |
| STC02317 14.09.06 | FULL003 | 109,500 bytes | recovered |
| STC02317 14.10.56 | FULL000 | **empty** | **HANG** |
| STC02322 | FULL000 | **empty** | **HANG** |
| STC02321 x100 | various | non-empty | **0 hangs** |
| STC02342 | XXX (`touch`) | **empty** | **HANG, on demand** |

3 empty-member abends -> 3 hangs. 102 non-empty abends -> 0 hangs.
C(3,3)/C(105,3) ~ **5 x 10^-6**. STC02317 is internally controlled: three
abends in one session, same dataset, same sweep path, and only the empty one
hung.

**STC02342 is a DETERMINISTIC REPRODUCER** (Linux, blkcalc disabled, PDS at
its 16 extent limit):

```
ls -l  /mnt/itest/temp.itest.fbsmall     # works
touch  /mnt/itest/temp.itest.fbsmall/xxx.txt
ls -l  /mnt/itest/temp.itest.fbsmall     # hangs
```

`touch` creates an empty member and nothing else; the idle sweep flushes it,
`E37`/`B14`, and the next directory `fopen` parks.

**Why Theories B and D looked so strong: both were proxies.** A COMMIT-driven
flush can NEVER be of an empty member, because the client only sends COMMIT
after writing. So `COMMIT => non-empty` and `empty => sweep` hold by
construction, and the sweep/COMMIT split was measuring emptiness the whole
time. The unallocate correlation follows the same way, since the sweep is what
releases the slot.

**Theory D was refuted by direct experiment, not just by inference.** On
2026-08-03 a `continue` was added to the failed-flush path in
`pww_flush_idle`, so a flush that abends no longer releases the slot and no
SVC 99 unallocate follows the abend at all. **It made no difference to the
hang.** That is the controlled test Theory D asks for, and it fails it.

Note the change does not remove the release permanently — it defers it. The
slot stays USED and dirty, the sweep retries it (and is refused by the
out-of-space guard) indefinitely, holding its ENQ and DD allocation until the
pool fills and `pww_slot_take` evicts it as LRU, which flushes and releases it
after all. So it is a diagnostic, not a fix.

**Correction to the record.** STC02322 was first read here as a hang on a
109,500 byte member, which was the main argument against this theory. That was
wrong: `pww_write` logs `pww_write: ... off=` only AFTER `pww_store_range`
succeeds, and blkcalc had refused every FBSMALL write before that point. The
line appears 6 times in that run for other datasets and **zero** times for
FBSMALL. Do not read `nfs3.proc_write` as evidence that data was stored.

**Evidence against / unexplained**

* The Windows hang runs (STC02307, 02309, 02313, 02316) have not been
  classified for emptiness; their logs were not retained.
* The mechanism inside JCC is inferred from the call sequence, not observed.

### Theory D — the SVC 99 UNALLOCATE after the abend leaks the lock (SUPERSEDED)

Added 2026-08-03 after STC02321, and displaced within the day by Theory E
above. Retained because the correlation is real and the reasoning shows how a
proxy variable can look decisive.

The abend interrupts `fclose`/STOW, so the runtime still holds state for that
DCB. `pww_slot_release_inner` then calls `pww_unlock` — which issues SVC 99 to
**unallocate the DD** — *before* `spill_close` and before anything else. The
proposal is that pulling the allocation out from under a DCB the runtime has
not finished tearing down is what leaves `@IO`'s lock held.

**Evidence for**

STC02321 is an internal control: one client, one server instance, one dataset,
100 `SB14` abends in three hours, **every one COMMIT-driven**
(`pww_flush_member` keeps the slot), **zero followed by an unallocate**, and
**zero hangs**. Only 17 unallocates occurred in the entire run, none within
six lines of an abend.

| abend followed by an SVC 99 unallocate? | abends | hangs |
|---|---|---|
| yes (sweep -> `pww_slot_release`) | 12 | **5** |
| no (COMMIT -> slot kept) | 126 | **0** |

Probability of all five hangs falling in the group of 12 by chance:
C(12,5)/C(138,5) ~ **2 x 10^-6**.

It also accounts for all three victims, because `pww_unlock` runs FIRST in the
release:

| run | after the abend | victim |
|---|---|---|
| STC02317 | unalloc, DEQ, released | the very next `@IO` call, the spill `fclose` |
| STC02316 | unalloc, DEQ, released, no spill | next `@IO`, an `fopen` from a later request |
| STC02322 | unalloc, DEQ, released, no spill, 47 lines of no I/O | next `@IO`, `_unlink` |

And it explains what Theory A never could: why a COMMIT abend is harmless.
Nothing is pulled away, so `@IO` recovers — 126 times out of 126.

**Evidence against / unexplained**

* Still only 5 of 12 — presumably whether the runtime had already finished
  with the DCB before the SVC 99 landed, but that is untested.
* Not directly demonstrated. It is a correlation over 138 abends plus a
  plausible mechanism, not an observation of the lock being taken.

**Correction this forces to §7.5.** Deferring the slot release was retired on
the grounds that STC02316's release "completed cleanly" and the hang came
later. Under Theory D the release IS the leak point; completing without
itself hanging does not mean it did not leave the lock held. That was a bad
inference.

**The fix is NOT to skip the unallocate** (user's position, and correct — the
allocation has to be released, and leaking DDs to dodge a runtime bug trades
one resource problem for another). If Theory D is right the remedy lies in
how the slot is torn down after an abend, not in omitting the teardown.

Settling it is deferred; the space prediction removes the abend, and with it
the whole question.

---

## 9. Mitigation

No mitigation is available from C at the point of failure: the code cannot
avoid file operations after an abend (closing a spill, logging, and even
freeing all reach the runtime), and it cannot tell which abend was the bad one.

The direction is therefore to **avoid the abend**.

| measure | status | effect |
|---|---|---|
| `pdsflush_slot` refuses a flush into a dataset already known to be out of space (`pdsflush_dataset_is_full`, expiry `PWW_FULL_EXPIRY_SEC`, currently 60s) | implemented | bounds the abend rate. In STC02317 it converted 4 of the 5 sweep flushes in the fatal batch into refusals, so only the first could abend |
| pre-flight space check via `mvsdscb.asm` before attempting a flush | **not implemented — the real fix** | removes the `E37`/`B14` abend entirely, which is the only abend observed here |
| replacing `fwrite` with an assembler writer under a private ESTAE | considered, rejected | disproportionate to the problem |

See `doc/design_nfs_write.md` §7.3.1.

---

## 10. Diagnostic notes for future work

* A hang produces no dump at the moment it happens; a `SYSUDUMP` DD alone
  yields nothing immediately. Either force one with `C <job>,DUMP` (completion
  code `S122`), or leave the task alone and let the 1-hour wait time limit
  terminate it (`S522`). Leaving it is worth the wait when you can spare it:
  the `S522` itself proves the task was in a genuine wait for the full hour.
* `ILC` / `INTC` in the dump header name the last interrupt. `ILC 2` +
  `INTC 0001` = `SVC 1`.
* `REGS AT ENTRY TO ABEND` are the registers *at the WAIT* (R0 = wait count,
  R1 = ECB, R2 = lock word). The PRB's own saved registers are **not** — they
  were identical across dumps taken at different load addresses.
* Resolve a code address as `PSW − load address`, then look it up in the
  linkage-editor **CSECT map**, not the cross-reference listing. Map
  `ST000nnn` to a real name via the **pre-link** listing.
* The STAE exit address moves with every relink (`0016A418`, `0016A3E0`,
  `0016B3E0` observed). Match on `USEREXIT [0-9A-F]+` and group; do not
  hard-code it.
* JES2 output is fetchable directly:
  `http://localhost:8080/jes/print?jobid=<ID>&dsid=<N>` — dsid 2 = job log,
  105 = SYSUDUMP, 170 = pre-link, 171 = LKED.
* JCC's `fflush` is a no-op, so a hung or cancelled task loses its buffered
  STDERR tail. Only the WTO output is real-time — which is why `vlog_msg`
  WTOs before writing the stream.
* Console WTO truncates at 126 bytes; `log_emit` splits at
  `LOG_LINE_MAX` (200) to avoid an `IEC036I 002-14` on the STDERR DD, where an
  over-long record abends rather than truncating.
