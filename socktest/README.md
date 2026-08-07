# socktest — a minimal reproducer for a socket `recv()` defect on MVS 3.8J

This directory is **self-contained and independent of MVS NFSD**. Nothing here
knows about NFS, RPC or PDS members. It exists to demonstrate, in the smallest
possible way, a defect in the socket receive path on MVS 3.8J under Hercules.

## The defect

While developing an NFSv3 server we saw PDS members intermittently corrupted:
part of an inbound RPC message turned up in the middle of the file data.
Instrumenting the receive path caught a 636-byte message sitting in the buffer
as

```
M[0..255]   followed by   M[0..379]
```

`recv()` returned 256 bytes, and the **next** `recv()` replayed the message
from byte 0 instead of continuing at byte 256. The receive loop writes to
`buf + done`, so the destination was right — the **source** restarted.

Evidence that this is not an application-level framing mistake:

* the TCP record mark (636) and the message's own length fields agreed
  exactly, so the framing was correct;
* the message arrived as a **single** fragment, so no reassembly was involved;
* both copies carried the **same XID**, and `nfsstat -c` showed **0 retrans in
  27,552 calls** over a window covering a known corruption — so the client
  sent the request exactly once.

In short: **a partial `recv()` appears not to advance the socket's read
pointer, so the following `recv()` re-delivers bytes already consumed.**

MVS 3.8J has no native TCP/IP stack — sockets are provided by a custom 370
opcode bridging to the host stack under Hercules. The fault could be in that
bridge, in the JCC socket library, or in how they interact. This program takes
no position on which; it just shows the behaviour.

## How it works

`rxtest` is a TCP server speaking a deliberately trivial protocol:

```
[4-byte big-endian length N] [N bytes of payload]
```

The payload is **self-describing** — the 4-byte word at byte offset `o` holds

```
(sequence << 16) | o
```

so any single wrong word identifies exactly which message and which offset the
bytes really came from. The 16/16 split allows **65,535 messages** of up to
65,532 bytes. A replay produces words whose encoded offset is
*lower* than their position, and `rxtest` says so in as many words:

```
*** CORRUPTION in message 7 (636 bytes): 95 bad word(s), first at offset 256
  expected word (msg 7, offset 256), got word (msg 7, offset 0)
  => REPLAY: the stack re-delivered THIS message starting at offset 0.
     256 byte(s) had already been consumed; recv() handed them over again.
  recv() call trace for this message:
     0: at buf+0      requested 636    returned 256   <-- PARTIAL
     1: at buf+256    requested 380    returned 380
```

`sender.py` splits every message into several TCP segments with short pauses
(and sets `TCP_NODELAY`, without which Nagle would coalesce them again),
because the bug only appears when `recv()` returns a partial read. Split
points are randomised so the boundary lands somewhere different each time —
the original corruption was rare precisely because it needs a segment boundary
to fall mid-message.

## Reproducing the real workload

The corruption was found in a **request/response** server -- receive a request,
do slow disk I/O, send a reply, receive the next -- whose main loop `select()`s
for readability before every read. A one-way firehose with no `send()` on the
socket may simply never reach the state that breaks, so two options restore
the real shape of the traffic:

| Option | Effect |
|---|---|
| `rxtest -r N` / `sender.py --reply N` | An N-byte reply after each message, so the two directions strictly alternate |
| `rxtest -s` | `select()` before each message, exactly as `nfsd.c` does before calling `rpc_recv()` |
| `sender.py --think SEC` | Pause after each reply, so the server sits idle in `select()` between requests |

**Recommended when hunting the original defect:**

```
rxtest -p 5555 -r 64 -s
python3 sender.py --host <mvs-host> --count 50000 --reply 64 --think 0.001
```

## How many messages?

The defect needs a TCP segment boundary to land mid-message, so it is rare --
in the NFS server it appeared roughly once in several thousand writes. Runs of
**tens of thousands** of messages are expected before it shows; `--count`
defaults to 50000 and the protocol allows up to 65535. A clean run of a few
hundred means nothing.

## Two receive strategies

| Mode | Behaviour |
|---|---|
| default | The ordinary loop: `recv()` the remainder, advance by what it returns, repeat. What every sockets tutorial teaches, and what MVS NFSD does. |
| `-f` | Ask `ioctlsocket(FIONREAD)` how many bytes are actually available and never request more, so `recv()` should never need to return short. |

Run both with identical sender settings. If the plain loop reports corruption
and `-f` does not, the fault is confined to the short-read path — and clamping
requests to `FIONREAD` becomes a viable workaround, not just a diagnostic.

## Running it

**On MVS**

1. Upload `src/rxtest.c` to `TONYW.SOCKTEST.C(RXTEST)`.
2. Submit `jcl/rxtest.jcl`. The job waits at `accept()`, so start the sender
   promptly.
3. Read the summary in the GO step's STDOUT.

Add `-f` via `PARM.GO` for the second run.

**On Linux (sender)**

```bash
python3 sender.py --host <mvs-host> --port 5555 --count 50000
```

Useful knobs: `--min-size` / `--max-size`, `--chunks` (pieces per message),
`--pause` (seconds between pieces), `--seed` (byte-identical repeat run),
`--reply` / `--think` (request/response mode, above).

**As a control**, run the receiver on Linux too — it is portable C89 and
builds with `cc -o rxtest src/rxtest.c`. A healthy stack should report many
partial reads and zero corruption. Establishing that baseline first means a
failure on MVS cannot be blamed on the test.

## Reading the result

```
messages received  : 50000
recv() calls       : 247512
partial reads      : 148903
replies sent       : 50000
CORRUPT messages   : 0
RESULT: PASSED - 148903 partial read(s) all handled correctly.
```

Three outcomes:

* **FAILED** — the socket layer delivered wrong bytes; the report names the
  offset and the true origin of the data.
* **PASSED** — partial reads occurred and were all handled correctly.
* **inconclusive** — *no* partial reads happened, so the suspect path was
  never exercised. Increase `--chunks`, increase `--pause`, or reduce the
  message size and try again. Do not read this as a pass.

## Validation of the harness itself

The payload format, the verification logic and the replay reporting were
exercised against a mock receiver before use: 300 messages produced 899
partial reads out of 1500 `recv()` calls with zero false positives, and an
injected replay matching the observed MVS signature (`M[0..255]` then `M[0..]`)
was correctly reported at offset 256 as originating from offset 0.
