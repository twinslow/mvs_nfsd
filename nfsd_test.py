#!/usr/bin/env python3
"""
nfsd_test.py - Functional test client for the minimal NFSv3 server.

Tests the full protocol stack over TCP on configurable ports so it
can be run without root privileges:

    # Start server on high ports in one terminal:
    ./nfsd -p 11111 -m 12048 -n 12049 nfsd.conf

    # Run tests in another:
    python3 nfsd_test.py [host [pmap_port [mount_port [nfs_port]]]]

Defaults: host=127.0.0.1  pmap=11111  mount=12048  nfs=12049
"""

import sys
import socket
import struct
import time
import os

# ------------------------------------------------------------------ #
# Configuration                                                        #
# ------------------------------------------------------------------ #
HOST       = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT_PMAP  = int(sys.argv[2]) if len(sys.argv) > 2 else 11111
PORT_MOUNT = int(sys.argv[3]) if len(sys.argv) > 3 else 12048
PORT_NFS   = int(sys.argv[4]) if len(sys.argv) > 4 else 12049

EXPORT_PATH = "/export/src"   # must match an entry in nfsd.conf

# ------------------------------------------------------------------ #
# RPC / XDR helpers                                                    #
# ------------------------------------------------------------------ #
_xid = int(time.time()) & 0xFFFFFFFF

def next_xid():
    global _xid
    _xid = (_xid + 1) & 0xFFFFFFFF
    return _xid

def u32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)

def u64(v):
    return struct.pack(">Q", v & 0xFFFFFFFFFFFFFFFF)

def xdr_string(s):
    b = s.encode()
    n = len(b)
    pad = (4 - n % 4) % 4
    return struct.pack(">I", n) + b + b'\x00' * pad

def xdr_opaque(data):
    n = len(data)
    pad = (4 - n % 4) % 4
    return struct.pack(">I", n) + data + b'\x00' * pad

def auth_null():
    return u32(0) + u32(0)   # flavor=0, length=0

def rpc_call(prog, vers, proc, args_bytes):
    xid = next_xid()
    header = (u32(xid) + u32(0) + u32(2) +   # xid, CALL, rpcvers=2
              u32(prog) + u32(vers) + u32(proc) +
              auth_null() +                    # credentials
              auth_null())                     # verifier
    body = header + args_bytes
    rm = struct.pack(">I", 0x80000000 | len(body))
    return xid, rm + body

def send_recv(sock, data):
    sock.sendall(data)
    # Read record mark
    rm_bytes = b''
    while len(rm_bytes) < 4:
        chunk = sock.recv(4 - len(rm_bytes))
        if not chunk:
            raise EOFError("connection closed")
        rm_bytes += chunk
    rm = struct.unpack(">I", rm_bytes)[0]
    length = rm & 0x7FFFFFFF
    # Read payload
    payload = b''
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise EOFError("connection closed")
        payload += chunk
    return payload

def parse_reply_hdr(data):
    """Return (xid, accept_stat, offset_after_header) or raise."""
    xid        = struct.unpack_from(">I", data, 0)[0]
    msg_type   = struct.unpack_from(">I", data, 4)[0]   # 1 = REPLY
    reply_stat = struct.unpack_from(">I", data, 8)[0]   # 0 = ACCEPTED
    # skip verifier: flavor(4) + len(4) + data(len)
    verf_flavor = struct.unpack_from(">I", data, 12)[0]
    verf_len    = struct.unpack_from(">I", data, 16)[0]
    offset = 20 + ((verf_len + 3) & ~3)
    accept_stat = struct.unpack_from(">I", data, offset)[0]
    return xid, accept_stat, offset + 4

