/*
 * mvs_fid.c - MVS file identifier generation.
 *
 * Generates a stable 64-bit hash from an MVS dataset name and an
 * optional PDS member name, for use as a NFS fileid / cache key.
 *
 * ALGORITHM: FNV-1a 64-bit (Fowler-Noll-Vo, variant 1a)
 * Reference:  http://www.isthe.com/chongo/tech/comp/fnv/
 *
 * FNV-1a was chosen for this application because:
 *
 *   1. Simple, self-contained implementation with no dependencies.
 *   2. Processes input one byte at a time -- completely endian-neutral,
 *      so the same hash value is produced on both big-endian MVS and
 *      little-endian Linux for identical input strings.
 *   3. Good avalanche properties for the short, uppercase strings
 *      typical of MVS dataset and member names.
 *   4. C89 compatible.  The 64-bit constants are constructed from two
 *      32-bit halves to avoid any reliance on ULL literals, which may
 *      be handled inconsistently by older compiler versions.
 *   5. Negligible code size and no heap allocation.
 *
 * DOMAIN SEPARATION
 *
 *   The dataset name and member name components are separated by a NUL
 *   byte before hashing.  Because NUL (0x00) is never a valid character
 *   in either an MVS dataset name or a PDS member name, this guarantees
 *   that the concatenation of (dsname, separator, member) is unique for
 *   every distinct (dsname, member) pair:
 *
 *     "SYS1.PARMLIB" + NUL + ""       -- the PDS itself
 *     "SYS1.PARMLIB" + NUL + "IEASYS" -- a specific member
 *
 *   Without the separator, "SYS1.PAR" + "MLIB.MBR" and "SYS1.PARMLIB"
 *   + ".MBR" would hash identically; with the separator they cannot.
 *
 * USAGE EXAMPLE
 *
 *   To populate vfs_stat_t for a PDS member:
 *
 *     vfs_stat_t vs;
 *     vs.fileid  = mvs_fid_hash("SYS1.PARMLIB", "IEASYS00");
 *     vs.raw_ino = mvs_fid_ino32("SYS1.PARMLIB", "IEASYS00");
 *     vs.raw_dev = (uint32_t)export_id;
 *
 *   To populate vfs_stat_t for the PDS directory itself:
 *
 *     vs.fileid  = mvs_fid_hash("SYS1.PARMLIB", NULL);
 *     vs.raw_ino = mvs_fid_ino32("SYS1.PARMLIB", NULL);
 *     vs.raw_dev = (uint32_t)export_id;
 *
 * FNV-1a 64-BIT CONSTANTS
 *
 *   Offset basis : 14695981039346656037  (0xCBF29CE484222325)
 *   Prime        :  1099511628211         (0x00000100000001B3)
 */

#include <string.h>     /* strlen        */
#include "mvsfid.h"

/* ------------------------------------------------------------------ */
/* FNV-1a 64-bit constants, expressed as 32-bit halves for C89        */
/* ------------------------------------------------------------------ */

/*
 * 0xCBF29CE484222325
 *   high word: 0xCBF29CE4
 *   low  word: 0x84222325
 */
#define FNV_INIT_HI   ((uint32_t)0xCBF29CE4u)
#define FNV_INIT_LO   ((uint32_t)0x84222325u)

/*
 * 0x00000100000001B3
 *   high word: 0x00000100
 *   low  word: 0x000001B3
 */
#define FNV_PRIME_HI  ((uint32_t)0x00000100u)
#define FNV_PRIME_LO  ((uint32_t)0x000001B3u)

