/*
 * tests/trpc.c - Unit tests for rpc.c (ONC RPC message header parse & build).
 *
 * Suite prefix: /rpc
 * Sub-suites  : /parse /accept_hdr /reject
 *
 * Scope: the PURE, buffer-only functions -- rpc_parse_call() and the reply
 * header writers (rpc_write_accept_hdr / _prog_mismatch / _proc_unavail).
 * These operate entirely on an xdr_t over an in-memory buffer, so they run
 * identically on the dev host and MVS.
 *
 * NOT tested here: rpc_recv() / rpc_send() -- they are TCP record-marking I/O
 * over recv()/send() and need a live socket (or a socket mock that would clash
 * with libc on the dev host), so they are left to integration testing.  Linking
 * rpc.o still pulls recv()/send() in; those resolve from the JCC C runtime on
 * MVS (exactly as the server build does) and from libc on the dev host.
 *
 * The CALL messages the /parse tests decode are built with the xdr writers, so
 * this suite also leans on xdr.c being correct (see txdr.c).
 *
 * munit note: full-width uint32 values are compared via u32_eq (see txdr.c for
 * the rationale); byte-level checks use munit_assert_int directly.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>

#include "munit.h"
#include "nfsd.h"     /* xdr_t, rpc_call_t, rpc_* prototypes, RPC_* constants */

static int u32_eq(uint32_t a, uint32_t b) { return a == b ? 1 : 0; }

/* Write the fixed CALL header words (xid, msg_type, rpcvers, prog, vers, proc)
   with a VALID msg_type (CALL) and rpcvers (2). */
static void put_call_hdr(xdr_t *xw, uint32_t xid, uint32_t prog,
                         uint32_t vers, uint32_t proc)
{
    xdr_write_uint32(xw, xid);
    xdr_write_uint32(xw, RPC_CALL);
    xdr_write_uint32(xw, 2u);              /* rpcvers */
    xdr_write_uint32(xw, prog);
    xdr_write_uint32(xw, vers);
    xdr_write_uint32(xw, proc);
}

/* Append an empty (AUTH_NULL) verifier -- always the trailing two words. */
static void put_null_verf(xdr_t *xw)
{
    xdr_write_uint32(xw, AUTH_FLAVOR_NULL);
    xdr_write_uint32(xw, 0u);
}

/* ==================================================================== */
/* /parse -- rpc_parse_call                                             */
/* ==================================================================== */

/* AUTH_NULL credential: uid/gid default to 0. */
static MunitResult test_parse_auth_null(const MunitParameter params[], void *data)
{
    uint8_t    buf[128];
    xdr_t      xw, xr;
    rpc_call_t call;
    (void)params; (void)data;

    memset(buf, 0, sizeof(buf));
    xdr_init_write(&xw, buf, sizeof(buf));
    put_call_hdr(&xw, 0xCAFEBABEu, 100003u, 3u, 1u);   /* NFS v3, GETATTR */
    xdr_write_uint32(&xw, AUTH_FLAVOR_NULL);            /* cred flavor    */
    xdr_write_uint32(&xw, 0u);                          /* cred length    */
    put_null_verf(&xw);
    munit_assert_int(xw.error, ==, 0);

    xdr_init_read(&xr, buf, xw.pos);
    munit_assert_int(rpc_parse_call(&xr, &call), ==, 0);
    munit_assert_int(u32_eq(call.xid,  0xCAFEBABEu), ==, 1);
    munit_assert_int(u32_eq(call.prog, 100003u), ==, 1);
    munit_assert_int(u32_eq(call.vers, 3u), ==, 1);
    munit_assert_int(u32_eq(call.proc, 1u), ==, 1);
    munit_assert_int(u32_eq(call.auth_uid, 0u), ==, 1);
    munit_assert_int(u32_eq(call.auth_gid, 0u), ==, 1);
    return MUNIT_OK;
}

/* AUTH_UNIX credential: uid/gid are extracted; machinename is skipped.
   cred body = stamp(4) + machinename(mnlen 4 + "host" 4) + uid(4) + gid(4)
             + ngids(4) = 24 bytes. */
