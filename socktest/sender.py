#!/usr/bin/env python3
"""
sender.py -- traffic generator for the rxtest socket reproducer.

Sends framed messages to rxtest (running on MVS, or locally as a control):

    [4-byte big-endian length N] [N bytes of payload]

The payload is self-describing: the 4-byte word at byte offset o holds

    (sequence << 20) | o

so the receiver can tell from a single wrong word exactly which message and
which offset the bytes really came from.  A stack that replays a message from
its start produces words whose encoded offset is lower than their position --
which is the defect this exists to demonstrate.

THE POINT OF THE PAUSES
-----------------------
The bug only shows up when recv() returns a PARTIAL read, so this sender goes
out of its way to cause them: each message is written in several pieces with a
short sleep between them, which forces separate TCP segments and gives the
receiver a good chance of waking up with only part of a message buffered.
Sizes and split points are randomised so the boundary lands in a different
place every time -- the original corruption was rare precisely because it
needs a segment boundary to fall mid-message.

TCP_NODELAY is essential here: without it Nagle would coalesce the pieces back
into one segment and no partial read would ever occur.

USAGE
    python3 sender.py --host 192.168.1.168 [--port 5555] [--count 2000]
    python3 sender.py --host mvs --min-size 400 --max-size 2000 --chunks 4
    python3 sender.py --host mvs --seed 42          # reproducible run

Send a zero-length message at the end to tell rxtest to stop and print its
summary; that happens automatically unless --no-eof is given.
"""

import argparse
import random
import socket
import struct
import sys
import time


def build_payload(seq, nbytes):
    """nbytes of self-describing data (nbytes is rounded down to a multiple
    of 4 by the caller).  Word at offset o == (seq << 20) | o."""
    if seq >= 4096:
        raise ValueError("sequence must be < 4096 to fit the 12-bit field")
    if nbytes > 0x100000:
        raise ValueError("message must be < 1 MiB to fit the 20-bit offset")
    base = seq << 20
    return b"".join(struct.pack(">I", base | o) for o in range(0, nbytes, 4))


def send_fragmented(sock, blob, chunks, pause):
    """Write blob in `chunks` pieces with `pause` seconds between them, so it
    crosses TCP segment boundaries at unpredictable places."""
    n = len(blob)
    if chunks <= 1 or n < 8:
        sock.sendall(blob)
        return

    # Random split points, always leaving at least 4 bytes per piece.
    cuts = sorted(random.sample(range(4, n - 3), min(chunks - 1, n - 8)))
    prev = 0
    for cut in cuts + [n]:
        sock.sendall(blob[prev:cut])
        prev = cut
        if pause > 0:
            time.sleep(pause)


def main(argv=None):
    ap = argparse.ArgumentParser(description="rxtest traffic generator")
    ap.add_argument("--host", required=True, help="rxtest host (MVS system)")
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--count", type=int, default=2000,
                    help="messages to send (default 2000)")
    ap.add_argument("--min-size", type=int, default=400,
                    help="smallest payload in bytes (default 400)")
    ap.add_argument("--max-size", type=int, default=2000,
                    help="largest payload in bytes (default 2000)")
    ap.add_argument("--chunks", type=int, default=3,
                    help="pieces to split each message into (default 3)")
    ap.add_argument("--pause", type=float, default=0.002,
                    help="seconds between pieces (default 0.002)")
    ap.add_argument("--seed", type=int, default=None,
                    help="RNG seed, for a byte-identical repeat run")
    ap.add_argument("--no-eof", action="store_true",
                    help="do not send the zero-length end marker")
    args = ap.parse_args(argv)

    if args.seed is not None:
        random.seed(args.seed)
    if args.count > 4096:
        print("note: sequence field is 12 bits; capping count at 4096")
        args.count = 4096

    sock = socket.create_connection((args.host, args.port), timeout=30)
    # Without this, Nagle coalesces the pieces and no partial read happens.
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    print("sender: connected to %s:%d" % (args.host, args.port))
    print("sender: %d messages, %d-%d bytes, %d chunks, %.3fs pause"
          % (args.count, args.min_size, args.max_size, args.chunks,
             args.pause))

    total = 0
    t0 = time.time()
    try:
        for seq in range(args.count):
            size = random.randint(args.min_size, args.max_size) & ~3
            blob = struct.pack(">I", size) + build_payload(seq, size)
            send_fragmented(sock, blob, args.chunks, args.pause)
            total += size
            if seq % 200 == 0 and seq:
                print("  ... %d messages sent" % seq)

        if not args.no_eof:
            sock.sendall(struct.pack(">I", 0))   # end marker
    except (BrokenPipeError, ConnectionResetError) as e:
        print("sender: connection lost after %d messages: %s" % (seq, e))
        return 1
    finally:
        try:
            sock.close()
        except OSError:
            pass

    dt = time.time() - t0
    print("sender: done - %d messages, %d payload bytes, %.1fs"
          % (args.count, total, dt))
    print("sender: now read rxtest's summary on the receiving side.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
