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

### Why the clients differ

The difference is in **request pattern**, not in client robustness.

Linux, once the dataset fills, produces writes that `pww_write` refuses before a
slot is ever created — in STC02315, 24 refusals against 10 abends. Nothing
accumulates dirty, so the idle sweep has nothing to flush and never abends.

Windows leaves five or six members pending simultaneously. When the dataset
fills, the sweep then attempts to flush each of them in turn.

Combined Linux exposure to date: **2,420 tests over 110 passes, zero hangs.**

Note on interpretation: this establishes that Linux does not *reach* the failing
state at anything like the Windows rate. It does not establish that Linux is
immune, because the abend and sweep-abend counts for the 100-pass run were not
captured. Without those counts the run cannot distinguish "structurally avoids
the trigger" from "hit the trigger and was lucky 100 times".

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

Both confirmed by `_write2op` markers, not inferred from the last log line.

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
