# Predicting if a given member can be written to PDS

The goal of this change is for the NFSD server to predict, on each NFS write request,
whether or not there is sufficient space remaining in the PDS to satisfy the request.

## Info we need on the PDS

* Record Format F(B), or V(B).
* Logical record length.
* Block size.
* Number of (full) blocks per track when PDS member is writtent to disk.
* Size of secondary extents (or no secondary extents) and how many secondary
  extents are already allocated.
* The number of tracks currently allocated in the PDS.
* The TTR of the last block that was written to the PDS.
* The amount of space remaining in the last track that contains the last
  written block.

# 1. MVSDSCB Utility

The current utility program will need to return two additional fields from the VTOC
DSCB Format 1 for the dataset -- The DS1LSTAR and DS1TRBAL fields.

The information from these fields will be part of the new `dataset_dscb_info_t` structure
(see next section).

# 2. Config file processing

* Get the DSCB information from the VTOC for all exported datasets.
* Call the MVSDSCB utility to get this information.
* Reject exported datasets if DSORG is not PO.
* Reject exported datasets with error if they are not RECFM=F/FB/V/VB.
* Reject any exported datasets for which we could not get the DSCB info
  (any error that came back from MVSDSCB).
* Save DSCB information in a new structure type `dataset_dscb_info_t` (to be defined)
  which will be a member of `pds_dataset_t`. I suspect this new structure will need to
  be defined in `src/nfsd.h`.
* I have deliberately not prefixed the new structure with "pds" in case we later start
  supporting the export of sequential datasets.
* `dataset_dscb_info_t` should contain DSORG, RECFM, BLKSIZE, LRECL, all space allocation
  information, allocated extends, current track allocation, secondary allocation size,
  creation date and referenced date, device type etc. In short just about everything.
  It should the current DS1LSTAT information field from the DSCB so we know the location
  of the last block written to the PDS.
* The new structure type `dataset_dscb_info_t` will be "C" friendly, unlike the `mvs_dscb_info_t`
  which is based around the fields coming from the DSCBs in the VTOC.
* Calculate the number of full blocks that can be written to a track in this PDS, based on
  block size and device type. This count will be added to a new member of the `pds_dataset_t`
  structure.

# 3. NFS Write Processing

When each NFS WRITE request is processed, it will update an estimation of the blocks
that will be needed in the PDS to write the member to disk.

After updating the estimated total block count and size of last block, it will be
determined (estimated) if the member could currently be written to the PDS given
the dataset space allocation and the current end of the PDS as written.

**Multiple pending writes for same dataset:** This decision needs to take into account
that there may be multiple pending writes for the same dataset. This need to be included
in the decision process.

Processing Steps --

1. Based on the data that has come in on the NFS write update the full block count,
and partial block size for the data that we currently have in the pending_member_t slot.

2. Get the latest DS1LSTAR and DS1TRBAL data from the VTOC.

3. Walk through all pending_member_t slots for the same dataset and see if they will all
   fit in the target dataset.

4. If we predict they fit then we can proceed with the NFS_WRITE

5. If we predict they will not fit then we respond NOSPC error to the NFS WRITE

### For RECFM=F(B) datasets

* Count number of records (by linefeeds) of regular length.
* Count records that exceed LRECL and will be wrapped.
* Gives total number of logical records to be written, which translates to a number.
  of whole blocks and size of last partial block (if any).

### For RECFM=V(B) datasets

* Count number of records (by linefeeds)
* Have to work through each text line and see what will fit in each block of given size.
* Accound for RDW and BDW 32 bit words for each logical record and each block.
* Will give a count of full blocks and a size for the last partial block if any.
* Blocks will be variable size, but can be assumed to be the maximum size for track
  usage calculation, which is the worst case scenario.

---

# 4. Module layout

| file | change | why |
|---|---|---|
| `src-asm/mvsdscb.asm` | modify | return `DS1LSTAR`, `DS1TRBAL` |
| `src/asmutils.h` | modify | two new fields in `mvs_dscb_info_t`, size assertion |
| `src/nfsd.h` | modify | new `dataset_dscb_info_t`; new member of `pds_dataset_t` |
| `src/exports.c` | modify | fetch DSCBs, validate, reject bad exports |
| `src/mvsblkc.h` | modify | 8-char aliases, a few added prototypes |
| **`src/mvsblkc.c`** | **new** | all block/track/TTR arithmetic and record counting |
| `src/mvspww.h` | modify | `blkcalc_info_t` field on `pending_member_t`; pool accessor |
| `src/mvspww.c` | modify | **one call site in `pww_write`**, plus init/reset |
| `jcl/makejcc.jcl` | modify | compile + link `MVSBLKC` |
| `tests/tmvsblkc.c` | new | unit tests (`TMVSBLKC` is exactly 8 characters) |
| `tests-jcl/testrun.jcl` | modify | compile + link the new test |