def conn(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect((HOST, port))
    return s

PASS = 0
FAIL = 0

def ok(label):
    global PASS
    PASS += 1
    print(f"  PASS  {label}")

def fail(label, reason=""):
    global FAIL
    FAIL += 1
    print(f"  FAIL  {label}" + (f": {reason}" if reason else ""))

# ------------------------------------------------------------------ #
# Test: Portmapper                                                     #
# ------------------------------------------------------------------ #
def test_portmapper():
    print("\n=== Portmapper (port {}) ===".format(PORT_PMAP))
    s = conn(PORT_PMAP)

    # NULL
    xid, pkt = rpc_call(100000, 2, 0, b'')
    resp = send_recv(s, pkt)
    _, stat, _ = parse_reply_hdr(resp)
    if stat == 0:
        ok("PMAPPROC_NULL")
    else:
        fail("PMAPPROC_NULL", f"accept_stat={stat}")

    # GETPORT for NFS (prog=100003, vers=3, proto=TCP=6)
    args = u32(100003) + u32(3) + u32(6) + u32(0)
    xid, pkt = rpc_call(100000, 2, 3, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    port = struct.unpack_from(">I", resp, off)[0]
    if stat == 0 and port == PORT_NFS:
        ok(f"PMAPPROC_GETPORT NFS -> {port}")
    else:
        fail("PMAPPROC_GETPORT NFS", f"stat={stat} port={port}")

    # GETPORT for MOUNT
    args = u32(100005) + u32(3) + u32(6) + u32(0)
    xid, pkt = rpc_call(100000, 2, 3, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    port = struct.unpack_from(">I", resp, off)[0]
    if stat == 0 and port == PORT_MOUNT:
        ok(f"PMAPPROC_GETPORT MOUNT -> {port}")
    else:
        fail("PMAPPROC_GETPORT MOUNT", f"stat={stat} port={port}")

    # GETPORT for unknown program
    args = u32(999999) + u32(1) + u32(6) + u32(0)
    xid, pkt = rpc_call(100000, 2, 3, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    port = struct.unpack_from(">I", resp, off)[0]
    if stat == 0 and port == 0:
        ok("PMAPPROC_GETPORT unknown -> 0")
    else:
        fail("PMAPPROC_GETPORT unknown", f"stat={stat} port={port}")

    s.close()

# ------------------------------------------------------------------ #
# Test: Mount                                                          #
# ------------------------------------------------------------------ #
ROOT_FH = None   # filled in by test_mount

def test_mount():
    global ROOT_FH
    print("\n=== Mount (port {}) ===".format(PORT_MOUNT))
    s = conn(PORT_MOUNT)

    # NULL
    xid, pkt = rpc_call(100005, 3, 0, b'')
    resp = send_recv(s, pkt)
    _, stat, _ = parse_reply_hdr(resp)
    if stat == 0:
        ok("MOUNTPROC3_NULL")
    else:
        fail("MOUNTPROC3_NULL", f"stat={stat}")

    # EXPORT
    xid, pkt = rpc_call(100005, 3, 5, b'')
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    vf = struct.unpack_from(">I", resp, off)[0]
    if stat == 0 and vf == 1:
        ok("MOUNTPROC3_EXPORT (has entries)")
    else:
        fail("MOUNTPROC3_EXPORT", f"stat={stat} vf={vf}")

    # MNT for our export
    args = xdr_string(EXPORT_PATH)
    xid, pkt = rpc_call(100005, 3, 1, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    mnt_stat = struct.unpack_from(">I", resp, off)[0]
    off += 4
    if mnt_stat != 0:
        fail("MOUNTPROC3_MNT", f"mnt_stat={mnt_stat}")
        s.close()
        return

    # Read returned fhandle
    fh_len = struct.unpack_from(">I", resp, off)[0]
    off += 4
    ROOT_FH = resp[off:off + fh_len]
    ok(f"MOUNTPROC3_MNT -> fh={ROOT_FH.hex()}")

    # UMNT (accept and ignore)
    args = xdr_string(EXPORT_PATH)
    xid, pkt = rpc_call(100005, 3, 3, args)
    resp = send_recv(s, pkt)
    _, stat, _ = parse_reply_hdr(resp)
    if stat == 0:
        ok("MOUNTPROC3_UMNT")
    else:
        fail("MOUNTPROC3_UMNT", f"stat={stat}")

    # Re-mount for NFS tests
    args = xdr_string(EXPORT_PATH)
    xid, pkt = rpc_call(100005, 3, 1, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    mnt_stat = struct.unpack_from(">I", resp, off)[0]
    off += 4
    if mnt_stat == 0:
        fh_len = struct.unpack_from(">I", resp, off)[0]
        off += 4
        ROOT_FH = resp[off:off + fh_len]

    s.close()

# ------------------------------------------------------------------ #
# Test: NFSv3                                                          #
# ------------------------------------------------------------------ #
def test_nfs3():
    if ROOT_FH is None:
        print("\n=== NFS3 (SKIPPED: no root fhandle) ===")
        return

    print("\n=== NFSv3 (port {}) ===".format(PORT_NFS))
    s = conn(PORT_NFS)
    fh_arg = xdr_opaque(ROOT_FH)

    # NULL
    xid, pkt = rpc_call(100003, 3, 0, b'')
    resp = send_recv(s, pkt)
    _, stat, _ = parse_reply_hdr(resp)
    if stat == 0:
        ok("NFS3PROC_NULL")
    else:
        fail("NFS3PROC_NULL", f"stat={stat}")

    # GETATTR on root
    xid, pkt = rpc_call(100003, 3, 1, fh_arg)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    off += 4
    if nfs_stat == 0:
        ftype = struct.unpack_from(">I", resp, off)[0]
        ok(f"NFS3PROC_GETATTR root (ftype={ftype})")
    else:
        fail("NFS3PROC_GETATTR root", f"nfs_stat={nfs_stat}")

    # FSSTAT
    xid, pkt = rpc_call(100003, 3, 18, fh_arg)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    if nfs_stat == 0:
        ok("NFS3PROC_FSSTAT")
    else:
        fail("NFS3PROC_FSSTAT", f"nfs_stat={nfs_stat}")

    # FSINFO
    xid, pkt = rpc_call(100003, 3, 19, fh_arg)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    if nfs_stat == 0:
        ok("NFS3PROC_FSINFO")
    else:
        fail("NFS3PROC_FSINFO", f"nfs_stat={nfs_stat}")

    # PATHCONF
    xid, pkt = rpc_call(100003, 3, 20, fh_arg)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    if nfs_stat == 0:
        ok("NFS3PROC_PATHCONF")
    else:
        fail("NFS3PROC_PATHCONF", f"nfs_stat={nfs_stat}")

    # ACCESS
    args = fh_arg + u32(0x3F)    # request all bits
    xid, pkt = rpc_call(100003, 3, 4, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    if nfs_stat == 0:
        ok("NFS3PROC_ACCESS root")
    else:
        fail("NFS3PROC_ACCESS root", f"nfs_stat={nfs_stat}")

    # READDIRPLUS on root (cookie=0, cookieverf=0, dircount=512, maxcount=4096)
    args = (fh_arg + u64(0) + struct.pack(">8s", b'\x00'*8) +
            u32(512) + u32(4096))
    xid, pkt = rpc_call(100003, 3, 17, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    if nfs_stat == 0:
        ok("NFS3PROC_READDIRPLUS root")
    else:
        fail("NFS3PROC_READDIRPLUS root", f"nfs_stat={nfs_stat}")

    # LOOKUP hello.txt (must exist in the exported directory)
    test_filename = "hello.txt"
    args = fh_arg + xdr_string(test_filename)
    xid, pkt = rpc_call(100003, 3, 3, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    off += 4
    if nfs_stat != 0:
        fail(f"NFS3PROC_LOOKUP {test_filename}", f"nfs_stat={nfs_stat}")
        s.close()
        return
    ok(f"NFS3PROC_LOOKUP {test_filename}")

    # Extract file handle for hello.txt
    fh2_len = struct.unpack_from(">I", resp, off)[0]
    off += 4
    file_fh  = resp[off:off + fh2_len]
    file_fh_arg = xdr_opaque(file_fh)

    # GETATTR on the file
    xid, pkt = rpc_call(100003, 3, 1, file_fh_arg)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    off += 4
    if nfs_stat == 0:
        ftype = struct.unpack_from(">I", resp, off)[0]
        size  = struct.unpack_from(">Q", resp, off + 20)[0]
        ok(f"NFS3PROC_GETATTR {test_filename} (ftype={ftype} size={size})")
    else:
        fail(f"NFS3PROC_GETATTR {test_filename}", f"nfs_stat={nfs_stat}")

    # READ 64 bytes at offset 0
    args = file_fh_arg + u64(0) + u32(64)
    xid, pkt = rpc_call(100003, 3, 6, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    off += 4
    if nfs_stat == 0:
        # skip post_op_attr(1+84 or 4), count, eof, data_len, data
        present = struct.unpack_from(">I", resp, off)[0]
        off += 4 + (84 if present else 0)
        nread = struct.unpack_from(">I", resp, off)[0]
        eof   = struct.unpack_from(">I", resp, off + 4)[0]
        dlen  = struct.unpack_from(">I", resp, off + 8)[0]
        data  = resp[off + 12: off + 12 + dlen]
        ok(f"NFS3PROC_READ {test_filename} nread={nread} eof={eof} "
           f"data={data[:20]!r}")
    else:
        fail(f"NFS3PROC_READ {test_filename}", f"nfs_stat={nfs_stat}")

    # CREATE a new file
    new_name = f"nfsd_test_{os.getpid()}.tmp"
    sattr3_null = (u32(0) + u32(0) + u32(0) + u32(0) +   # no mode/uid/gid/size
                   u32(0) + u32(0))                        # atime/mtime: DONT_CHANGE
    args = fh_arg + xdr_string(new_name) + u32(0) + sattr3_null  # UNCHECKED
    xid, pkt = rpc_call(100003, 3, 8, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    off += 4
    if nfs_stat == 0:
        ok(f"NFS3PROC_CREATE {new_name}")
        # Get file handle for created file
        fh3_present = struct.unpack_from(">I", resp, off)[0]
        if fh3_present:
            off += 4
            fh3_len = struct.unpack_from(">I", resp, off)[0]
            off += 4
            new_fh = resp[off:off + fh3_len]
            new_fh_arg = xdr_opaque(new_fh)

            # WRITE some data to it
            write_data = b"Hello from nfsd_test!\n"
            args = (new_fh_arg + u64(0) + u32(len(write_data)) +
                    u32(2) +    # FILE_SYNC
                    xdr_opaque(write_data))
            xid, pkt = rpc_call(100003, 3, 7, args)
            resp = send_recv(s, pkt)
            _, stat, off2 = parse_reply_hdr(resp)
            nfs_stat = struct.unpack_from(">I", resp, off2)[0]
            if nfs_stat == 0:
                ok(f"NFS3PROC_WRITE {new_name}")
            else:
                fail(f"NFS3PROC_WRITE {new_name}", f"nfs_stat={nfs_stat}")

            # REMOVE the test file
            args = fh_arg + xdr_string(new_name)
            xid, pkt = rpc_call(100003, 3, 12, args)
            resp = send_recv(s, pkt)
            _, stat, off2 = parse_reply_hdr(resp)
            nfs_stat = struct.unpack_from(">I", resp, off2)[0]
            if nfs_stat == 0:
                ok(f"NFS3PROC_REMOVE {new_name}")
            else:
                fail(f"NFS3PROC_REMOVE {new_name}", f"nfs_stat={nfs_stat}")
    else:
        fail(f"NFS3PROC_CREATE {new_name}", f"nfs_stat={nfs_stat}")

    # MKDIR should return NOTSUPP
    args = fh_arg + xdr_string("should_not_exist") + sattr3_null
    xid, pkt = rpc_call(100003, 3, 9, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    if nfs_stat == 10004:    # NFS3ERR_NOTSUPP
        ok("NFS3PROC_MKDIR -> NOTSUPP (correct)")
    else:
        fail("NFS3PROC_MKDIR NOTSUPP check", f"nfs_stat={nfs_stat}")

    # COMMIT
    args = file_fh_arg + u64(0) + u32(0)
    xid, pkt = rpc_call(100003, 3, 21, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    nfs_stat = struct.unpack_from(">I", resp, off)[0]
    if nfs_stat == 0:
        ok("NFS3PROC_COMMIT")
    else:
        fail("NFS3PROC_COMMIT", f"nfs_stat={nfs_stat}")

    s.close()

# ------------------------------------------------------------------ #
# Run                                                                  #
# ------------------------------------------------------------------ #
print(f"nfsd_test.py  host={HOST}  pmap={PORT_PMAP}  "
      f"mount={PORT_MOUNT}  nfs={PORT_NFS}")
print(f"export under test: {EXPORT_PATH}")

# ------------------------------------------------------------------ #
# Test: RENAME                                                         #
# ------------------------------------------------------------------ #
def test_rename():
    if ROOT_FH is None:
        print("\n=== RENAME (SKIPPED) ===")
        return
    print("\n=== RENAME ===")
    s = conn(PORT_NFS)
    fh = xdr_opaque(ROOT_FH)
    sattr3_null = u32(0)*6

    # Create source file
    args = fh + xdr_string("rename_src.tmp") + u32(0) + sattr3_null
    xid, pkt = rpc_call(100003, 3, 8, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    ns = struct.unpack_from(">I", resp, off)[0]
    if ns != 0:
        fail("RENAME setup CREATE", f"nfs_stat={ns}")
        s.close(); return
    ok("RENAME setup: CREATE rename_src.tmp")

    # RENAME src -> dst
    args = fh + xdr_string("rename_src.tmp") + fh + xdr_string("rename_dst.tmp")
    xid, pkt = rpc_call(100003, 3, 14, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    ns = struct.unpack_from(">I", resp, off)[0]
    if ns == 0:
        ok("NFS3PROC_RENAME rename_src.tmp -> rename_dst.tmp")
    else:
        fail("NFS3PROC_RENAME", f"nfs_stat={ns}")
        s.close(); return

    # LOOKUP src must be NOENT
    args = fh + xdr_string("rename_src.tmp")
    xid, pkt = rpc_call(100003, 3, 3, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    ns = struct.unpack_from(">I", resp, off)[0]
    if ns == 2:   # NFS3ERR_NOENT
        ok("LOOKUP old name after rename -> NOENT (correct)")
    else:
        fail("LOOKUP old name after rename", f"nfs_stat={ns} (want 2=NOENT)")

    # LOOKUP dst must succeed
    args = fh + xdr_string("rename_dst.tmp")
    xid, pkt = rpc_call(100003, 3, 3, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    ns = struct.unpack_from(">I", resp, off)[0]
    if ns == 0:
        ok("LOOKUP new name after rename -> OK (correct)")
    else:
        fail("LOOKUP new name after rename", f"nfs_stat={ns}")

    # Cleanup REMOVE
    args = fh + xdr_string("rename_dst.tmp")
    xid, pkt = rpc_call(100003, 3, 12, args)
    resp = send_recv(s, pkt)
    _, stat, off = parse_reply_hdr(resp)
    ns = struct.unpack_from(">I", resp, off)[0]
    if ns == 0:
        ok("RENAME cleanup: REMOVE rename_dst.tmp")
    else:
        fail("RENAME cleanup REMOVE", f"nfs_stat={ns}")
    s.close()

try:
    test_portmapper()
    test_mount()
    test_nfs3()
    test_rename()
except ConnectionRefusedError:
    print("\nERROR: connection refused -- is the server running?")
    print("  ./nfsd -p 11111 -m 12048 -n 12049 nfsd.conf")
    sys.exit(1)
except Exception as e:
    print(f"\nERROR: {e}")
    import traceback; traceback.print_exc()
    sys.exit(1)

print(f"\n{'='*50}")
print(f"Results: {PASS} passed, {FAIL} failed")
sys.exit(0 if FAIL == 0 else 1)
