# Limits and Tunables

Every `#define` in the server that caps, sizes or times something, with what
it governs and what changing it costs.

Protocol constants (`NFS3PROC_*`, `ACCESS3_*`, `NFS3ERR_*`, `NF3*`) are
deliberately excluded — they are fixed by RFC 1813 and are not tunable.

Generated 2026-08-08 from `src/`. If a value here disagrees with the source,
the source wins.

---

## 1. Pool sizes — how many of a thing can exist at once

| Macro | Value | File | Governs | What happens at the limit |
|---|---|---|---|---|
| `PWW_MAX_PENDING` | 8 | `mvspww.h` | Concurrent pending (being-written) members | The least-recently-used slot is flushed and released to make room — a STOW with no client request in flight |
| `MAX_OPEN_DIRS` | 8 | `nfsd.h` | Cached open directory handles (`mvsdol.c` pool) | LRU slot is claimed; its member cache is discarded |
| `MVSFSZ_CACHE_CAPACITY` | 250 | `mvsfsz.h` | File-size cache entries | Oldest entry evicted; the next `stat` of that member re-reads it |
| `MVS_RCACHE_ENTRIES` | 20 | `mvsprw.h` | Sequential-read position cache entries | LRU evicted; the next read of that member restarts positioning |
| `MAX_EXPORTS` | 16 | `nfsd.h` | Export paths in the config | Further exports are rejected at load |
| `MAX_PDS_PER_EXPORT` | 32 | `nfsd.h` | Datasets under one export (⇒ 512 datasets total) | Further datasets rejected at load |
| `MAX_CONNECTIONS` | 16 | `nfsd.h` | Simultaneous TCP connections | New connections refused |
| `PWW_FULL_REMEMBER` | 4 | `mvspwfl.c` | Datasets remembered as out-of-space | Oldest forgotten — a flush into it may abend again |
| `PWW_RECENT_FLUSH` | 8 | `mvspww.c` | Members remembered for `PERF_PWW_LATE_GAP` | Statistic loses the diagnosis, never the detection |
| `RPC_MAX_FRAGS` | 32 | `rpc.c` | Fragment offsets recorded per RPC message | — |
| `LOG_ASCII_MAX_ARGS` | 4 | `logger.h` | `log_ascii()` conversions per log call | **Fails silently** — see §5 |

## 2. Sizes — how big a thing can get

| Macro | Value | File | Governs |
|---|---|---|---|
| `PWW_MAX_MEMBER_BYTES` | 1 MB | `mvspww.h` | Absolute per-member cap; beyond it a write gets `ENOSPC` |
| `PWW_SPILL_THRESHOLD` | 16 KB | `mvspww.h` | In-memory prefix; a larger member spills to a temp dataset (`mvsspl.c`) |
| `MAX_READ_SIZE` / `MAX_WRITE_SIZE` | 64 KB | `nfsd.h` | Largest single NFS READ / WRITE payload |
| `BUF_SIZE` | 128 KB | `nfsd.h` | Socket buffers — 64 KB payload plus headers |
| `SPILL_BLK` | 4096 | `mvsspl.c` | Spill dataset block size; fixed by its DCB (`RECFM=FB LRECL=4096`) |
| `LOG_LINE_MAX` | 200 | `logger.c` | Log record split point — see §5 |
| `MAX_PATH`, `MAX_NAME` | 256 | `nfsd.h` | Path and name buffers |
| `MAX_PATH_LEN` | 256 | `mvsvfs.h` | Same, in the MVS VFS |
| `MAX_DSNAME_LEN` | 45 | `nfsd.h` | 44-char MVS dsname + NUL |
| `MVSFSZ_DSNAME_LEN` | 45 | `mvsfsz.h` | Same, in the size cache |
| `FH_DSNAME_LEN` / `MVS_DSNAME_MAX` | 44 | `nfsd.h`, `mvsfid.h` | Dsname without the NUL |
| `FH_MEMBER_LEN` / `MVS_MEMBER_MAX` | 8 | `nfsd.h`, `mvsfid.h` | PDS member name |
| `MVSFSZ_MEMBER_LEN` | 9 | `mvsfsz.h` | Member name + NUL |
| `OUR_FHSIZE` | 60 | `nfsd.h` | Our file handle — **must stay ≤ `NFS3_FHSIZE`** |
| `NFS3_FHSIZE` | 64 | `nfsd.h` | Protocol maximum (RFC 1813) |
| `MAX_FILE_EXT_LEN` | 16 | `nfsd.h` | File extension |
| `CFG_FILEEXT_MAX` | 16 | `cfgopts.h` | Same — tied to the above by a compile-time assertion in `exports.c` |
| `CFG_MAX_TOKS` | 16 | `exports.c` | Tokens per config line |
| `MVS_ISPF_STATS_LEN` | 30 | `mvspdir.h` | ISPF statistics user-data area |
| `BLKC_STATE_BUF` | 96 | `mvsblkc.c` | Diagnostic buffer for the VTOC state string |
| `BLKC_MAX_EXTENTS` | 16 | `mvsblkc.c` | Single-volume extent limit — **MVS architecture, not a choice** |