The design intent is that `mvspww.c` grows **one** call and nothing else of
substance. Everything the prediction needs — including walking the pending
pool — lives behind that call in `mvsblkc.c`.

---

# 5. `src/mvsblkc.c` — proposed contents

## 5.1 Public (already in `mvsblkc.h`)

| function | role |
|---|---|
| `blkcalc_info_init` | set RECFM/BLKSIZE/LRECL, zero the counters |
| `blkcalc_add_blocks_for_data` | fold one write's bytes into the running estimate |
| `blkcalc_will_member_fit` | does this member fit, given a starting TTR; returns the predicted end TTR. **Signature changed — see §7.6** |

## 5.2 Public (proposed additions)

| function | role |
|---|---|
| `blkcalc_dataset_init(dataset_dscb_info_t *out, const mvs_dscb_info_t *raw)` | config time: convert the raw DSCB into the C-friendly form and derive blocks-per-track |
| `blkcalc_admit_write(...)` | the single entry point `pww_write` calls — see §6.8 |

## 5.3 Static helpers

| function | role |
|---|---|
| `blkc_recs_fb()` | ASCII byte run → logical record count for RECFM=F/FB, honouring LRECL wrap — spec in §7.3.1 |
| `blkc_recs_vb()` | same for V/VB, accounting for RDW (4) per record and BDW (4) per block — spec in §7.3.1 |
| `blkc_blocks_to_trks()` | full blocks + partial → tracks, via `mvs_blocks_per_track()` |
| `blkc_ttr_add()` | advance a TTR by n blocks, using blocks-per-track and `DS1TRBAL` |
| `blkc_vtoc_read()` | re-read `DS1LSTAR`/`DS1TRBAL` for a dataset — called once per admit, unconditionally (§7.4) |

`mvs_blocks_per_track()` already exists in `mvsutl.h` and is documented as
**exact** for 3390, 3380 and 3350 — no new device arithmetic is needed.

---

# 6. Change map, file by file

## 6.1 `src-asm/mvsdscb.asm`

Add `DS1LSTAR` (3 bytes, TTR of the last block written) and `DS1TRBAL`
(2 bytes, bytes remaining on that track) to the `DSCBOUT` DSECT.

**Append them at the end.** Every existing offset in `mvs_dscb_info_t` stays
put, so nothing else has to be re-checked. 96 + 5 → pad to **104**.

## 6.2 `src/asmutils.h`

```c
    uint8_t  lstar[3];          /* offset  96 - DS1LSTAR, TTR of last block */
    uint8_t  trbal[2];          /* offset  99 - DS1TRBAL, bytes left on trk */
    uint8_t  reserved2[3];      /* offset 101 - alignment padding           */
} mvs_dscb_info_t;              /* 104 bytes                                */
```

Update the size assertion to 104. That assertion is the only thing standing
between a repacked struct and reading garbage — it must change with the
struct, not after it.

## 6.3 `src/nfsd.h`

New `dataset_dscb_info_t` as described in §2, plus one derived field the
prediction needs on every call:

```c
typedef struct {
    uint8_t   valid;             /* 0 = DSCB could not be read              */
    uint8_t   dsorg;             /* decoded, not raw bytes                  */
    uint8_t   recfm;
    uint16_t  blksize;
    uint16_t  lrecl;
    char      volser[7];         /* NUL-terminated, unlike the DSCB form    */
    uint8_t   devcode;           /* devtype[3]                              */
    uint32_t  trklen;            /* bytes per track                         */
    uint32_t  trkcyl;
    uint32_t  tracks;            /* currently allocated, all extents        */
    uint8_t   nextents;
    uint32_t  sec_tracks;        /* 0 = no secondary / not convertible      */
    uint8_t   sec_flags;
    uint32_t  sec_qty;
    /* Re-read from the VTOC on every admit decision -- see §7.4.  These
       are the only fields in this struct that are not config-time constants. */
    uint32_t  lstar_tt;          /* DS1LSTAR track                          */
    uint8_t   lstar_r;           /* DS1LSTAR record                         */
    uint16_t  trbal;             /* bytes left on the lstar track           */
    int       blocks_per_track;  /* derived; 0 = unknown                    */
    uint32_t  create_date;       /* unpacked                                */
    uint32_t  ref_date;
} dataset_dscb_info_t;
```

