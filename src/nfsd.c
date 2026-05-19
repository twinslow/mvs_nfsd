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

#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "nfsd.h"

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
/* Global write verifier: set once at startup from time()               */
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

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        perror("nfsd: setsockopt SO_REUSEADDR");

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "nfsd: bind port %d: %s\n", port, strerror(errno));
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
    socklen_t          plen = sizeof(peer);
    int                cfd;

    cfd = accept(lsock, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) return;

    if (g_nconns >= MAX_CONNECTIONS) {
        fprintf(stderr, "nfsd: connection table full (%d), dropping\n",
                MAX_CONNECTIONS);
        close(cfd);
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
    xdr_init_write(&out, g_send_buf, BUF_SIZE);

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
    int     pmap_sock, mount_sock, nfs_sock;
    int     port_pmap  = PORT_PORTMAP;
    int     port_mount = PORT_MOUNT;
    int     port_nfs   = PORT_NFS;
    int     maxfd, n, i, opt;
    fd_set  rfds;
    time_t  now;

    while ((opt = getopt(argc, argv, "p:m:n:v")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_port(optarg, &port_pmap)  < 0) {
                fprintf(stderr, "nfsd: invalid port: %s\n", optarg);
                return 1;
            }
            break;
        case 'm':
            if (parse_port(optarg, &port_mount) < 0) {
                fprintf(stderr, "nfsd: invalid port: %s\n", optarg);
                return 1;
            }
            break;
        case 'n':
            if (parse_port(optarg, &port_nfs)   < 0) {
                fprintf(stderr, "nfsd: invalid port: %s\n", optarg);
                return 1;
            }
            break;
        case 'v': g_verbose  = 1;            break;
        default:
            fprintf(stderr,
                "usage: %s [-p pmap] [-m mount] [-n nfs] <config>\n",
                argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr,
            "usage: %s [-p pmap] [-m mount] [-n nfs] <config>\n",
            argv[0]);
        return 1;
    }

    /* Load export configuration */
    n = exports_load(argv[optind]);
    if (n < 0) {
        fprintf(stderr, "nfsd: cannot open config: %s\n", argv[optind]);
        return 1;
    }
    fprintf(stderr, "nfsd: loaded %d export(s) from %s\n",
            n, argv[optind]);

    fh_init();

    /* Publish actual ports for portmapper responses */
    g_port_pmap  = port_pmap;
    g_port_mount = port_mount;
    g_port_nfs   = port_nfs;

    /* Write verifier: combine startup time + PID to avoid collision on
     * fast restarts (time() has only 1-second granularity). */
    {
        pid_t pid = getpid();
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

    signal(SIGPIPE, SIG_IGN);

    pmap_sock  = make_listen_sock(port_pmap);
    mount_sock = make_listen_sock(port_mount);
    nfs_sock   = make_listen_sock(port_nfs);

    fprintf(stderr,
        "nfsd: listening -- portmapper=%d  mount=%d  nfs=%d\n",
        port_pmap, port_mount, port_nfs);

    /* ---- Main select() event loop ---- */
    for (;;) {
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

        n = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(pmap_sock,  &rfds)) accept_conn(pmap_sock,  PROTO_PORTMAP);
        if (FD_ISSET(mount_sock, &rfds)) accept_conn(mount_sock, PROTO_MOUNT);
        if (FD_ISSET(nfs_sock,   &rfds)) accept_conn(nfs_sock,   PROTO_NFS);

        for (i = 0; i < g_nconns; ) {
            if (FD_ISSET(g_conns[i].fd, &rfds)) {
                if (handle_connection(&g_conns[i]) < 0) {
                    close(g_conns[i].fd);
                    g_conns[i] = g_conns[--g_nconns];
                    continue;
                }
            }
            i++;
        }
    }

    close(pmap_sock);
    close(mount_sock);
    close(nfs_sock);
    return 0;
}