static MunitResult test_parse_auth_unix(const MunitParameter params[], void *data)
{
    uint8_t    buf[128];
    xdr_t      xw, xr;
    rpc_call_t call;
    (void)params; (void)data;

    memset(buf, 0, sizeof(buf));
    xdr_init_write(&xw, buf, sizeof(buf));
    put_call_hdr(&xw, 1u, 100003u, 3u, 4u);
    xdr_write_uint32(&xw, AUTH_FLAVOR_UNIX);            /* cred flavor    */
    xdr_write_uint32(&xw, 24u);                         /* cred length    */
    xdr_write_uint32(&xw, 0u);                          /* stamp          */
    xdr_write_string(&xw, "host", 4);                   /* machinename    */
    xdr_write_uint32(&xw, 1234u);                       /* uid            */
    xdr_write_uint32(&xw, 5678u);                       /* gid            */
    xdr_write_uint32(&xw, 0u);                          /* ngids          */
    put_null_verf(&xw);
    munit_assert_int(xw.error, ==, 0);

    xdr_init_read(&xr, buf, xw.pos);
    munit_assert_int(rpc_parse_call(&xr, &call), ==, 0);
    munit_assert_int(u32_eq(call.auth_uid, 1234u), ==, 1);
    munit_assert_int(u32_eq(call.auth_gid, 5678u), ==, 1);
    return MUNIT_OK;
}

/* AUTH_UNIX with extra group ids: the ngids array is skipped and pos is
   realigned to the credential body end, so parsing still succeeds. */
static MunitResult test_parse_auth_unix_gids(const MunitParameter params[], void *data)
{
    uint8_t    buf[128];
    xdr_t      xw, xr;
    rpc_call_t call;
    (void)params; (void)data;

    memset(buf, 0, sizeof(buf));
    xdr_init_write(&xw, buf, sizeof(buf));
    put_call_hdr(&xw, 7u, 100003u, 3u, 1u);
    xdr_write_uint32(&xw, AUTH_FLAVOR_UNIX);
    xdr_write_uint32(&xw, 32u);            /* 24 + two extra gids (8) */
    xdr_write_uint32(&xw, 0u);             /* stamp        */
    xdr_write_string(&xw, "host", 4);      /* machinename  */
    xdr_write_uint32(&xw, 42u);            /* uid          */
    xdr_write_uint32(&xw, 99u);            /* gid          */
    xdr_write_uint32(&xw, 2u);             /* ngids = 2    */
    xdr_write_uint32(&xw, 11u);            /* gid[0]       */
    xdr_write_uint32(&xw, 22u);            /* gid[1]       */
    put_null_verf(&xw);

    xdr_init_read(&xr, buf, xw.pos);
    munit_assert_int(rpc_parse_call(&xr, &call), ==, 0);
    munit_assert_int(u32_eq(call.auth_uid, 42u), ==, 1);
    munit_assert_int(u32_eq(call.auth_gid, 99u), ==, 1);
    return MUNIT_OK;
}

/* A reply msg_type (not CALL) is rejected. */
static MunitResult test_parse_wrong_msgtype(const MunitParameter params[], void *data)
{
    uint8_t    buf[128];
    xdr_t      xw, xr;
    rpc_call_t call;
    (void)params; (void)data;

    xdr_init_write(&xw, buf, sizeof(buf));
    xdr_write_uint32(&xw, 1u);             /* xid       */
    xdr_write_uint32(&xw, RPC_REPLY);      /* NOT a CALL */
    xdr_write_uint32(&xw, 2u);
    xdr_write_uint32(&xw, 100003u);
    xdr_write_uint32(&xw, 3u);
    xdr_write_uint32(&xw, 1u);
    put_null_verf(&xw);

    xdr_init_read(&xr, buf, xw.pos);
    munit_assert_int(rpc_parse_call(&xr, &call), ==, -1);
    return MUNIT_OK;
}

/* An rpcvers other than 2 is rejected. */
static MunitResult test_parse_wrong_rpcvers(const MunitParameter params[], void *data)
{
    uint8_t    buf[128];
    xdr_t      xw, xr;
    rpc_call_t call;
    (void)params; (void)data;

    xdr_init_write(&xw, buf, sizeof(buf));
    xdr_write_uint32(&xw, 1u);
    xdr_write_uint32(&xw, RPC_CALL);
    xdr_write_uint32(&xw, 3u);             /* rpcvers 3 -- unsupported */
    xdr_write_uint32(&xw, 100003u);
    xdr_write_uint32(&xw, 3u);
    xdr_write_uint32(&xw, 1u);
    put_null_verf(&xw);

    xdr_init_read(&xr, buf, xw.pos);
    munit_assert_int(rpc_parse_call(&xr, &call), ==, -1);
    return MUNIT_OK;
}

/* A message that ends mid-header sets the xdr error and is rejected. */
static MunitResult test_parse_truncated(const MunitParameter params[], void *data)
{
    uint8_t    buf[128];
    xdr_t      xw, xr;
    rpc_call_t call;
    (void)params; (void)data;

    xdr_init_write(&xw, buf, sizeof(buf));
    put_call_hdr(&xw, 1u, 100003u, 3u, 1u);

    /* Only expose the first 8 bytes (xid + msg_type): the vers/prog reads
       run off the end, set x->error, and parsing fails. */
    xdr_init_read(&xr, buf, 8u);
    munit_assert_int(rpc_parse_call(&xr, &call), ==, -1);
    munit_assert_int(xr.error, ==, 1);
    return MUNIT_OK;
}