Add to `pds_dataset_t`:

```c
    dataset_dscb_info_t dscb;
```

`pds_dataset_t` has no "new fields last" rule (unlike `pending_member_t`), so
placement is free — next to `dcbinfo` reads best.

## 6.4 `src/exports.c`

`mvs_dscb()` takes a **list** of dsnames and fills an array, so this is one
call for the whole config, not one per dataset. That argues for a new pass
after parsing rather than an addition to `dataset_init`:

```
exports_load()
    ... existing parse loop ...
    cfg_load_dscb_info();        /* NEW */
    cfg_drop_failed_exports();   /* existing -- picks up anything failed above */
```

`cfg_load_dscb_info()`:

1. Walk every export's every dataset, building the `dsnlist` array.
2. One `mvs_dscb(MVS_DSCB_REQ_INFO, 0, dsnlist, data)` call.
3. Per dataset, apply the policy from §2 and call `cfg_fail_export()` on any
   breach:
   * entry `status != MVS_DSCB_ST_OK` → fail
   * `DSORG != PO` → fail
   * `RECFM` not F/FB/V/VB → fail
   * `blocks_per_track == 0` (geometry unreadable) → fail
4. Otherwise `blkcalc_dataset_init(&ds->dscb, &data[i])`.

Failing the *export* rather than the dataset matches the existing rule that a
partially-applied export is worse than a missing one (§10.1 of the export
options design), and `cfg_drop_failed_exports()` already does the removal.

Note `dataset_init` keeps its existing `mvs_get_dcb_info_dsn` call. The two
sources overlap on RECFM/LRECL/BLKSIZE; worth a cross-check log line, and
worth deciding later whether `dcbinfo` can simply be derived from `dscb` and
the older call dropped.

## 6.5 `src/mvsblkc.h`

Two gaps in the header as committed:

**8-character external names.** JCC truncates external symbols to 8, and
`blkcalc_` is already 8, so *every* public function collides. Add the alias
block the other modules use:

```c
#define blkcalc_info_init            blkcInit
#define blkcalc_add_blocks_for_data  blkcAddB
#define blkcalc_will_member_fit      blkcFit
#define blkcalc_dataset_init         blkcDsIn
#define blkcalc_admit_write          blkcAdmW
```

**Typo:** the closing guard comment reads `MVSBLKK_H_INCLUDED` (K, not C).
Harmless, but it should match.

## 6.6 `src/mvspww.h`

Add to `pending_member_t`:

```c
    blkcalc_info_t blkcalc;      /* running block estimate for this member  */
```

**This must go at the END of the struct**, per the existing comment there —
fields inserted mid-struct shift every offset after them, and a module built
against an older header then reads the wrong fields. That comment is load
bearing.

`mvspww.h` will need `#include "mvsblkc.h"`. `mvsblkc.h` must **not** include
`mvspww.h` — see §6.7 for how the pool is reached instead.

Also add a pool accessor so `mvsblkc.c` can do the multi-slot walk without
`mvspww.c` growing a loop:

```c
/* Slot i of the pending pool, or NULL if i is out of range or the slot is
   free.  Exposed so the space predictor can walk pending members for a
   dataset; nothing else should use it. */
pending_member_t *pww_slot_at(int i);
```

## 6.7 `src/mvsblkc.c` — dependency direction

`mvsblkc.h` must include `nfsd.h`, because §7.6 puts `dataset_dscb_info_t` in
the public signatures. The resulting chain has no cycle:

```
mvspww.h  ->  mvsblkc.h  ->  nfsd.h  ->  types.h, mvsio.h
```

`nfsd.h` does not include `mvspww.h` (checked), so nothing closes the loop.
`mvsblkc.c` additionally includes `mvspww.h` so it can walk the pending pool;
that include stays in the `.c` and out of the header.

