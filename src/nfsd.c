/*
 * nfsd.c - Minimal NFSv3 server entry point.
 *
 * Creates three TCP listening sockets (portmapper/111, mount/20048,
 * nfs/2049) and runs a single select() loop handling up to
 * MAX_CONNECTIONS concurrent clients.
 *
 * Port overrides via command-line flags allow testing without root:
 *   ./nfsd -p 11111 -m 12048 -n 12049 nfsd.conf
 *
 * On MVS: replace socket()/bind()/listen()/accept() with the
 * equivalents from the Hercules TCP/IP instruction interface.
 *
 * Usage:
 *   sudo ./nfsd [-p pmap_port] [-m mount_port] [-n nfs_port] <config>
 *
 * Standard mount:
 *   sudo mount -t nfs -o nfsvers=3,nolock server:/export/foo /mnt/foo
 * Explicit ports (skip portmapper):
 *   sudo mount -t nfs -o nfsvers=3,port=2049,mountport=20048,nolock \
 *        server:/export/foo /mnt/foo
 */

#ifdef __MVS__

#include <sockets.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "asmutils.h"

#else

#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#endif

#include "nfsd.h"
#include "logger.h"
#include "mvsfsz.h"
#include "mvsprf.h"
#include "mvspww.h"
#include "mvsfid.h"
#include "mvsutl.h"

/* ------------------------------------------------------------------- */
/* Minimal getopt for JCC/MVS (JCC C89 library has no getopt).         */
/*                                                                     */
/* Supports:                                                           */
/*   -x          option without argument                               */
/*   -x val      option with argument (next argv element)              */
/*   -xval       option with argument (attached, no space)             */
/*   --           end-of-options marker                                */
/*                                                                     */
/* Clustered flags (-xyz) are not supported; each flag must be its own */
/* -<char> token.  That is sufficient for nfsd's -p -m -n -v flags.    */
/*                                                                     */
/* optarg, optind, and optopt are declared static because they are     */
/* consumed only within this translation unit.  On Linux the real      */
/* POSIX symbols (from <unistd.h>) are used instead.                   */
/* ------------------------------------------------------------------- */
#ifdef __MVS__

static char *optarg = NULL; /* points to option argument, or NULL       */
static int   optind = 1;    /* index of next argv[] element to examine  */
static int   optopt = 0;    /* option char that triggered an error      */

static int getopt(int argc, char *argv[], const char *optstring)
{
    const char *p;
    const char *argrest;
    char        c;

    /* No more arguments, or next argument is not an option */
    if (optind >= argc || argv[optind] == NULL)   return -1;
    if (argv[optind][0] != '-')                   return -1;
    if (argv[optind][1] == '\0')                  return -1;

    /* "--" signals end of options */
    if (argv[optind][1] == '-' && argv[optind][2] == '\0') {
        optind++;
        return -1;
    }

    c       = argv[optind][1];          /* the option letter            */
    argrest = argv[optind] + 2;         /* anything after the letter    */
    optind++;

    /* Look the letter up in the option string */
    p = strchr(optstring, (int)(unsigned char)c);
    if (p == NULL) {
        logmsg_error("NFSDM010E", "unknown option -- %c", c);
        optopt = (int)(unsigned char)c;
        return (int)'?';
    }

    if (p[1] == ':') {
        /* This option requires an argument */
        if (*argrest != '\0') {
            /* Attached form: -p11111 */
            optarg = (char *)argrest;
        } else if (optind < argc && argv[optind] != NULL) {
            /* Separate form: -p 11111 */
            optarg = argv[optind];
            optind++;
        } else {
            logmsg_error("NFSDM020E", "option requires an argument -- %c", c);
            optopt = (int)(unsigned char)c;
            return (int)'?';
        }
    } else {
        optarg = NULL;
    }

    return (int)(unsigned char)c;
}

#endif /* __MVS__ */

