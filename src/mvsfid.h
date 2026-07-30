/*
 * mvs_fid.h - MVS file identifier generation.
 *
 * Provides a stable 64-bit hash derived from an MVS dataset name and
 * an optional PDS member name.  The result is suitable for use as the
 * fileid (NFS inode number) in vfs_stat_t and as a cache key.
 *
 * Portability:
 *   - C89 compliant; no external library dependencies.
 *   - Safe on both 32-bit MVS (GCCMVS) and 64-bit Linux (x86_64).
 *   - Byte-by-byte processing: endian-neutral.
 *   - uint64_t is used for the hash value; on GCCMVS this is emulated
 *     as unsigned long long.
 *
 * MVS external symbol limit:
 *   The IEWL linkage editor on MVS 3.8j truncates external names to
 *   8 characters.  The #define block below maps the long names to
 *   short names when compiling for MVS.  The defines must appear
 *   before the prototype so both the prototype and all call sites
 *   resolve to the same short name.
 */

#ifndef MVS_FID_H
#define MVS_FID_H

/* The following will include correct header files or define types    */
/* directly.                                                          */
#include "types.h"

/* ------------------------------------------------------------------ */
/* MVS name length limits                                             */
/* ------------------------------------------------------------------ */

/* Maximum dataset name length (qualifier1.qualifier2...qualifierN)   */
#define MVS_DSNAME_MAX  44

/* Maximum PDS member name length                                     */
#define MVS_MEMBER_MAX   8

/* ------------------------------------------------------------------ */
/* MVS 8-character external symbol aliases                            */
/* ------------------------------------------------------------------ */
#define mvs_fid_hash    mvsFidHs
#define mvs_fid_ino32   mvsFidI3

/* ------------------------------------------------------------------ */
/* Prototypes                                                         */
/* ------------------------------------------------------------------ */

/*
 * mvs_fid_hash: generate a stable 64-bit file identifier from an MVS
 * dataset name and an optional member name.
 *
 * Parameters:
 *   dsname  NUL-terminated dataset name, at most MVS_DSNAME_MAX (44)
 *           characters.  Must not be NULL.  Case-sensitive: pass the
 *           name in its canonical form (uppercase on MVS/EBCDIC;
 *           uppercase ASCII on Linux).
 *
 *   member  NUL-terminated PDS member name, at most MVS_MEMBER_MAX (8)
 *           characters.  Pass NULL or an empty string "" to hash the
 *           dataset name alone (i.e. to identify the PDS itself rather
 *           than one of its members).
 *
 * Returns:
 *   A non-zero uint64_t hash value.
 *
 *   The value is stable: the same inputs always produce the same
 *   output on any platform.
 *
 *   Domain separation guarantees that a dataset-only hash never
 *   collides with a dataset+member hash, and that two dataset+member
 *   pairs that differ only in how the characters are split between the
 *   two components never collide.
 *
 * Intended use in vfs_stat_t:
 *   vs->fileid  = mvs_fid_hash(dsname, member);  -- 64-bit NFS fileid
 *   vs->raw_ino = mvs_fid_ino32(dsname, member); -- 32-bit cache key
 *   vs->raw_dev = (uint32_t)export_id;           -- identifies volume
 */
uint64_t mvs_fid_hash(const char *dsname, const char *member);

/*
 * mvs_fid_ino32: return a 32-bit cache key derived from mvs_fid_hash.
 *
 * Folds the 64-bit hash to 32 bits by XOR-ing the high and low halves.
 * Use this for vfs_stat_t.raw_ino.
 *
 * Note: two different (dsname, member) pairs may produce the same
 * 32-bit value (32-bit fold collision) even though their 64-bit hashes
 * differ.  The file handle cache uses (raw_dev, raw_ino) together as a
 * composite key, so a collision in raw_ino alone does not cause
 * incorrect behaviour -- it only means two entries share a cache slot
 * and one will evict the other.  Use fileid (the full 64-bit value)
 * where uniqueness is important.
 */
uint32_t mvs_fid_ino32(const char *dsname, const char *member);

#endif /* MVS_FID_H */