## 6.8 `src/mvspww.c` — the single insertion

`pww_write` is already a clean sequence of named steps. The prediction becomes
one more, placed **after** the slot exists and **before** `pww_store_range`:

```c
    pww_record_write_gap(pm);

    /* Predict whether this member -- and every other pending member for the
       same dataset -- will still fit once stowed.  Refusing the WRITE here is
       what keeps the flush from abending S B14 out of space, which is what
       can leak the runtime's file lock and hang the server
       (doc/analysis_io_lock_hang.md). */
    if (blkcalc_admit_write(pm, (const char *)data, (int)count, offset) < 0) {
        log_warn("pww_write: %s(%s) refused -- predicted not to fit",
                 dsname_ebcdic, member_name);
        errno = ENOSPC;
        return -1;
    }

    if (pww_store_range(pm, offset, data, count) < 0)
        return -1;
```

`blkcalc_admit_write` does all of it: takes a **copy** of `pm->blkcalc`, folds
in this write, walks the pool for other pending members of the same dataset,
decides, and only on success commits the copy back. Deciding on a copy avoids
any rollback path — on refusal nothing has been modified and the member is
exactly as it was.

Two smaller changes in the same file:

* `pww_slot_new` / `pww_slot_init` must call `blkcalc_info_init` with the
  dataset's RECFM/BLKSIZE/LRECL.
* `pww_create`'s truncate branch (`pm->high_water = 0`) must reset the
  estimate too, or a re-create inherits the old member's block count.

## 6.9 `src/mvspwfl.c` — no change

Because the VTOC is read on every admit decision (§7.4) there is no cached
value to invalidate after a STOW, so the flush module is untouched.

The existing `pdsflush_dataset_is_full` guard **stays**. Prediction is an
estimate; the guard is the backstop for when the estimate is wrong.

---

# 7. Design decisions

All settled except §7.5, which is deferred out of scope.

## 7.1 The estimate is only valid for sequential append — SETTLED (option b)

`blkcalc_add_blocks_for_data` is cumulative — it folds each write into a
running total and carries `last_unterm_text_line_chars` across calls. That is
correct **only** while writes arrive strictly sequentially with no overlap.
dino_nfs does not have that guarantee:

| case | what happens today | effect on a cumulative estimate |
|---|---|---|
| sequential append | offset advances by the byte count | correct |
| Windows small append | client re-sends from offset 0 (read-modify-write) | double counts everything |
| rewrite of a pending member (test 2.4) | `pww_create` truncates, then writes from 0 | stale unless reset |
| out-of-order write into a CREATEd slot | legal, design §5.2 | miscounts |
| `SETATTR` truncate | shrinks `high_water` | estimate too high |

Options:

* **(a) Recompute from the whole member on every write.** Always right. Cheap
  while in memory (≤16 KB) but a full spill-file re-read once the member
  spills, on every write — too expensive.
* **(b) Fast path plus recompute.** Track the byte offset the estimate has
  consumed. If `offset == consumed`, fold incrementally. Otherwise recompute
  from 0 via `pww_read_range` (which already hides memory-vs-spill). Correct
  in all cases, and the expensive path is taken only by the writes that
  actually need it.
* **(c) Conservative fallback.** On any non-sequential write, estimate from
  `high_water` and LRECL assuming worst-case record density. Never
  under-estimates, but will refuse writes that would have fitted.

**Recommendation: (b).** The common NFS pattern is sequential, so the fast
path dominates; and unlike (c) it cannot produce a false ENOSPC, which would
be a visible regression in the tests that currently pass.

This needs one extra field in `blkcalc_info_t`:

```c
    uint32_t  consumed_upto;    /* byte offset the estimate has folded in */
```

## 7.2 RECFM=V/VB long lines — SETTLED, and it is a config-time check

Long text lines **wrap** on V/VB exactly as they do on F/FB (proven on this
system). What causes the `S002-14` abend is a *dataset definition* problem:
the logical record length cannot exceed `BLKSIZE - 8`.

That makes it a **config-time validation**, not a per-write one — the
condition is a property of the dataset and never changes at run time. Add to
the `cfg_load_dscb_info()` policy in §6.4, for V/VB only:

```
    LRECL - 4 > BLKSIZE - 8    ->  fail the export
```