/* -------------------------------------------------------------------- */
/* parse_port: parse a decimal port string into [1, 65535].             */
/* Returns 0 on success, -1 if the string is not a valid port number.   */
/* -------------------------------------------------------------------- */
static int parse_port(const char *s, int *out)
{
    char *end;
    long  v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
        return -1;
    *out = (int)v;
    return 0;
}

/* -------------------------------------------------------------------- */
/* Global write verifier: set once at startup from time() + pseudo-PID  */
/* -------------------------------------------------------------------- */
uint8_t g_write_verifier[8];
int     g_port_pmap  = PORT_PORTMAP;
int     g_port_mount = PORT_MOUNT;
int     g_port_nfs   = PORT_NFS;
int     g_verbose    = 0;

/* ------------------------------------------------------------------ */
/* Global I/O buffers shared across all connections (single-threaded) */
/* ------------------------------------------------------------------ */
static uint8_t g_recv_buf[BUF_SIZE];
static uint8_t g_send_buf[BUF_SIZE];

/* ------------------------------------------------------------------ */
/* Connection table                                                   */
/* ------------------------------------------------------------------ */
static conn_t g_conns[MAX_CONNECTIONS];
static int    g_nconns = 0;

/* -------------------------------------------------------------------- */
/* sock_close: close a SOCKET descriptor.                               */
/*                                                                      */
/* On the MVS TCP/IP interface a socket MUST be closed with             */
/* closesocket().  The C library's close() operates on MVS files and    */
/* does NOT close a socket: the connection stays open, no FIN is ever   */
/* sent to the peer, and the descriptor leaks.  The visible symptom is  */
/* a peer (e.g. the Linux NFS client) stuck in FIN_WAIT2 waiting for a  */
/* FIN that never arrives, while our side sits in CLOSE_WAIT -- and,    */
/* once MAX_CONNECTIONS descriptors have leaked, accept_conn() starts   */
/* rejecting new connections and the mount wedges.                      */
/*                                                                      */
/* On POSIX, close() is the correct call (there is no closesocket()).   */
/* -------------------------------------------------------------------- */
#ifdef __MVS__
#define sock_close(fd)   closesocket(fd)
#else
#define sock_close(fd)   close(fd)
#endif

/* -------------------------------------------------------------------- */
/* make_listen_sock: create a TCP listening socket on port.             */
/* SO_REUSEADDR lets the server restart without waiting for TIME_WAIT.  */
/* Calls exit() on failure.                                             */
/* -------------------------------------------------------------------- */
static int make_listen_sock(int port)
{
    int                fd;
    int                opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

#ifdef __MVS__
    /* Note that is known to fail under JCC library. */
    if (setsockopt(fd, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        logmsg_debug("NFSDM027D", "Failed to setsockopt SOREUSEADDR - %s",
            strerror(errno));
    }
#else
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        perror("nfsd: setsockopt SO_REUSEADDR");
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        logmsg_error("NFSDM030E", "NFSD bind port %d failed - %s",
            port, strerror(errno));
        exit(1);
    }
    if (listen(fd, 8) < 0) {
        logmsg_error("NFSDM031E", "NFSD listen on port %d failed - %s",
            port, strerror(errno));
        exit(1);
    }
    return fd;
}

