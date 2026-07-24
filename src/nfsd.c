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

/* ------------------------------------------------------------------ */
/* Minimal getopt for JCC/MVS (JCC C89 library has no getopt).         */
/*                                                                      */
/* Supports:                                                            */
/*   -x          option without argument                                */
/*   -x val      option with argument (next argv element)              */
/*   -xval       option with argument (attached, no space)             */
/*   --           end-of-options marker                                 */
/*                                                                      */
/* Clustered flags (-xyz) are not supported; each flag must be its own  */
/* -<char> token.  That is sufficient for nfsd's -p -m -n -v flags.    */
/*                                                                      */
/* optarg, optind, and optopt are declared static because they are      */
/* consumed only within this translation unit.  On Linux the real       */
/* POSIX symbols (from <unistd.h>) are used instead.                   */
/* ------------------------------------------------------------------ */
#ifdef __MVS__

static char *optarg = NULL; /* points to option argument, or NULL      */
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
        log_error("unknown option -- %c", c);
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
            log_error("option requires an argument -- %c", c);
            optopt = (int)(unsigned char)c;
            return (int)'?';
        }
    } else {
        optarg = NULL;
    }

    return (int)(unsigned char)c;
}

#endif /* __MVS__ */

/* ------------------------------------------------------------------ */
/* parse_port: parse a decimal port string into [1, 65535].             */
/* Returns 0 on success, -1 if the string is not a valid port number.   */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* Global write verifier: set once at startup from time() + pseudo-PID   */
/* ------------------------------------------------------------------ */
uint8_t g_write_verifier[8];
int     g_port_pmap  = PORT_PORTMAP;
int     g_port_mount = PORT_MOUNT;
int     g_port_nfs   = PORT_NFS;
int     g_verbose    = 0;

/* ------------------------------------------------------------------ */
/* Global I/O buffers shared across all connections (single-threaded)   */
/* ------------------------------------------------------------------ */
static uint8_t g_recv_buf[BUF_SIZE];
static uint8_t g_send_buf[BUF_SIZE];

/* ------------------------------------------------------------------ */
/* Connection table                                                     */
/* ------------------------------------------------------------------ */
static conn_t g_conns[MAX_CONNECTIONS];
static int    g_nconns = 0;

/* ------------------------------------------------------------------ */
/* sock_close: close a SOCKET descriptor.                               */
/*                                                                      */
/* On the MVS TCP/IP interface a socket MUST be closed with            */
/* closesocket().  The C library's close() operates on MVS files and   */
/* does NOT close a socket: the connection stays open, no FIN is ever   */
/* sent to the peer, and the descriptor leaks.  The visible symptom is  */
/* a peer (e.g. the Linux NFS client) stuck in FIN_WAIT2 waiting for a  */
/* FIN that never arrives, while our side sits in CLOSE_WAIT -- and,    */
/* once MAX_CONNECTIONS descriptors have leaked, accept_conn() starts   */
/* rejecting new connections and the mount wedges.                      */
/*                                                                      */
/* On POSIX, close() is the correct call (there is no closesocket()).   */
/* ------------------------------------------------------------------ */
#ifdef __MVS__
#define sock_close(fd)   closesocket(fd)
#else
#define sock_close(fd)   close(fd)
#endif

/* ------------------------------------------------------------------ */
/* make_listen_sock: create a TCP listening socket on port.             */
/* SO_REUSEADDR lets the server restart without waiting for TIME_WAIT.  */
/* Calls exit() on failure.                                             */
/* ------------------------------------------------------------------ */
static int make_listen_sock(int port)
{
    int                fd;
    int                opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

#ifdef __MVS__
    if (setsockopt(fd, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        perror("nfsd: setsockopt SO_REUSEADDR");
#else
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        perror("nfsd: setsockopt SO_REUSEADDR");
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("nfsd: bind port %d: %s", port, strerror(errno));
        exit(1);
    }
    if (listen(fd, 8) < 0) { perror("listen"); exit(1); }
    return fd;
}

/* ------------------------------------------------------------------ */
/* accept_conn: accept one connection and add it to the table.          */
/* ------------------------------------------------------------------ */
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
        log_error("nfsd: connection table full (%d), dropping",
                MAX_CONNECTIONS);
        sock_close(cfd);
        return;
    }

    g_conns[g_nconns].fd    = cfd;
    g_conns[g_nconns].proto = proto;
    g_nconns++;
}

