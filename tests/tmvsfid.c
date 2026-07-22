/*
 * tests/tmvsfid.c - Unit tests for mvsfid.c (FNV-1a fileid hashing).
 *
 * Suite prefix: /mvsfid
 * Sub-suites  : /hash /ino32
 *
 * mvs_fid_hash() / mvs_fid_ino32() back every NFS fileid and cache key.  They
 * are pure (FNV-1a, only strlen), so they run identically on the dev host and
 * MVS -- with one caveat that shapes every test here:
 *
 *   The hash is byte-wise, so identical *bytes* hash identically on any host,
 *   but a string LITERAL is ASCII on the dev host and EBCDIC under JCC, so the
 *   same literal hashes to DIFFERENT values on the two platforms.  Therefore no
 *   test asserts an absolute hash value -- every check is RELATIONAL (two calls
 *   compared to each other), which holds on both platforms.
 *
 * What is pinned: determinism, non-zero output, distinctness across dsname and
 * across member, NULL vs "" equivalence, the domain-separation invariant (the
 * NUL between the two components, so no split of the same characters collides),
 * the length clamps (MVS_DSNAME_MAX / MVS_MEMBER_MAX), and the ino32 XOR-fold.
 *
 * munit note: full-width values are compared with u64_eq / u32_eq (munit has no
 * uint assertion and narrows to int); see txdr.c for the rationale.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include "munit.h"
#include "mvsfid.h"   /* mvs_fid_hash / mvs_fid_ino32 (+ MVS aliases, limits) */

static int u64_eq(uint64_t a, uint64_t b) { return a == b ? 1 : 0; }
static int u32_eq(uint32_t a, uint32_t b) { return a == b ? 1 : 0; }

/* ==================================================================== */
/* /hash -- mvs_fid_hash                                                */
/* ==================================================================== */

/* Same inputs always produce the same hash. */
static MunitResult test_hash_deterministic(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(u64_eq(mvs_fid_hash("SYS1.PARMLIB", "IEASYS00"),
                            mvs_fid_hash("SYS1.PARMLIB", "IEASYS00")), ==, 1);
    munit_assert_int(u64_eq(mvs_fid_hash("SYS1.PARMLIB", NULL),
                            mvs_fid_hash("SYS1.PARMLIB", NULL)), ==, 1);
    return MUNIT_OK;
}

/* The result is never zero (NFS clients treat fileid 0 as "unavailable"). */
static MunitResult test_hash_nonzero(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(u64_eq(mvs_fid_hash("A", "B"), (uint64_t)0), ==, 0);
    munit_assert_int(u64_eq(mvs_fid_hash("SYS1.PARMLIB", NULL), (uint64_t)0), ==, 0);
    munit_assert_int(u64_eq(mvs_fid_hash("", NULL), (uint64_t)0), ==, 0);
    return MUNIT_OK;
}

/* Different dataset names hash differently. */
static MunitResult test_hash_distinct_dsname(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(u64_eq(mvs_fid_hash("SYS1.PARMLIB", NULL),
                            mvs_fid_hash("SYS1.PROCLIB", NULL)), ==, 0);
    return MUNIT_OK;
}

/* Same dataset, different members hash differently. */
static MunitResult test_hash_distinct_member(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(u64_eq(mvs_fid_hash("SYS1.PARMLIB", "MEMA"),
                            mvs_fid_hash("SYS1.PARMLIB", "MEMB")), ==, 0);
    return MUNIT_OK;
}

/* NULL and "" both mean "no member" (identical hash); a real member differs. */
static MunitResult test_hash_member_vs_none(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(u64_eq(mvs_fid_hash("SYS1.PARMLIB", NULL),
                            mvs_fid_hash("SYS1.PARMLIB", "")), ==, 1);
    munit_assert_int(u64_eq(mvs_fid_hash("SYS1.PARMLIB", NULL),
                            mvs_fid_hash("SYS1.PARMLIB", "M")), ==, 0);
    return MUNIT_OK;
}

/* Domain separation: the NUL between components means the SAME characters split
   differently across (dsname, member) cannot collide.  "AB"+"C", "A"+"BC" and
   "ABC"+none all differ. */