/* -------------------------------------------------------------------- */
/* accept_conn: accept one connection and add it to the table.          */
/* -------------------------------------------------------------------- */
static void accept_conn(int lsock, int proto)
{
    struct sockaddr_in peer;
#ifdef __MVS__
    long int           plen = sizeof(peer);
#else
    socklen_t          plen = sizeof(peer);
#endif
    int                cfd;
#ifndef __MVS__
    int                flag;
#endif

    cfd = accept(lsock, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) return;

#ifndef __MVS__
    /* Disable Nagle's algorithm so small RPC replies are sent immediately.
     * Without this, Nagle + the client's TCP delayed ACK interact to add
     * ~40 ms of latency per RPC round-trip, which compounds across the
     * many sequential requests a directory listing generates.
     * On MVS: TCP_NODELAY is not available in the JCC socket headers;
     * rpc_send() sends the mark and body in one send_all() call instead. */
    flag = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif

    if (g_nconns >= MAX_CONNECTIONS) {
        logmsg_error("NFSDM040E", "NFSD connection table full (%d), dropping connection",
                MAX_CONNECTIONS);
        sock_close(cfd);
        return;
    }

    g_conns[g_nconns].fd    = cfd;
    g_conns[g_nconns].proto = proto;
    logmsg_trace("NFSDM045T", "NFSD accepted connection fd=%d (proto=%d/%s)",
                g_conns[g_nconns].fd,
                g_conns[g_nconns].proto,
                CONN_PROTO_TO_STR(g_conns[g_nconns].proto) );
    g_nconns++;
}

