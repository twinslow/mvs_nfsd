/*
 * rxtest.c - minimal reproducer for a socket receive defect on MVS 3.8J
 *            under Hercules.
 *
 * THE DEFECT
 * ----------
 * While developing an NFSv3 server (dino-nfs) we saw PDS members
 * intermittently corrupted: a chunk of an inbound RPC message appeared in
 * the middle of the file data.  Instrumenting the receive path showed a
 * 636-byte message arriving in the buffer as
 *
 *     M[0..255]  followed by  M[0..379]
 *
 * i.e. recv() returned 256 bytes, and the NEXT recv() replayed the message
 * from byte 0 instead of continuing at byte 256.  The receiving loop writes
 * to buf+done, so the destination was correct -- the SOURCE restarted.  The
 * TCP record mark and the message's own length fields agreed, so this is not
 * a framing error: bytes that had already been handed to the application
 * were handed over a second time.
 *
 * In short: a PARTIAL recv() appears not to advance the socket's read
 * pointer, so the following recv() re-delivers data already consumed.
 *
 * MVS 3.8J has no native TCP/IP stack; sockets are provided by a custom 370
 * opcode that bridges to the host's stack under Hercules.  The defect may
 * therefore live in that bridge, in the JCC socket library, or in the
 * interaction between them -- this program does not assume which.
 *
 * WHAT THIS PROGRAM DOES
 * ----------------------
 * It is a TCP server speaking a deliberately trivial framing protocol, so
 * nothing about NFS or RPC is required to reproduce the problem:
 *
 *     [4-byte big-endian length N] [N bytes of payload]
 *
 * The payload is self-describing.  Every 4-byte word encodes both the
 * message number it belongs to and its own offset within that message:
 *
 *     word at byte offset o  ==  (sequence << 20) | o
 *
 * So a single mismatched word says exactly where the wrong bytes came from.
 * If word 64 of message 7 reads as (7 << 20) | 0, the stack replayed message
 * 7 from its beginning -- which is precisely the failure above.
 *
 * The companion sender (sender.py) deliberately splits each message across
 * several TCP segments with small pauses, to force recv() to return partial
 * reads.  Partial reads are perfectly legal; handling them is the caller's
 * job.  The bug is that doing so correctly is not sufficient here.
 *
 * TWO RECEIVE STRATEGIES
 * ----------------------
 *   default   the ordinary loop -- recv() the whole remainder, advance by
 *             whatever it returns, repeat.  This is what dino-nfs does, and
 *             what every sockets tutorial teaches.
 *
 *   -f        ask ioctlsocket(FIONREAD) how many bytes are actually
 *             available and never request more than that, so recv() should
 *             never need to return short.  If the defect is "a short read
 *             does not consume", this avoids it -- making -f a candidate
 *             workaround as well as a diagnostic.
 *
 * Run it both ways: identical traffic, and the difference (if any) localises
 * the fault to the short-read path.
 *
 * BUILD
 *   MVS:   see jcl/rxtest.jcl
 *   Linux: cc -o rxtest rxtest.c        (a control run; expect zero replays)
 *
 * USAGE
 *   rxtest [-p port] [-f] [-v]
 *      -p   port to listen on (default 5555)
 *      -f   use the FIONREAD strategy instead of the plain loop
 *      -v   print a line per message
 *
 * JCC C89: declarations precede statements; block comments only.
 */

#ifdef __MVS__
#include <sockets.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Platform shims                                                       */
/* ------------------------------------------------------------------ */
#ifdef __MVS__
#define SOCK_CLOSE(fd)      closesocket(fd)
#define SET_REUSE(fd, p)    setsockopt((fd), SO_REUSEADDR, (p), sizeof(int))
typedef long int            addrlen_t;
#else
#define SOCK_CLOSE(fd)      close(fd)
#define SET_REUSE(fd, p)    setsockopt((fd), SOL_SOCKET, SO_REUSEADDR, \
                                       (p), sizeof(int))
typedef unsigned int        addrlen_t;
#define ioctlsocket         ioctl
#endif

#define MAX_MSG      (256u * 1024u)  /* largest payload accepted        */
#define MAX_TRACE    512             /* recv() calls recorded per msg   */
#define MAX_REPORTS  10              /* detailed reports before summary */

static unsigned char g_buf[MAX_MSG];