/* ------------------------------------------------------------------ */
/* Internal: assemble two uint32_t halves into a uint64_t             */
/* ------------------------------------------------------------------ */
static uint64_t make64(uint32_t hi, uint32_t lo)
{
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* ------------------------------------------------------------------ */
/* Internal: FNV-1a 64-bit multiply                                   */
/*                                                                    */
/* Computes hash * FNV_PRIME using 32x32->64 partial products to      */
/* avoid requiring a 64x64 multiply instruction.  On platforms with   */
/* a native 64-bit multiply (Linux x86_64, GCCMVS with -m64 or        */
/* software 64-bit) the compiler will optimise this to a single MULL. */
/*                                                                    */
/* Let H = (hi << 32) | lo  and  P = (ph << 32) | pl.                 */
/* H * P = (hi*pl)<<32 + (lo*ph)<<32 + (lo*pl)                        */
/* (the hi*ph term overflows 64 bits and is discarded -- intentional  */
/*  in FNV: we keep only the low 64 bits of the product)              */
/* ------------------------------------------------------------------ */
static uint64_t fnv1a_mul(uint64_t hash)
{
    uint32_t hi = (uint32_t)(hash >> 32);
    uint32_t lo = (uint32_t)(hash & 0xFFFFFFFFu);

    uint64_t cross = (uint64_t)hi * (uint64_t)FNV_PRIME_LO
                   + (uint64_t)lo * (uint64_t)FNV_PRIME_HI;
    uint64_t low64 = (uint64_t)lo * (uint64_t)FNV_PRIME_LO;

    return low64 + (cross << 32);
}

/* ------------------------------------------------------------------ */
/* Internal: FNV-1a 64-bit -- process one byte                        */
/* ------------------------------------------------------------------ */
static uint64_t fnv1a_step(uint64_t hash, unsigned char b)
{
    hash ^= (uint64_t)b;    /* XOR in the byte  */
    hash  = fnv1a_mul(hash); /* multiply by prime */
    return hash;
}

/* ------------------------------------------------------------------ */
/* Internal: FNV-1a 64-bit -- process 'len' bytes from 's'            */
/* ------------------------------------------------------------------ */
static uint64_t fnv1a_feed(uint64_t hash, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        hash = fnv1a_step(hash, (unsigned char)s[i]);
    return hash;
}

/* ------------------------------------------------------------------ */
/* mvs_fid_hash                                                       */
/*                                                                    */
/* Public function -- see mvs_fid.h for the full contract.            */
/* ------------------------------------------------------------------ */
uint64_t mvs_fid_hash(const char *dsname, const char *member)
{
    uint64_t hash;
    size_t   dslen;
    size_t   mblen;

    /* --- Seed with FNV offset basis --- */
    hash = make64(FNV_INIT_HI, FNV_INIT_LO);

    /* --- Dataset name component --- */
    dslen = (dsname != NULL) ? strlen(dsname) : 0;
    if (dslen > MVS_DSNAME_MAX) dslen = MVS_DSNAME_MAX;

    if (dslen > 0)
        hash = fnv1a_feed(hash, dsname, dslen);

    /*
     * Domain separator: a single NUL byte marks the end of the
     * dataset name component.  NUL cannot appear in a valid MVS
     * dataset name or member name, so ("DS\0MEM") is unique to
     * the pair ("DS", "MEM") and cannot collide with any other
     * split of those characters across the two arguments.
     */
    hash = fnv1a_step(hash, (unsigned char)'\0');

    /* --- Member name component (optional) --- */
    mblen = 0;
    if (member != NULL) {
        mblen = strlen(member);
        if (mblen > MVS_MEMBER_MAX) mblen = MVS_MEMBER_MAX;
    }

    if (mblen > 0)
        hash = fnv1a_feed(hash, member, mblen);

    /*
     * Guarantee a non-zero result.  Some NFS client implementations
     * treat fileid == 0 as "inode not available".  If the hash
     * happens to collide with zero (astronomically unlikely but
     * theoretically possible), substitute the FNV offset basis,
     * which is itself non-zero by construction.
     */
    if (hash == (uint64_t)0)
        hash = make64(FNV_INIT_HI, FNV_INIT_LO);

    return hash;
}

/* ------------------------------------------------------------------ */
/* mvs_fid_ino32                                                      */
/*                                                                    */
/* Fold the 64-bit hash to 32 bits for use as vfs_stat_t.raw_ino.     */
/* XOR-folding preserves the avalanche property: a 1-bit change in    */
/* either input affects roughly half the output bits.                 */
/* ------------------------------------------------------------------ */
uint32_t mvs_fid_ino32(const char *dsname, const char *member)
{
    uint64_t h  = mvs_fid_hash(dsname, member);
    uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(h >> 32);
    uint32_t result = lo ^ hi;

    /* Same zero-avoidance as mvs_fid_hash */
    if (result == 0u) result = FNV_INIT_LO;

    return result;
}