/* -------------------------------------------------------------------- */
/* handle_connection: receive one RPC, dispatch, send reply.            */
/* Returns 0 on success, -1 to close the connection.                    */
/* -------------------------------------------------------------------- */
static int handle_connection(conn_t *conn)
{
    uint32_t   msglen = 0;
    rpc_call_t call;
    xdr_t      in, out;

    if (rpc_recv(conn->fd, g_recv_buf, BUF_SIZE, &msglen) < 0) return -1;

    xdr_init_read (&in,  g_recv_buf, msglen);
    /* g_send_buf[0..3] is headroom for the RPC record mark written by
     * rpc_send(); the XDR response body starts at g_send_buf[4].      */
    xdr_init_write(&out, g_send_buf + 4, BUF_SIZE - 4);

    if (rpc_parse_call(&in, &call) < 0) return -1;

    switch (conn->proto) {
    case PROTO_PORTMAP: handle_portmap(conn->fd, &call, &in, &out); break;
    case PROTO_MOUNT:   handle_mount  (conn->fd, &call, &in, &out); break;
    case PROTO_NFS:     handle_nfs3   (conn->fd, &call, &in, &out); break;
    default:            break;
    }

    if (out.pos > 0) return rpc_send(conn->fd, g_send_buf, out.pos);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Setup write verifier                                               */
/* ------------------------------------------------------------------ */
static void set_write_verifier() {
    time_t         now;

    /* Write verifier: combine startup time + PID (a hashed JES2 job id on
     * MVS, getpid() elsewhere) to avoid collision on fast restarts (time()
     * has only 1-second granularity). */
#ifdef __MVS__

    /* Get jobid ... e.g. "STC01234".  May be NULL if the PSA->TCB->JSCB
        ->SSIB chain does not resolve. */
    char *job_id = get_jes2_jobid();

    /* Convert that to a pseudo-pid via hash.  mvs_fid_ino32() treats a
        NULL name as zero-length, so this is NULL-safe. */
    uint32_t pid = mvs_fid_ino32(job_id, NULL);

    logmsg_info("NFSDM050I", "NFSD running as %s ... using pseudo-pid 0x%08X",
                (job_id != NULL) ? job_id : "(unknown)", pid);

#else
    pid_t pid = getpid();
#endif
    now = time(NULL);
    g_write_verifier[0] = (uint8_t)((uint32_t)now >> 24);
    g_write_verifier[1] = (uint8_t)((uint32_t)now >> 16);
    g_write_verifier[2] = (uint8_t)((uint32_t)now >>  8);
    g_write_verifier[3] = (uint8_t)((uint32_t)now      );
    g_write_verifier[4] = (uint8_t)((uint32_t)pid >> 24);
    g_write_verifier[5] = (uint8_t)((uint32_t)pid >> 16);
    g_write_verifier[6] = (uint8_t)((uint32_t)pid >>  8);
    g_write_verifier[7] = (uint8_t)((uint32_t)pid      );
}

/* ------------------------------------------------------------------ */
/* Process operator command (MVS only at this time)                   */
/* ------------------------------------------------------------------ */
static int process_operator_command() {

#ifdef __MVS__
    char           modify_buf[128]; /* MODIFY command text (EBCDIC)        */
    int            modify_len;      /* bytes of command text returned      */
    int            cib_rc;          /* getcib return code                  */
    int            handle_rc;
    modify_len = 0;
    cib_rc = getcib(modify_buf, (size_t)(sizeof(modify_buf) - 1),
                    &modify_len);
    if (cib_rc == 2) {
        /* STOP (P) command received -- exit RC = 2 for shutdown */
        log_set_level(LOG_INFO);
        log_set_wto_level(LOG_INFO);
        logmsg_info("NFSDM060I", "MVS STOP command received, shutting down");
        return 2;
    } else if (cib_rc == 1) {
        /* MODIFY (F) command received.  Hand the operand text to the
         * logger, which claims "SET LOGLVL ..." commands; anything it
         * does not recognise (return 1) is reported as unhandled. */
        if (modify_len > 0) {
            modify_buf[modify_len] = '\0';

            handle_rc = log_handle_modify(modify_buf);
            if ( handle_rc <= 0 )
                /* Return if cmd recognized */
                return 0;

            handle_rc = mvsprf_handle_modify(modify_buf);
            if ( handle_rc == 1 ) {
                /* If cmd unrecognized then write out warning */
                logmsg_warn("NFSDM070W", "MVS MODIFY ignored (unrecognised): %s",
                            modify_buf);
                return 0;
            }

        } else {
            logmsg_error("NFSDM080E", "MVS MODIFY received (no data)");
        }
    }
#endif

    return 0;
}

/* ==================================================================== */
/* Server startup / event loop / shutdown                               */
/*                                                                      */
/* The phases of main(), each with the reasoning that justifies it, so  */
/* main() reads as the sequence it performs.                            */
/* ==================================================================== */

/* What the command line asked for. */
typedef struct {
    int         port_pmap;
    int         port_mount;
    int         port_nfs;
    const char *config_path;
} server_opts_t;

/* The three listening sockets, passed around as one thing. */
typedef struct {
    int pmap;
    int mount;
    int nfs;
} listeners_t;

/* Bring up every subsystem that must be ready before the first request. */
static void server_init_subsystems(void)
{
    log_set_level(LOG_DEBUG);
    log_set_timestamps(1);
    log_proc_init();       /* per-procedure log levels -> inherit global */

    logmsg_info("NFSDM090I", "NFSD Starting up");

    dir_openlist_init();
    mvs_rcache_init();
    pww_init();
    mvsfsz_init();
    mvsprf_init();

    /* Cache the MVS local-time offset (CVTLDTO) once at startup: used to
       convert ISPF member stats (stored in local time) to/from UTC epoch.
       time()/gettimeofday() already return UTC, so they need no correction. */
    mvs_tz_init();
    /* Reported as +/-HH:MM rather than raw seconds: several real zones sit
       on a quarter or half hour (+05:45 Nepal, -03:30 Newfoundland), and a
       seconds figure makes those hard to check at a glance.

       The sign is taken off BEFORE the arithmetic.  mvs_tz_offset() is
       negative west of GMT, and C89 leaves the rounding direction of
       division and remainder with a negative operand implementation
       defined -- so -21600 could yield either -6:0 or -5:-3600. */
    {
        int  tz   = mvs_tz_offset();
        int  mag  = (tz < 0) ? -tz : tz;
        char sign = (tz < 0) ? '-' : '+';

        logmsg_info("NFSDM100I",
                    "NFSD MVS local-time offset = %c%02d:%02d (HH:MM)",
                    sign, mag / 3600, (mag % 3600) / 60);
    }
}

/* Parse argv into *opts, which arrives holding the defaults.
   Returns 0 on success, or the non-zero EXIT CODE main() should return
   (the distinct codes 101-105 identify which argument was at fault). */
static int parse_args(int argc, char *argv[], server_opts_t *opts)
{
    int opt;

    while ((opt = getopt(argc, argv, "p:m:n:v")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_port(optarg, &opts->port_pmap)  < 0) {
                logmsg_error("NFSDM110E", "Invalid port-mapper port: %s", optarg);
                return 101;
            }
            break;
        case 'm':
            if (parse_port(optarg, &opts->port_mount) < 0) {
                logmsg_error("NFSDM120E", "Invalid mount port: %s", optarg);
                return 102;
            }
            break;
        case 'n':
            if (parse_port(optarg, &opts->port_nfs)   < 0) {
                logmsg_error("NFSDM130E", "Invalid NFS port: %s", optarg);
                return 103;
            }
            break;
        case 'v': g_verbose  = 1;            break;
        default:
            logmsg_error("NFSDM140E", "usage: %s [-p pmap] [-m mount] [-n nfs] <config>",
                argv[0]);
            return 104;
        }
    }

    if (optind >= argc) {
        logmsg_error("NFSDM150E", "usage: %s [-p pmap] [-m mount] [-n nfs] <config>",
            argv[0]);
        return 105;
    }
    opts->config_path = argv[optind];
    return 0;
}