/* ------------------------------------------------------------------ */
/* handle_connection: receive one RPC, dispatch, send reply.            */
/* Returns 0 on success, -1 to close the connection.                   */
/* ------------------------------------------------------------------ */
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
/* main                                                                  */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int            pmap_sock, mount_sock, nfs_sock;
    int            port_pmap  = PORT_PORTMAP;
    int            port_mount = PORT_MOUNT;
    int            port_nfs   = PORT_NFS;
    int            maxfd, n, i, opt;
    fd_set         rfds;
    struct timeval tv;      /* select() timeout -- re-set each iteration  */
    time_t         now;
#ifdef __MVS__
    char           modify_buf[128]; /* MODIFY command text (EBCDIC)       */
    int            modify_len;      /* bytes of command text returned      */
    int            cib_rc;          /* getcib return code                  */
#endif

    log_set_level(LOG_DEBUG);
    log_set_timestamps(1);
    log_proc_init();       /* per-procedure log levels -> inherit global */

    log_info("nfsd: starting up");

    dir_openlist_init();
    mvs_rcache_init();
    pww_init();
    mvsfsz_init();
    mvsprf_init();

    /* Cache the MVS local-time offset (CVTLDTO) once at startup: used to
       convert ISPF member stats (stored in local time) to/from UTC epoch.
       time()/gettimeofday() already return UTC, so they need no correction. */
    mvs_tz_init();
    log_info("nfsd: MVS local-time offset = %d seconds (0 = no offset)",
             mvs_tz_offset());
    
    //mvsfsz_load("//DDN:FILESIZE");
    
    while ((opt = getopt(argc, argv, "p:m:n:v")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_port(optarg, &port_pmap)  < 0) {
                log_error("nfsd: invalid port: %s", optarg);
                return 101;
            }
            break;
        case 'm':
            if (parse_port(optarg, &port_mount) < 0) {
                log_error("nfsd: invalid port: %s", optarg);
                return 102;
            }
            break;
        case 'n':
            if (parse_port(optarg, &port_nfs)   < 0) {
                log_error("nfsd: invalid port: %s", optarg);
                return 103;
            }
            break;
        case 'v': g_verbose  = 1;            break;
        default:
            log_error("usage: %s [-p pmap] [-m mount] [-n nfs] <config>",
                argv[0]);
            return 104;
        }
    }

    if (optind >= argc) {
        log_error("usage: %s [-p pmap] [-m mount] [-n nfs] <config>",
            argv[0]);
        return 105;
    }

    /* Load export configuration */
    n = exports_load(argv[optind]);
    if (n < 0) {
        log_error("nfsd: cannot open config: %s", argv[optind]);
        return 106;
    }
    log_info("nfsd: loaded %d export(s) from %s",
            n, argv[optind]);

    /* File handles are self-describing -- no handle cache to initialise. */

    /* Publish actual ports for portmapper responses */
    g_port_pmap  = port_pmap;
    g_port_mount = port_mount;
    g_port_nfs   = port_nfs;

    /* Write verifier: combine startup time + PID (a hashed JES2 job id on
     * MVS, getpid() elsewhere) to avoid collision on fast restarts (time()
     * has only 1-second granularity). */
    {
#ifdef __MVS__

        /* Get jobid ... e.g. "STC01234".  May be NULL if the PSA->TCB->JSCB
           ->SSIB chain does not resolve. */
        char *job_id = get_jes2_jobid();

        /* Convert that to a pseudo-pid via hash.  mvs_fid_ino32() treats a
           NULL name as zero-length, so this is NULL-safe. */
        uint32_t pid = mvs_fid_ino32(job_id, NULL);

        log_info("NFSD running as %s ... using pseudo-pid 0x%08X",
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

#ifndef __MVS__
    signal(SIGPIPE, SIG_IGN);
#endif

    pmap_sock  = make_listen_sock(port_pmap);
    mount_sock = make_listen_sock(port_mount);
    nfs_sock   = make_listen_sock(port_nfs);

    log_info(
        "Listening -- portmapper=%d  mount=%d  nfs=%d",
        port_pmap, port_mount, port_nfs);

    /* ---- Main select() event loop ---- */
    for (;;) {

        /* ---- Check for MVS operator commands ---- */
#ifdef __MVS__
        modify_len = 0;
        cib_rc = getcib(modify_buf, (size_t)(sizeof(modify_buf) - 1),
                        &modify_len);
        if (cib_rc == 2) {
            /* STOP (P) command received -- exit the loop cleanly */
            log_info("MVS STOP command received, shutting down");
            break;
        }
#endif

        /* ---- Fatal abend in the write path? (design_nfs_write.md Sec 7.3) --
         * The flush traps out-of-space abends and keeps serving, but anything
         * else is a probable program error: it has been reported loudly and we
         * now shut down rather than carry on from state we no longer trust.
         * Ending the task also lets MVS reclaim any allocation or SPFEDIT
         * enqueue the cleanup could not release. */
        if (pww_fatal_abend()) {
            log_error("unrecoverable abend in the write path -- shutting down");
            break;
        }

#ifdef __MVS__
        if (cib_rc == 1) {
            /* MODIFY (F) command received.  Hand the operand text to the
             * logger, which claims "SET LOGLVL ..." commands; anything it
             * does not recognise (return 1) is reported as unhandled. */
            if (modify_len > 0) {
                modify_buf[modify_len] = '\0';
                if (log_handle_modify(modify_buf) == 1)
                    log_warn("MVS MODIFY ignored (unrecognised): %s",
                             modify_buf);
            } else {
                log_error("MVS MODIFY received (no data)");
            }
        }
#endif

        /* ---- Build the fd_set ---- */
        FD_ZERO(&rfds);
        FD_SET(pmap_sock,  &rfds);
        FD_SET(mount_sock, &rfds);
        FD_SET(nfs_sock,   &rfds);

        maxfd = pmap_sock;
        if (mount_sock > maxfd) maxfd = mount_sock;
        if (nfs_sock   > maxfd) maxfd = nfs_sock;

        for (i = 0; i < g_nconns; i++) {
            FD_SET(g_conns[i].fd, &rfds);
            if (g_conns[i].fd > maxfd) maxfd = g_conns[i].fd;
        }

        /* ---- Wait for activity (2-second timeout to poll for STOP) ---- */
        tv.tv_sec  = 2;
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
           every wake-up (activity or the 2s timeout), so flush latency is
           bounded to a couple of seconds. */
        pww_flush_idle(time(NULL));

        if (n == 0) continue;  /* timeout -- go back and check for STOP  */

        if (FD_ISSET(pmap_sock,  &rfds)) accept_conn(pmap_sock,  PROTO_PORTMAP);
        if (FD_ISSET(mount_sock, &rfds)) accept_conn(mount_sock, PROTO_MOUNT);
        if (FD_ISSET(nfs_sock,   &rfds)) accept_conn(nfs_sock,   PROTO_NFS);

        for (i = 0; i < g_nconns; ) {
            if (FD_ISSET(g_conns[i].fd, &rfds)) {
                if (handle_connection(&g_conns[i]) < 0) {
                    /* Peer closed (or the RPC failed): close OUR half so a
                       FIN goes back and the descriptor is released.  Must be
                       sock_close() -- see its comment. */
                    log_warn("nfsd: closing connection fd=%d (proto=%d)",
                             g_conns[i].fd, g_conns[i].proto);
                    sock_close(g_conns[i].fd);
                    g_conns[i] = g_conns[--g_nconns];
                    continue;
                }
            }
            i++;
        }
    }

    /* Flush any outstanding buffered member writes before we exit. */
    pww_flush_all();

    log_info("Closing sockets");

    /* Close any still-open client connections before the listeners, so each
       peer gets a FIN rather than being left hanging. */
    for (i = 0; i < g_nconns; i++)
        sock_close(g_conns[i].fd);
    g_nconns = 0;

    sock_close(pmap_sock);
    sock_close(mount_sock);
    sock_close(nfs_sock);

    mvsprf_dump();
    log_info("Shutting down");

    return 0;
}