/* One entry per recv() call, so a failure can be explained by the exact
   sequence of partial reads that produced it. */
static struct {
    unsigned long requested;
    long          returned;
    unsigned long at;                /* buffer offset written to        */
} g_trace[MAX_TRACE];
static int g_ntrace = 0;

/* Totals for the closing summary. */
static unsigned long g_msgs      = 0;
static unsigned long g_bytes     = 0;
static unsigned long g_recvcalls = 0;
static unsigned long g_partials  = 0;   /* recv() returned < requested   */
static unsigned long g_badmsgs   = 0;
static int           g_reports   = 0;

static int g_use_fionread = 0;
static int g_verbose      = 0;

/* ------------------------------------------------------------------ */
static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] <<  8) |  (unsigned long)p[3];
}

/* ------------------------------------------------------------------ */
/* How many bytes can be read without blocking.  Returns -1 if the      */
/* query itself is unsupported or fails, so the caller can fall back.   */
/* ------------------------------------------------------------------ */
static long bytes_available(int fd)
{
#ifdef FIONREAD
    long navail = 0;
    if (ioctlsocket(fd, FIONREAD, (char *)&navail) < 0)
        return -1;
    return navail;
#else
    (void)fd;
    return -1;
#endif
}

/* Block until fd is readable.  Used only by the FIONREAD strategy, which
   must not call recv() speculatively. */
static int wait_readable(int fd)
{
    fd_set rfds;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    return select(fd + 1, &rfds, (fd_set *)0, (fd_set *)0,
                  (struct timeval *)0);
}

