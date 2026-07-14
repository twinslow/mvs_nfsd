# Design: Stable Synthetic Timestamp for Stats-less PDS Members

Status: **Proposed — open decision.** The change is *not* implemented; this
document records the problem and the candidate solutions so the choice of
synthetic value can be made deliberately. (The related directory-refresh
work — "option 3" — is being implemented separately; see
[§6](#6-interaction-with-the-directory-refresh-change).)

> **Correction (important):** this work was originally motivated by a Windows
> "Access is denied" delete failure that was attributed to the moving
> timestamp. That attribution is **wrong** — the evidence in §3 falsifies it.
> The change still stands, but on **caching-hygiene** grounds (an unstable
> synthetic timestamp is a defect in itself), *not* as a fix for the delete
> failure, whose cause remains unresolved.

Author: design discussion, dino_nfs.

## 1. Problem

A PDS member that carries **no ISPF statistics** currently reports a
modification time that **advances on every `stat`**:
`mvs_set_no_ispf_stats()` synthesises it from `gettimeofday()` on each call
(see §2). Every time a client looks at the file, the reported `mtime` (and
`ctime`) has moved.

That is a defect on its own terms — an unstable synthetic timestamp — and it
**defeats NFS client attribute caching**: a client can never conclude the
file is unchanged, so it keeps re-validating and re-fetching attributes.

It is the **same class of bug** the directory code already guards against.
See the `g_dir_epoch` rationale in `src/mvsvfs.c` ("the value we report MUST
be stable across calls"), which exists precisely because a moving *directory*
`mtime` makes the Linux client restart READDIRPLUS from cookie 0 forever.
`mvs_set_no_ispf_stats` violates that same principle for member files.

This work item makes the value stable. Note carefully what it does **not**
claim: see §3 for a Windows delete failure that was initially — and
incorrectly — attributed to this moving timestamp.

## 2. Root cause

`mvs_set_no_ispf_stats()` in `src/mvspdir.c` synthesises the member's
`crdate`/`chgdate` with a fresh `gettimeofday()` on every call:

```c
gettimeofday(&tv, NULL);
entry->crdate  = (int32_t)tv.tv_sec;
entry->chgdate = (int32_t)tv.tv_sec;
```

`vfs_stat` then maps `chgdate -> atime/mtime` and `crdate -> ctime`
(`src/mvsvfs.c`), so the member's `mtime` is literally "now" every time it
is stat'd.

The two other `chgdate = now` assignments in `mvspdir.c`
(`mvs_build_write_stats`) are **not** part of this bug: there `now` is a
caller-supplied value that becomes the member's real, *persisted* changed
date, stamped into the directory via STOW and stable thereafter.

## 3. Evidence — and the corrected conclusion

The investigation began with a Windows delete failure and an appealing but
**wrong** hypothesis that the moving `mtime` caused it. The full data:

| Case | Name has `$` | Origin | ISPF stats | mtime | Server state | `del` |
|---|---|---|---|---|---|---|
| A `$TEST1`  | yes | cold / pre-existing | none | moving | long-running | **"Access is denied"**, no `REMOVE` on the wire |
| B `TEST1X`  | no  | cold (TSO `RENAME` of A) | none | moving | long-running | deletes cleanly |
| C `$test1`  | yes | created by Windows (NFS `copy`) | yes | stable | long-running | deletes cleanly |
| D `GENER1`  | no  | cold (`IEBGENER`) | none | moving | after restart | deletes cleanly |
| E `$GENER2` | yes | cold (`IEBGENER`) | none | moving | after restart | deletes cleanly |

Corrections to earlier claims in this document:

- **TSO `RENAME` does not create ISPF statistics.** Case B therefore had
  *no* stats and a *moving* `mtime`, yet deleted cleanly. That alone
  **falsifies** "a moving `mtime` prevents deletion."
- The NFS `copy` (case C) *did* create stats (write path) and also deleted —
  but it was also created by Windows in-session, so stats and origin are
  confounded there.

The decisive case is **E**: `$GENER2` has the **same profile as the failing
case A** — leading `$`, cold/pre-existing, no stats, moving `mtime` — yet it
deleted cleanly. So none of those member properties explains the failure.
The one thing that separates A from D/E is that A was attempted on a
**long-running server** while D/E followed a **server restart**.

**Conclusion: the case-A "Access is denied" is not reproducible from the
member's properties, and now looks like a transient runtime cache-state
artifact (server- or client-side) that a restart cleared** — consistent with
how intermittent these delete failures have been throughout. If it recurs,
the diagnostic that matters is whether a `REMOVE` reaches the wire (server
refusal vs. the client refusing locally) and what the directory/handle cache
state was at the time — not the member's name or stats.

What §1–§2 establish **independently of the delete puzzle** is only that the
stats-less `mtime` is unstable. That is worth fixing for caching hygiene
regardless of whether it is ever tied to the delete failure.

## 4. Requirements for the replacement value

1. **Stable across calls** *(mandatory).* The same member must report the
   same timestamp on every `stat`. This is the whole fix.
2. **Stable across server restarts** *(desirable).* If the value changes on
   restart, every stats-less member's `mtime` jumps at once, so every client
   invalidates its cached attributes and listings for those members after
   each restart — a needless refresh storm. A value that survives restarts
   avoids it.
3. **Sane and plausible.** Not `0`/1970 (some tools treat it as "unknown"
   or render it as broken); not in the future.
4. **Deterministic / no per-call variation.**

## 5. Options

| # | Value | Stable across calls | Stable across restarts | Extra I/O | Notes |
|---|---|---|---|---|---|
| 1 | **Fixed constant** (e.g. `2000-01-01`) | ✅ | ✅ | none | All stats-less members share one arbitrary date. Simplest. |
| 2 | **Server-start epoch** (reuse `g_dir_epoch`) | ✅ | ❌ | none | Jumps on restart → client-wide cache refresh for all stats-less members (violates req. 2). Needs plumbing from `mvsvfs.c`. |
| 3 | **Per-member hash-derived date** (from `mvs_fid_hash`) | ✅ | ✅ | none | Distinct per member, but the dates are meaningless and can sort oddly. More code. |
| 4 | **PDS creation date** (`DS1CREDT` via DSCB `OBTAIN`) | ✅ | ✅ | one VTOC read per dataset (cacheable) | Real and meaningful ("dataset was created on …"); same date for every member in a PDS; needs a small assembler/OBTAIN path; date-granular. |
| 5 | Derive from TTR / directory position | ✅ | ✅ | none | No real time meaning; rejected. |

### Honest framing

Without ISPF stats there is **no per-member timestamp on disk** — any value
we report is synthetic. So the real axis is not "which value is correct"
(none is) but "which stable-but-arbitrary value looks least wrong and costs
the least machinery."

- **Option 1** is the least code and fully satisfies reqs. 1–4, at the cost
  of every stats-less member showing an identical placeholder date (which is
  harmless — identical timestamps are normal — and arguably a useful visual
  hint that "this member has no stats").
- **Option 4** is the most *meaningful* value if a fixed placeholder feels
  wrong: the member inherits its containing PDS's creation date. The cost is
  a DSCB read, which can be cached per dataset so it happens once.
- **Option 2** is tempting (mirrors the directory) but fails requirement 2
  and would trigger a client-wide cache-refresh storm on every restart.

## 6. Interaction with the directory-refresh change

This is one of **two** synthetic-timestamp issues found in the same
investigation:

- **This document (member `mtime`)** — an unstable synthetic timestamp on
  stats-less members that defeats client attribute caching.
- **Directory refresh (option 3, implemented separately)** — a PDS that is
  changed out-of-band (e.g. members added by `IEBGENER`) does not bump its
  directory `mtime`, so clients never re-read the listing. Option 3 adds a
  throttled directory-signature check that bumps `dir_mtime` on a real
  change.

They are independent fixes. Option 3 makes an out-of-band member *appear* in
Windows; this change gives stats-less members a *stable* timestamp so client
caches for them behave sanely. (Whether it has any bearing on the delete
failure in §3 is unresolved — see §3.)

## 7. Open decision

Pick the synthetic value (§5). Current lean: **option 1** (fixed constant)
for simplicity and restart-stability, with **option 4** (PDS creation date)
as the fallback if a fixed placeholder is undesirable. Once chosen, the
change is a few lines in `mvs_set_no_ispf_stats()` (drop the
`gettimeofday()`; assign the chosen stable value to `crdate`/`chgdate`).
