# MVS support for the VFS module

The MVS support modules reach real MVS resources — PDS directories, member
data, dataset attributes (DSORG, RECFM, LRECL, BLKSIZE) — and return them in
MVS terms. Translating those into the hierarchical, un\*x-style view that NFS
clients see is the job of the MVS VFS implementation, `mvsvfs.c`.

> **Note (2026-08-24):** this file previously documented a proposed API
> (`mvs_get_dsinfo`, `mvs_get_pdsdir`, `mvs_read`, `mvs_write` and their
> structs) that was never built under those names. None of those symbols exist
> in the source. The sketch has been replaced by the map below, which points at
> the modules that were actually written.

## Where the work actually lives

| Module | Responsibility |
|---|---|
| `mvsio.c/h` | Path classification (`mvs_path_type`), member-name validation, DCB info retrieval |
| `mvspdir.c/h` | PDS directory block parsing; ISPF statistics decode and encode |
| `mvsprw.c/h` | PDS member reads, with a sequential-read position cache |
| `mvspww.c/h` | Pending-member write pool — buffers WRITEs, STOWs on COMMIT |
| `mvsspl.c/h` | Write-spill store, backing a large pending member with a temporary PS dataset |
| `mvsblkc.c/h` | PDS space prediction — refuses a write that could not fit |
| `mvsdol.c/h` | Directory open-list pool — caches open PDS directory scans |
| `mvsfsz.c/h` | File-size cache — true text-mode sizes of PDS members |
| `mvsfid.c/h` | Stable 64-bit file IDs from dataset + member name |
| `mvsutl.c/h` | JES2 job-id lookup; CVT timezone offset and LOCAL ↔ UTC epoch conversion |

The assembler helpers live in `src-asm/` and are declared in `src/asmutils.h`:
`getcib` (MODIFY command block), `mvs_dynalloc` (SVC 99), `mvs_stow`
(BLDL/FIND/STOW), `mvs_enq` (SPFEDIT serialisation), and the DSCB reader used
by the space prediction.

## Further reading

| Document | Covers |
|---|---|
| [readme_vfs.md](readme_vfs.md) | The VFS interface every backend implements |
| [readme_mvsfsz.md](readme_mvsfsz.md) | The file-size cache in detail |
| [design_nfs_write.md](design_nfs_write.md) | The write path, enqueue and STOW |
| [design_pds_full_prediction.md](design_pds_full_prediction.md) | Space prediction and the DSCB reads behind it |
| [s370_jcc_prologue.md](s370_jcc_prologue.md) | Calling assembler from JCC-compiled code |