/* ------------------------------------------------------------------ */
/* Read exactly len bytes into buf, recording every recv() call.        */
/* Returns 0, or -1 on EOF/error.                                       */
/* ------------------------------------------------------------------ */
static int rx_all(int fd, unsigned char *buf, unsigned long len)
{
    unsigned long done = 0;
    unsigned long want;
    long          navail;
    long          n;

    while (done < len) {
        want = len - done;

        if (g_use_fionread) {
            /* Never ask for more than is really there, so recv() has no
               reason to return short.  If nothing is buffered yet, wait for
               readability rather than issuing a speculative recv(). */
            navail = bytes_available(fd);
            if (navail < 0) {
                fprintf(stderr, "rxtest: FIONREAD unsupported (errno %d) -"
                                " falling back to the plain loop\n", errno);
                g_use_fionread = 0;
            } else if (navail == 0) {
                if (wait_readable(fd) <= 0) return -1;
                continue;
            } else if ((unsigned long)navail < want) {
                want = (unsigned long)navail;
            }
        }

        n = (long)recv(fd, (char *)(buf + done), want, 0);
        g_recvcalls++;
        if (g_ntrace < MAX_TRACE) {
            g_trace[g_ntrace].requested = want;
            g_trace[g_ntrace].returned  = n;
            g_trace[g_ntrace].at        = done;
            g_ntrace++;
        }
        if (n <= 0) return -1;
        if ((unsigned long)n < want) g_partials++;
        done += (unsigned long)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
static void dump_trace(void)
{
    int i;

    fprintf(stderr, "  recv() call trace for this message:\n");
    for (i = 0; i < g_ntrace; i++)
        fprintf(stderr, "    %2d: at buf+%-6lu requested %-6lu returned %ld%s\n",
                i, g_trace[i].at, g_trace[i].requested, g_trace[i].returned,
                (g_trace[i].returned >= 0 &&
                 (unsigned long)g_trace[i].returned < g_trace[i].requested)
                    ? "   <-- PARTIAL" : "");
}

/* ------------------------------------------------------------------ */
/* Verify the self-describing payload.  Every word must equal            */
/* (seq << 20) | its own offset; anything else names its true origin.    */
/* Returns 0 if the message is intact.                                   */
/* ------------------------------------------------------------------ */
static int verify(unsigned long seq, unsigned long len)
{
    unsigned long o;
    unsigned long expect;
    unsigned long actual;
    unsigned long src_seq;
    unsigned long src_off;
    unsigned long nbad = 0;
    unsigned long first_bad = 0;

    for (o = 0; o + 4 <= len; o += 4) {
        expect = (seq << 20) | o;
        actual = be32(g_buf + o);
        if (actual != expect) {
            if (nbad == 0) first_bad = o;
            nbad++;
        }
    }
    if (nbad == 0) return 0;

    g_badmsgs++;
    if (g_reports >= MAX_REPORTS) return -1;
    g_reports++;

    actual  = be32(g_buf + first_bad);
    src_seq = actual >> 20;
    src_off = actual & 0xFFFFFuL;

    fprintf(stderr,
        "\n*** CORRUPTION in message %lu (%lu bytes): %lu bad word(s),"
        " first at offset %lu\n", seq, len, nbad, first_bad);
    fprintf(stderr,
        "  expected word (msg %lu, offset %lu), got word (msg %lu,"
        " offset %lu)\n", seq, first_bad, src_seq, src_off);

    if (src_seq == seq && src_off < first_bad)
        fprintf(stderr,
            "  => REPLAY: the stack re-delivered THIS message starting at"
            " offset %lu.\n     %lu byte(s) had already been consumed;"
            " recv() handed them over again.\n", src_off, first_bad);
    else if (src_seq != seq)
        fprintf(stderr,
            "  => CROSS-MESSAGE: bytes belong to message %lu, not %lu.\n",
            src_seq, seq);
    else
        fprintf(stderr, "  => data is out of order within this message.\n");

    dump_trace();
    return -1;
}

/* ------------------------------------------------------------------ */
static void serve(int cfd)
{
    unsigned char hdr[4];
    unsigned long len;
    unsigned long seq = 0;

    for (;;) {
        g_ntrace = 0;

        if (rx_all(cfd, hdr, 4) < 0) break;      /* clean EOF ends the run */
        len = be32(hdr);
        if (len == 0) break;                     /* sender says "done"     */
        if (len > MAX_MSG) {
            fprintf(stderr, "rxtest: message %lu too large (%lu bytes)\n",
                    seq, len);
            break;
        }
        if (rx_all(cfd, g_buf, len) < 0) {
            fprintf(stderr, "rxtest: short read on message %lu\n", seq);
            break;
        }

        g_msgs++;
        g_bytes += len;
        (void)verify(seq, len);

        if (g_verbose)
            printf("msg %lu: %lu bytes, %d recv() call(s)\n",
                   seq, len, g_ntrace);
        seq++;
    }
}

/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int                port = 5555;
    int                lfd;
    int                cfd;
    int                opt = 1;
    int                i;
    struct sockaddr_in addr;
    struct sockaddr_in peer;
    addrlen_t          plen;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-f") == 0)            g_use_fionread = 1;
        else if (strcmp(argv[i], "-v") == 0)            g_verbose = 1;
        else {
            fprintf(stderr, "usage: %s [-p port] [-f] [-v]\n", argv[0]);
            return 2;
        }
    }

    printf("rxtest: listening on port %d, strategy = %s\n",
           port, g_use_fionread ? "FIONREAD-clamped" : "plain recv loop");

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    if (SET_REUSE(lfd, (char *)&opt) < 0) perror("setsockopt SO_REUSEADDR");

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((unsigned short)port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(lfd, 4) < 0) { perror("listen"); return 1; }

    plen = sizeof(peer);
    cfd  = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) { perror("accept"); return 1; }
    printf("rxtest: connection accepted\n");

    serve(cfd);

    SOCK_CLOSE(cfd);
    SOCK_CLOSE(lfd);

    printf("\n================ rxtest summary ================\n");
    printf("strategy           : %s\n",
           g_use_fionread ? "FIONREAD-clamped" : "plain recv loop");
    printf("messages received  : %lu\n", g_msgs);
    printf("bytes received     : %lu\n", g_bytes);
    printf("recv() calls       : %lu\n", g_recvcalls);
    printf("partial reads      : %lu\n", g_partials);
    printf("CORRUPT messages   : %lu\n", g_badmsgs);
    printf("================================================\n");
    if (g_badmsgs > 0)
        printf("RESULT: FAILED - the socket layer delivered wrong bytes.\n");
    else if (g_partials == 0)
        printf("RESULT: inconclusive - no partial reads occurred, so the\n"
               "        suspect path was never exercised.  Make the sender\n"
               "        fragment harder (smaller chunks / longer pauses).\n");
    else
        printf("RESULT: PASSED - %lu partial read(s) all handled correctly.\n",
               g_partials);

    return (g_badmsgs > 0) ? 1 : 0;
}