i.e. reject when `LRECL > BLKSIZE - 4`. No per-write `EINVAL` path is needed,
and `blkcalc_add_blocks_for_data` does not need to return a validity error.

### 7.2.1 The same rule explains an earlier abend — and NFSD's own log DD is misdefined

On 2026-07-30 a single over-long `log_warn` killed NFSD with `S002-14` on its
`RECFM=V` STDERR DD. That was recorded at the time as "V does not wrap". Given
that V/VB *does* wrap, the real cause is the rule above:

```
NFSD STDERR DD:  RECFM=V  BLKSIZE=250, no LRECL
                 -> LRECL defaults to BLKSIZE-4 = 246
                 -> legal maximum is  BLKSIZE-8 = 242
                 -> misdefined by 4 bytes
```

A record of 243..246 bytes passes the LRECL check and then cannot fit the
block. That is exactly the boundary the abend hit.

**Out of scope here, but worth fixing separately:** state `LRECL=242` on that
DD (or raise BLKSIZE to 254). `log_emit()`'s split at `LOG_LINE_MAX` (200)
in `src/logger.c` currently works around it and should stay either way, but it
is a workaround for a bad DCB rather than a property of RECFM=V.

## 7.3 JCC's text-mode record splitting — SETTLED

The flush does not construct records itself. It opens `"wt"` and `fwrite`s the
whole EBCDIC stream, letting the runtime split on newline
(`pdsflush_write_member`, `src/mvspwfl.c`). So the record count the PDS
receives is whatever **JCC** produces, and `blkcalc` must reproduce it exactly.
Confirmed behaviour:

| question | answer |
|---|---|
| F/FB line longer than LRECL | wraps into whole records — 100 chars at LRECL=80 gives **two** 80-byte records, 160 bytes in the block |
| consecutive newlines, F/FB | one blank record each, 80 bytes of EBCDIC space (`0x40`) |
| consecutive newlines, V/VB | one zero-length record each, **4 bytes** (the RDW alone) |
| trailing bytes with no final newline | written as a record, same as if terminated |
| padding | F/FB **are** padded; V/VB are **not** |

### 7.3.1 Resulting arithmetic — the spec for `blkc_recs_fb` / `blkc_recs_vb`

Split the ASCII stream at `\n`. Every line, **including empty ones and the
unterminated trailing run**, produces at least one record.

**RECFM=F / FB**, `L = LRECL`:

```
records(line of n chars) = (n == 0) ? 1 : (n + L - 1) / L
bytes per record         = L          (padded with 0x40)
records per block        = BLKSIZE / L
```

**RECFM=V / VB**, usable data per record `D = LRECL - 4`:

```
records(line of n chars) = (n == 0) ? 1 : (n + D - 1) / D
bytes per record         = 4 + min(remaining, D)      (no padding)
block                    = 4 (BDW) + records, up to BLKSIZE
```

Per §3, treat every V/VB block as full-size for the track-usage calculation —
the worst case, and it keeps the prediction conservative.

### 7.3.2 Incremental counting across writes

`last_unterm_text_line_chars` exists for the case where a write ends
mid-line. The rule that avoids double counting:

* Count records only for lines **terminated within this chunk**.
* Carry the trailing unterminated run's *length* forward, not a record count.
* When more data arrives, its first line's length is `carry + new`, and the
  record count is computed from that total — so a wrapped line is counted
  correctly no matter where the chunk boundary falls.
* At fit-check time, if `carry > 0`, add `records(carry)` for it, because JCC
  will write those trailing bytes as a record.

## 7.4 VTOC freshness — SETTLED: read every time

`DS1LSTAR` / `DS1TRBAL` can move *within* a single member's write sequence,
so a value cached for the life of that sequence would be wrong:

1. concurrent NFS writes to two or more members of the same dataset — the
   normal case on Windows, where five or six members sit pending at once and
   any flush among them advances the end of the dataset;
2. a TSO user editing and saving a member during the sequence — less likely,
   but nothing in the server would see it.

So `blkc_vtoc_read()` runs on **every admit decision**, unconditionally. No
cache, no invalidation, no staleness window.

Cost is one VTOC read per NFS WRITE. Accepted for now; a pacing mechanism can
be added later if the I/O proves too heavy, and it will be easy to add because
the read sits behind a single function.

Two consequences worth noting:

* it is one read per *admit*, not per pending slot — the walk over the other
  pending members (§3 step 3) starts from the figures already read and threads
  the predicted TTR through them, so adding pending members costs no extra
  I/O;
* `mvspwfl.c` needs no change at all (§6.9), and `dataset_dscb_info_t` needs
  no `refreshed` timestamp.

## 7.5 Directory space — OUT OF SCOPE

A PDS directory is a fixed allocation made at create time. It can fill
independently of the data area, and when it does the STOW fails — a different
abend, but reached through the same flush path, so the same hang exposure.

**Deliberately deferred.** Recorded here so it is a known gap rather than an
oversight: after this change the data-area abend is predicted and avoided, but
a directory-full abend is not, and would still reach the flush. The existing
`pdsflush_dataset_is_full` backstop (§6.9) continues to bound the damage.

## 7.6 Signature of `blkcalc_will_member_fit` — SETTLED: pass the pointer

Replace the `export_idx` / `dataset_idx` pair with a
`const dataset_dscb_info_t *`, so the function is pure arithmetic over its
arguments and touches no global state:

```c
int blkcalc_will_member_fit(
    const blkcalc_info_t      *blkcalc_info,
    const dataset_dscb_info_t *ds_info,
    uint32_t                   last_block_ttr,
    uint32_t                  *predicted_last_block_ttr);
```

`blkcalc_admit_write` does the export/dataset lookup and the VTOC read once,
then passes the pointer down for each pending member.

This is what makes `tests/tmvsblkc.c` worth writing: every case is a
stack-allocated `dataset_dscb_info_t` filled with known geometry, with no
export table, no config file, and no VTOC. Given that the TTR and track
arithmetic is the most off-by-one-prone part of the change, being able to test
it exhaustively matters more here than anywhere else in the design.

`blkcalc_info_t` becomes `const` here too — the fit test answers a question,
it does not update the estimate. `blkcalc_admit_write` owns the update.

---

# 8. Suggested order of work

Each step should build and pass on MVS before the next.

| # | step | verifiable by |
|---|---|---|
| 1 | `mvsdscb.asm` + `asmutils.h`: the two new fields | `tests/testdscb.c` prints them for a known PDS |
| 2 | `mvsblkc.c`: record counting and block/track/TTR maths, pure functions per §7.3.1 | `tests/tmvsblkc.c`, no server needed |
| 3 | `nfsd.h` + `exports.c`: DSCB load and validation, including the §7.2 LRECL rule | server logs correct geometry for every export; a deliberately bad export is rejected |
| 4 | `mvspww.h` / `mvspww.c`: state, init, reset — **estimate computed and logged, but not enforced** | log shows predicted vs. actual across a full suite run; any disagreement is a bug found with no behaviour change |
| 5 | enforce: `blkcalc_admit_write` returns the refusal | test 1.3 returns ENOSPC with **no** `SB14` in the job log |

Step 4 is the important one. Running the predictor in observe-only mode over
the existing suite is what turns "the arithmetic looks right" into evidence,
and it costs nothing to leave it in that mode for a few runs.

Step 2 is now fully specified by §7.3.1 and needs no MVS access to write or to
unit-test — it is pure integer arithmetic. Worth doing first for that reason:
it is the part most likely to harbour an off-by-one, and it is the part that
can be tested hardest.

**Success criterion for the whole change:** a full-suite Windows run with
`upload_full_dataset` filling the PDS produces zero `IEC032I E37-04` /
`IEC217I B14-10` in the job log, and therefore zero exposure to the hang in
`doc/analysis_io_lock_hang.md`.

---

# 9. Implementation notes

Written 2026-08-03. Everything in §4–§7 is implemented except where noted.

## 9.1 DSORG bits are NOT the same in the DCB and the DSCB

The trap that would have broken this silently: `mvsio.h` defines
`MVS_DCB_DSORG_PO 0x40` / `MVS_DCB_DSORG_PS 0x02`, taken from JCC's
`__getdcb()`. The **format 1 DSCB uses the opposite bits** — `0x40` is PS and
`0x02` is PO, as `tests/testdscb.c` has always decoded it.

Validating a DSCB-sourced DSORG with the DCB constant would have classified
every PDS as sequential and failed every export. `asmutils.h` now carries a
separate `MVS_DSCB_DSORG_*` set with a warning, and `dataset_dscb_info_t`
documents which set applies to its fields.