/* Create the three listening sockets and publish the ports we are actually
   serving on, which is what the portmapper hands back to clients.
   make_listen_sock() exits on failure, so there is no error path here. */
static void open_listeners(const server_opts_t *opts, listeners_t *lsn)
{
    g_port_pmap  = opts->port_pmap;
    g_port_mount = opts->port_mount;
    g_port_nfs   = opts->port_nfs;

    lsn->pmap  = make_listen_sock(opts->port_pmap);
    lsn->mount = make_listen_sock(opts->port_mount);
    lsn->nfs   = make_listen_sock(opts->port_nfs);

    logmsg_info("NFSDM160I", "Listening on ports portmapper=%d mount=%d nfs=%d",
        opts->port_pmap, opts->port_mount, opts->port_nfs);
}

/* Fatal abend in the write path? (design_nfs_write.md Sec 7.3)
   The flush traps out-of-space abends and keeps serving, but anything else is
   a probable program error: it has been reported loudly and we now shut down
   rather than carry on from state we no longer trust.  Ending the task also
   lets MVS reclaim any allocation or SPFEDIT enqueue the cleanup could not
   release.  Returns 1 when the loop should exit. */
static int write_path_is_fatal(void)
{
    if (!pww_fatal_abend())
        return 0;
    logmsg_error("NFSDM170E", "Unrecoverable ABEND in the write path -- shutting down");
    return 1;
}

/* Build the read set: the three listeners plus every live connection.
   *maxfd_out receives the highest descriptor, for select(). */
static void build_read_set(const listeners_t *lsn, fd_set *rfds, int *maxfd_out)
{
    int maxfd;
    int i;

    FD_ZERO(rfds);
    FD_SET(lsn->pmap,  rfds);
    FD_SET(lsn->mount, rfds);
    FD_SET(lsn->nfs,   rfds);

    maxfd = lsn->pmap;
    if (lsn->mount > maxfd) maxfd = lsn->mount;
    if (lsn->nfs   > maxfd) maxfd = lsn->nfs;

    for (i = 0; i < g_nconns; i++) {
        FD_SET(g_conns[i].fd, rfds);
        if (g_conns[i].fd > maxfd) maxfd = g_conns[i].fd;
    }
    *maxfd_out = maxfd;
}

/* Accept anything new on the listeners, then service every connection that
   select() marked readable.  A connection whose handler fails is closed and
   removed from the table. */