## 3. Timers and thresholds

| Macro | Value | File | Governs |
|---|---|---|---|
| `PWW_IDLE_TIMEOUT_SECONDS` | 1 | `mvspww.h` | Idle time before a pending member is flushed and its slot (with the SPFEDIT enqueue) released |
| `MVSVFS_DIR_OPENLIST_TIMEOUT_SECS` | 30 | `mvsdol.h` | Idle time before a directory slot may be reclaimed |
| `DIR_REFRESH_THROTTLE_SECS` | 10 | `mvsvfs.c` | Interval between out-of-band directory-change checks — how quickly a member added by ISPF/IEBGENER becomes visible |
| `MVS_RCACHE_MAX_AGE_SECONDS` | 5 | `mvsprw.h` | Read-position cache entry lifetime |
| `PWW_FULL_EXPIRY_SEC` | 60 | `mvspwfl.c` | How long a dataset stays remembered as out-of-space |
| `PWW_RECENT_EXPIRY_SEC` | 300 | `mvspww.c` | Statistic ring entry lifetime |
| `RPC_IO_TIMEOUT_SECONDS` | 30 | `rpc.c` | Bounds `recv_all`, so one stalled client cannot block the single-threaded server |

## 4. Growth policy

| Macro | Value | File | Governs |
|---|---|---|---|
| `MVSPDIR_MLIST_INITIAL_SIZE` | 40 | `mvspdir.h` | Directory member list, initial entries (~1.9 KB) |
| `MVSPDIR_MLIST_INCREMENT_SIZE` | 40 | `mvspdir.h` | Linear growth step |

**Do not change the increment to geometric growth.** It was measured on
2026-08-07: doubling stabilised the region at a flat ~2100 KB, against
1180 KB rising to a 1316 KB plateau with linear growth. It removed 136 KB of
growth by adding 920 KB of permanent footprint, because every list is then
over-allocated by up to 2× and the eight cached directory slots hold that
peak for the life of the server. The note is repeated in
`mvspdir_mlist_expand()`.

---

## 5. Limits that fail quietly

Most limits above are self-announcing — an allocation fails, a request is
refused, a slot is evicted. These three are not, and are worth knowing:

**`LOG_ASCII_MAX_ARGS` (4).** A fifth `log_ascii()` call in a single log
statement silently recycles the first buffer, so two `%s` arguments print the
same string. No error, no diagnostic — just a wrong message, which is
particularly unhelpful in the middle of an incident.

**`LOG_LINE_MAX` (200).** This is not a formatting preference. The STDERR DD
is `RECFM=V BLKSIZE=250`, and writing a record longer than the DD allows does
**not** truncate: QSAM abends the task `S002-14` and the server dies. That
happened on 2026-07-30 from one over-long `log_warn`. `log_emit()` splits at
this length so no caller has to count characters. See
[`analysis_io_lock_hang.md`](analysis_io_lock_hang.md).

**`MVSFSZ_CACHE_CAPACITY` (250).** Documented as "sized to hold all members of
a typical large PDS", which no longer holds for every exported dataset —
`TEMP.HASPSRC.ASM` alone has more members than that. The cache still works, it
just thrashes, so sizes are recomputed more often than intended. Correctness
is unaffected.

## 6. Where the footprint actually goes

If region size matters, these are the levers in order of effect. Measured
2026-08-07: the region plateaus at ~1316 KB with the integration-test
datasets, and no leak was found — allocations balance on every path and
dynamic allocations come and go cleanly.

1. **`MAX_OPEN_DIRS` (8).** Each cached directory slot keeps its member list
   allocated at the high-water mark of the largest directory it has held, and
   the list is never shrunk (`mvspdir_mlist_shrink()` exists but is called
   nowhere). At ~48 bytes an entry, a 900-member PDS is ~44 KB per slot, so
   eight slots is ~350 KB. **The ceiling scales with the largest directory
   served**, so a site with big PDSs sits well above the figure above.
2. **`BUF_SIZE` (128 KB)** and the static read/write buffers — fixed, and
   part of the load module rather than the heap.
3. **`PWW_SPILL_THRESHOLD` (16 KB) × `PWW_MAX_PENDING` (8)** — at most 128 KB
   of pending member buffers before members go to disk instead.

Note that on MVS `free()` returns storage to JCC's heap free list, **not** to
the system: the region only ever grows, so what you observe is a high-water
mark of demand rather than current usage. Growth that levels off is expected;
growth that never levels is not.

The single biggest reduction available is not a tunable at all —
`mvs_pds_get_member_entry()` reads from the requested member to the *end of
the directory* and then uses only `list[0]`. Bounding that read would cut both
peak and allocation churn. See the note at that function.