## 9.2 Test data must be built from bytes, not string literals

A pending member's stream is ASCII (it is translated only as the flush writes
it), so `blkcalc_add_blocks_for_data` looks for `0x0A`. On JCC a `'\n'`
literal is EBCDIC `0x15`, so `"hello\n"` in a test contains **no terminator
the module recognises**. `tests/tmvsblkc.c` builds every buffer from explicit
byte values for that reason.

## 9.3 Conservative choices, and the one place they could bite

Every ambiguous choice is resolved towards "predict full": a wrong "fits"
costs an abend, a wrong "full" costs an ENOSPC the client reports.

* the current track's remaining capacity is the **smaller** of what
  `DS1LSTAR`'s record number implies and what `DS1TRBAL` implies;
* a partial last block counts as a whole block;
* V/VB blocks are assumed full-size for track usage, per §3;
* an unreadable or incomplete DSCB answers "does not fit", never "fits" —
  `blkcalc_dataset_init` refuses to set `valid` unless
  `blocks_per_track`, `blksize`, `lrecl` and `tracks` are all usable.

**The case to watch in the observe-only run (§8 step 4):** a dataset of ONE
track whose `DS1TRBAL` reads 0 gets an availability of zero and refuses
everything. TRBAL is only 0 when the track is exactly full, so this should
not arise on a real export, but a false ENOSPC would show up here first.

## 9.3.1 `DS1LSTAR`'s record number is NOT in units of blocks-per-track — FIXED

Found in the first observe-only run (STC02322). `TEMP.ITEST.FBSMALL` reported:

```
tracks=16  ext=16  blk/trk=6  lstar=15.14  trbal=102
```

Record **14** on a track that holds **6** full blocks. A TTR is a two byte
relative track address followed by a one byte **physical block** number, and
those blocks are whatever was actually written — short member-tail blocks,
256 byte directory blocks, the EOF marker. `blocks_per_track - r` is therefore
meaningless against a real `DS1LSTAR`.

`TT = 0x000F` says the last track written was 15, the last track of a 16 track
allocation. `DS1TRBAL` is the useful value.

**The fix (implemented).** The two questions are now separated in
`blkc_track_room()`:

| track | how its free room is measured | why |
|---|---|---|
| the one `DS1LSTAR` names | `DS1TRBAL / bytes-per-block`, capped at `blocks_per_track` | the only sound measure of a track whose existing blocks are of unknown size |
| one this module PREDICTED, chaining members | `blocks_per_track - r` | exact, because every block we counted is assumed full sized, so r really is in units of blocks |

`blkc_ttr_add()` now fills the current track using the same function and then
whole tracks, instead of the old `tt * bpt + r` index that mixed the units.

**A second bug this exposed.** When a chained member lands on the `DS1LSTAR`
track, `DS1TRBAL` still describes that track *as the VTOC found it* and knows
nothing about blocks already predicted onto it — so the same free space would
be credited to every pending member in turn. `blkc_track_room()` now subtracts
`r - lstar_r`. Covered by `/fit/trbal_once`, which fails against the previous
code.

## 9.3.2 The predictor was correct on its first real test (STC02322)

Same run, warn-only mode. `TEMP.ITEST.FBSMALL` was already at its 16 extent
limit with 102 bytes free. On FULL000's very first write the predictor said
*"predicted NOT to fit -- 9 blocks needed"*, and FULL000's flush is exactly
what abended `SB14`. It called all six pending members the same way, and no
member was wrongly refused. Enforcement would have prevented that abend, and
therefore the hang that followed it.

## 9.3.3 CREATE must be predicted too, and an empty member is not free

Found in the second observe-only run (STC02334, Linux, test 1.3 only, dataset
already full). Every WRITE was correctly refused with ENOSPC — and a flush
abended `SB14` anyway.