static MunitTest parse_tests[] = {
    { "/auth_null",     test_parse_auth_null,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/auth_unix",     test_parse_auth_unix,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/auth_unix_gids", test_parse_auth_unix_gids, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/wrong_msgtype", test_parse_wrong_msgtype,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/wrong_rpcvers", test_parse_wrong_rpcvers,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/truncated",     test_parse_truncated,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /accept_hdr -- rpc_write_accept_hdr                                  */
/* ==================================================================== */

static MunitResult test_accept_hdr(const MunitParameter params[], void *data)
{
    uint8_t buf[64];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    rpc_write_accept_hdr(&x, 0x12345678u, RPC_SUCCESS);
    munit_assert_int(x.error,    ==, 0);
    munit_assert_int((int)x.pos, ==, 24);          /* six 4-byte words */

    xdr_init_read(&x, buf, 24u);
    munit_assert_int(u32_eq(xdr_read_uint32(&x), 0x12345678u),      ==, 1); /* xid    */
    munit_assert_int(u32_eq(xdr_read_uint32(&x), RPC_REPLY),        ==, 1);
    munit_assert_int(u32_eq(xdr_read_uint32(&x), MSG_ACCEPTED),     ==, 1);
    munit_assert_int(u32_eq(xdr_read_uint32(&x), AUTH_FLAVOR_NULL), ==, 1); /* verf flavor */
    munit_assert_int(u32_eq(xdr_read_uint32(&x), 0u),              ==, 1); /* verf len    */
    munit_assert_int(u32_eq(xdr_read_uint32(&x), RPC_SUCCESS),     ==, 1); /* accept stat */
    return MUNIT_OK;
}

static MunitTest accept_tests[] = {
    { "/basic", test_accept_hdr, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* /reject -- rpc_write_prog_mismatch / rpc_write_proc_unavail          */
/* ==================================================================== */

static MunitResult test_prog_mismatch(const MunitParameter params[], void *data)
{
    uint8_t buf[64];
    xdr_t   x;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    rpc_write_prog_mismatch(&x, 0xAAAAAAAAu, 3u, 3u);
    munit_assert_int((int)x.pos, ==, 32);          /* header (24) + lo + hi */

    xdr_init_read(&x, buf, 32u);
    munit_assert_int(u32_eq(xdr_read_uint32(&x), 0xAAAAAAAAu),  ==, 1); /* xid  */
    munit_assert_int(u32_eq(xdr_read_uint32(&x), RPC_REPLY),    ==, 1);
    munit_assert_int(u32_eq(xdr_read_uint32(&x), MSG_ACCEPTED), ==, 1);
    (void)xdr_read_uint32(&x);                      /* verf flavor */
    (void)xdr_read_uint32(&x);                      /* verf len    */
    munit_assert_int(u32_eq(xdr_read_uint32(&x), PROG_MISMATCH), ==, 1);
    munit_assert_int(u32_eq(xdr_read_uint32(&x), 3u), ==, 1);  /* lo */
    munit_assert_int(u32_eq(xdr_read_uint32(&x), 3u), ==, 1);  /* hi */
    return MUNIT_OK;
}

static MunitResult test_proc_unavail(const MunitParameter params[], void *data)
{
    uint8_t  buf[64];
    xdr_t    x;
    uint32_t stat;
    int      i;
    (void)params; (void)data;

    xdr_init_write(&x, buf, sizeof(buf));
    rpc_write_proc_unavail(&x, 0x55u);
    munit_assert_int((int)x.pos, ==, 24);          /* just the accept header */

    xdr_init_read(&x, buf, 24u);
    for (i = 0; i < 5; i++)
        (void)xdr_read_uint32(&x);                  /* xid..verf */
    stat = xdr_read_uint32(&x);                     /* accept stat */
    munit_assert_int(u32_eq(stat, PROC_UNAVAIL), ==, 1);
    return MUNIT_OK;
}

static MunitTest reject_tests[] = {
    { "/prog_mismatch", test_prog_mismatch, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/proc_unavail",  test_proc_unavail,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

/* ==================================================================== */
/* Suite registration                                                    */
/* ==================================================================== */

static MunitSuite sub_suites[] = {
    { "/parse",       parse_tests,  NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/accept_hdr",  accept_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { "/reject",      reject_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
    { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

MunitSuite trpc_suite = {
    "/rpc",
    NULL,
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