static MunitResult test_hash_domain_separation(const MunitParameter params[], void *data)
{
    uint64_t h_ab_c;
    uint64_t h_a_bc;
    uint64_t h_abc;
    (void)params; (void)data;

    h_ab_c = mvs_fid_hash("AB",  "C");
    h_a_bc = mvs_fid_hash("A",   "BC");
    h_abc  = mvs_fid_hash("ABC", NULL);

    munit_assert_int(u64_eq(h_ab_c, h_a_bc), ==, 0);
    munit_assert_int(u64_eq(h_ab_c, h_abc),  ==, 0);
    munit_assert_int(u64_eq(h_a_bc, h_abc),  ==, 0);
    return MUNIT_OK;
}

/* Inputs past the length limits are clamped, so trailing characters beyond
   MVS_DSNAME_MAX (44) / MVS_MEMBER_MAX (8) do not change the hash. */
static MunitResult test_hash_truncation(const MunitParameter params[], void *data)
{
    char ds_max[64];
    char ds_over[64];
    int  i;
    (void)params; (void)data;

    for (i = 0; i < MVS_DSNAME_MAX; i++) {
        ds_max[i]  = (char)'A';
        ds_over[i] = (char)'A';
    }
    ds_max[MVS_DSNAME_MAX] = '\0';
    for (i = MVS_DSNAME_MAX; i < MVS_DSNAME_MAX + 6; i++)
        ds_over[i] = (char)'Z';                 /* extra, must be ignored */
    ds_over[MVS_DSNAME_MAX + 6] = '\0';

    munit_assert_int(u64_eq(mvs_fid_hash(ds_max,  NULL),
                            mvs_fid_hash(ds_over, NULL)), ==, 1);

    /* Member clamp at 8: "MEMBER12" == "MEMBER12XY" (tail dropped). */
    munit_assert_int(u64_eq(mvs_fid_hash("DS", "MEMBER12"),
                            mvs_fid_hash("DS", "MEMBER12XY")), ==, 1);
    return MUNIT_OK;
}

static MunitTest hash_tests[] = {
    { "/deterministic",    test_hash_deterministic,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nonzero",          test_hash_nonzero,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/distinct_dsname",  test_hash_distinct_dsname,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/distinct_member",  test_hash_distinct_member,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/member_vs_none",   test_hash_member_vs_none,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/domain_separation", test_hash_domain_separation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/truncation",       test_hash_truncation,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /ino32 -- mvs_fid_ino32 (XOR-fold of the 64-bit hash)               */
/* ==================================================================== */

static MunitResult test_ino32_deterministic(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(u32_eq(mvs_fid_ino32("SYS1.PARMLIB", "IEASYS00"),
                            mvs_fid_ino32("SYS1.PARMLIB", "IEASYS00")), ==, 1);
    return MUNIT_OK;
}

static MunitResult test_ino32_nonzero(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(u32_eq(mvs_fid_ino32("SYS1.PARMLIB", "IEASYS00"), 0u), ==, 0);
    munit_assert_int(u32_eq(mvs_fid_ino32("A", NULL), 0u), ==, 0);
    return MUNIT_OK;
}

/* ino32 is the XOR of the 64-bit hash's high and low words. */
static MunitResult test_ino32_matches_fold(const MunitParameter params[], void *data)
{
    uint64_t h;
    uint32_t lo;
    uint32_t hi;
    uint32_t fold;
    (void)params; (void)data;

    h    = mvs_fid_hash("SYS1.PARMLIB", "IEASYS00");
    lo   = (uint32_t)(h & 0xFFFFFFFFu);
    hi   = (uint32_t)(h >> 32);
    fold = lo ^ hi;    /* non-zero for this input, so no zero-substitution */

    munit_assert_int(u32_eq(mvs_fid_ino32("SYS1.PARMLIB", "IEASYS00"), fold), ==, 1);
    return MUNIT_OK;
}

/* Distinct inputs fold to distinct 32-bit keys (for these chosen names). */
static MunitResult test_ino32_distinct(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    munit_assert_int(u32_eq(mvs_fid_ino32("SYS1.PARMLIB", NULL),
                            mvs_fid_ino32("SYS1.PROCLIB", NULL)), ==, 0);
    munit_assert_int(u32_eq(mvs_fid_ino32("DS", "MEMA"),
                            mvs_fid_ino32("DS", "MEMB")), ==, 0);
    return MUNIT_OK;
}

static MunitTest ino32_tests[] = {
    { "/deterministic", test_ino32_deterministic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nonzero",       test_ino32_nonzero,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/matches_fold",  test_ino32_matches_fold,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/distinct",      test_ino32_distinct,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/hash",  hash_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/ino32", ino32_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite tmvsfid_suite = {
    "/mvsfid",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