`pww_create` marks the slot dirty unconditionally ("an empty create still
needs to stow an empty member on COMMIT"). That is a promise to stow, and the
idle sweep keeps it even when every subsequent WRITE has been refused. So an
EMPTY member was flushed into the very abend this mechanism exists to prevent.

Two changes, and both were needed — the first alone would not have worked:

1. **`blkcalc_will_member_fit` charges a minimum of one block.**
   `blkcalc_total_blocks` returns 0 for a member with no content, so a
   create-time check would have computed "0 blocks needed, fits" on a
   completely full PDS. A stow always writes an EOF marker and takes a
   directory entry. Covered by `/fit/empty_costs_one`.
2. **`pww_create` runs the predictor** (`blkcalc_admit_write(pm, NULL, 0, 0)`)
   before marking the slot dirty, and releases a slot it had just created if
   the answer is no.

The gate now sits on both entry points to the pending pool, which is what §3
implied but only stated for WRITE.

## 9.3.4 SETATTR(size) is a third entry point, and it was ungated

Found by the instrumentation of §9.3.5, in STC02352 (Linux, dataset
compressed so it had room for ~6 of the 12 members test 1.3 writes).

There are **three** ways content enters the pending pool, not two:

| entry point | gated? |
|---|---|
| `pww_create` (NFS CREATE) | yes, since §9.3.3 |
| `pww_write` (NFS WRITE) | yes, from the start |
| **`pww_truncate` (NFS SETATTR size)** | **no** |

Linux preallocates: it sends `SETATTR(size=109500)` between the CREATE and
the first WRITE. `pww_truncate` zero-extended the member to 109500 bytes,
spilled it, and marked it dirty **without consulting the predictor**. Every
subsequent WRITE was then correctly refused -- but the member already existed
at full size, and the idle sweep carried it into the `E37`.

The log said so unambiguously once the figures were there:

```
blkcalc: ...(FULL010) fits -- need 1 avail 6 ...     <- the CREATE
pww_create: ...(FULL010)
spill_open: ...(FULL010) spilled to //DSN:&&PWWSP00  <- the SETATTR, ungated
proc_write ... count 65536 offset 0
blkcalc: rebuilding ... (hw=109500 consumed=0)       <- already full size
blkcalc: ...(FULL010) NOT fit -- need 15 avail 6
...
pdsflush_slot: ...(FULL010) failed though predicted to fit -- need 1 avail 6 hw=109500
```

`need 1 ... hw=109500` is the whole story: the CREATE was the last decision
the predictor was ever asked about.

**Fix.** `pww_truncate` now gates both of its paths:

* the "no pending member, truncate to 0" path creates an empty member, so it
  takes the same `blkcalc_admit_write(pm, NULL, 0, 0)` check as `pww_create`,
  releasing the slot on refusal;
* a GROW of an existing member calls
  `blkcalc_admit_write(pm, NULL, 0, (uint64_t)size)`. Passing `offset = size`
  with no data makes `blkc_build_trial` model exactly a member of `size` zero
  bytes, which is what the zero-extension produces. A SHRINK only frees space
  and is left unchecked.

**Also worth noting:** had that truncate fitted, the flush would have stowed a
member of pure zeros the client never wrote. The gate closes a data-integrity
hole as well as a space one.

## 9.3.5 Instrumentation: log the AVAILABLE side, not just the needed side

The first two failures of this kind could not be diagnosed at all, because the
predictor logged only "N blocks needed" and never what it believed was free,
nor the VTOC figures behind it. Added:

* every refusal (`WARN`, so visible at INFO level) and every admit (`DEBUG`)
  reports `need`, `avail` and `lstar/trbal/trk/ext/bpt/sec`;
* the chained-member refusal reports the other member's need and the TTR it
  chained from, instead of just a name;
* `blkcalc_info_t` carries `last_need` / `last_avail` from the last ADMIT, and
  `pdsflush_slot` prints them when a flush fails anyway -- the admitted
  decision is the one at fault, and it otherwise leaves no trace in a
  production log. That single line is what identified §9.3.4.

`blkc_need()` is shared by the fit test and the diagnostics so the logged
figure can never drift from the one the decision used.

## 9.4 Not implemented

* **§7.5 directory space** — out of scope by decision. A directory-full
  abend still reaches the flush.
* **§7.4 pacing** — the VTOC is read on every admit, unconditionally, as
  agreed. `blkc_vtoc_read()` is the single place to add throttling later.

## 9.5 Recovery if the estimate ever drifts

`consumed_upto` is compared against `high_water` on every write. Anything that
moves one without the other — a truncate, a failed `pww_store_range` after the
estimate was committed, an out-of-order write — makes the next write take the
rebuild path automatically. The estimate is therefore self-correcting rather
than needing explicit invalidation, and `pdsflush_dataset_is_full` remains as
the backstop underneath it.