static void service_ready(const listeners_t *lsn, fd_set *rfds)
{
    int i;

    if (FD_ISSET(lsn->pmap,  rfds)) accept_conn(lsn->pmap,  PROTO_PORTMAP);
    if (FD_ISSET(lsn->mount, rfds)) accept_conn(lsn->mount, PROTO_MOUNT);
    if (FD_ISSET(lsn->nfs,   rfds)) accept_conn(lsn->nfs,   PROTO_NFS);

    for (i = 0; i < g_nconns; ) {
        if (FD_ISSET(g_conns[i].fd, rfds)) {
            if (handle_connection(&g_conns[i]) < 0) {
                /* Peer closed (or the RPC failed): close OUR half so a FIN
                   goes back and the descriptor is released.  Must be
                   sock_close() -- see its comment. */
                logmsg_trace("NFSDM180T", "NFSD closing connection fd=%d (proto=%d/%s)",
                         g_conns[i].fd, g_conns[i].proto,
                         CONN_PROTO_TO_STR(g_conns[i].proto) );
                sock_close(g_conns[i].fd);
                g_conns[i] = g_conns[--g_nconns];
                continue;   /* the slot now holds a DIFFERENT connection */
            }
        }
        i++;
    }
}

/* Flush what is still buffered, then close everything down in the order that
   leaves no peer hanging: client connections first (each gets a FIN), then
   the listeners. */
static void server_shutdown(const listeners_t *lsn)
{
    int i;

    pww_flush_all();

    logmsg_info("NFSDM190I", "Closing sockets");

    for (i = 0; i < g_nconns; i++)
        sock_close(g_conns[i].fd);
    g_nconns = 0;

    sock_close(lsn->pmap);
    sock_close(lsn->mount);
    sock_close(lsn->nfs);

    mvsprf_dump();
    logmsg_info("NFSDM200I", "Shutting down");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    server_opts_t  opts;
    listeners_t    lsn;
    int            maxfd, n, rc;
    fd_set         rfds;
    struct timeval tv;      /* select() timeout -- re-set each iteration  */

    server_init_subsystems();

    opts.port_pmap   = PORT_PORTMAP;
    opts.port_mount  = PORT_MOUNT;
    opts.port_nfs    = PORT_NFS;
    opts.config_path = NULL;

    rc = parse_args(argc, argv, &opts);
    if (rc != 0)
        return rc;

    /* Load export configuration */
    n = exports_load(opts.config_path);
    if (n < 0) {
        logmsg_error("NFSDM210E", "Cannot open config: %s", opts.config_path);
        return 106;
    }
    /* Report the DATASET total too, not just the export count: several
       datasets normally share one export path, so the export count alone
       does not tell an operator whether everything they configured was
       actually accepted.  A dataset dropped by cfg_load_dscb_info() (a bad
       DSCB, wrong DSORG/RECFM) takes its whole export with it, and that is
       far easier to spot against an expected dataset count. */
    {
        int exp_idx;
        int ds_total = 0;

        for (exp_idx = 0; exp_idx < n; exp_idx++)
            ds_total += export_dataset_count(exp_idx);

        logmsg_info("NFSDM220I", "Loaded %d export(s), %d dataset(s) from %s",
                    n, ds_total, opts.config_path);
    }

    /* File handles are self-describing -- no handle cache to initialise. */

    set_write_verifier();

#ifndef __MVS__
    signal(SIGPIPE, SIG_IGN);
#endif

    open_listeners(&opts, &lsn);

    /* ---- Main select() event loop ---- */
    for (;;) {

        if (process_operator_command() == 2)
            break;

        if (write_path_is_fatal())
            break;

        build_read_set(&lsn, &rfds, &maxfd);

        /* ------ Wait for activity (1s timeout to work poll loop) ------ */
        /* Note that the JCC select does not timeout for subsecond values */
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
#ifndef __MVS__
            if (errno == EINTR) continue;
#endif
            perror("select");
            break;
        }

        /* Flush any buffered PDS-member writes that have gone idle.  Runs on
           every wake-up (activity or the 1s timeout), so flush latency is
           bounded to around 1 seconds. */
        pww_flush_idle(time(NULL));

        if (n == 0) continue;  /* timeout -- go back and check for STOP  */

        service_ready(&lsn, &rfds);
    }

    server_shutdown(&lsn);
    return 0;
}
