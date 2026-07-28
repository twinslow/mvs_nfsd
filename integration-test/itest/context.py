"""
Context -- the single object every test receives.

It hides the mode-specific bits (mount layout, MVS member-name mapping, the
verification backend) behind a small helper API, so a test body reads as plain
file operations plus verify() calls and never branches on mvs/plain.
"""

import os
import time

from pathlib import Path

from . import textutil
from .runner import TestSkip


class Context(object):
    def __init__(self, cfg, backend, workdir):
        self.cfg = cfg
        self.mode = cfg["mode"]
        self.mount_root = Path(cfg["nfs"]["mount_point"])
        self.backend = backend
        self.workdir = Path(workdir)
        self.opts = cfg["options"]
        self._to_clean = []          # (key, name) created during a test
        self._preserve = set()       # (key, name) to KEEP for post-mortem
        self._failed = False         # set by the runner when a test fails
        self._pending_verify = []    # (key, name, text) awaiting the drain
        self._dirty_since_settle = True   # an NFS write may hold a slot

    # -- dataset / path mapping -------------------------------------------
    def ds(self, key):
        return self.cfg["datasets"][key]

    def keys(self):
        return list(self.cfg["datasets"].keys())

    def pds_dir(self, key):
        return self.mount_root / self.ds(key)["nfs_dir"]

    def filename(self, key, name):
        ext = self.ds(key).get("ext", "")
        return name if not ext else "%s.%s" % (name, ext)

    def member_path(self, key, name):
        return self.pds_dir(key) / self.filename(key, name)

    def mvs_name(self, name):
        return name.upper()[:8]

    def is_vb(self, key):
        return str(self.ds(key).get("recfm", "FB")).upper().startswith("V")

    # -- data generation --------------------------------------------------
    def gen(self, key, kind):
        """kind: 'small' or 'large'.  Returns text sized for the dataset RECFM.
        'large' deliberately exceeds the server's in-memory / spill thresholds."""
        n = 6 if kind == "small" else int(self.opts["large_lines"])
        if self.is_vb(key):
            return textutil.gen_var_lines(n, base=20, tag="ITV")
        return textutil.gen_fixed_lines(n, int(self.opts["line_width"]), tag="ITF")

    # -- NFS-side file IO (always LF line endings on write) ---------------
    def write_member(self, key, name, text, track=True):
        p = self.member_path(key, name)
        self._dirty_since_settle = True    # may leave a write slot held
        with open(str(p), "w", newline="\n") as f:
            f.write(text)
            self._sync(f)
        if track:
            self._to_clean.append((key, name))

    def touch_member(self, key, name, track=True):
        """Create an empty member (the 'touch' case), synced like a write."""
        p = self.member_path(key, name)
        self._dirty_since_settle = True    # may leave a write slot held
        with open(str(p), "w", newline="\n") as f:
            self._sync(f)
        if track:
            self._to_clean.append((key, name))

    def _sync(self, f):
        """Flush, and OPTIONALLY fsync.

        fsync is OFF by default (options.use_fsync).  It is not needed: the
        poll/settle helpers below already wait for the server's idle-sweep
        flush.  But on Windows it is far from free -- a FILE_SYNC client never
        sends NFS COMMIT on its own, so fsync makes it start, which drags the
        server's synchronous flush path into every write.  That is a real change
        in server behaviour and deserves to be an explicit, separately testable
        choice rather than a silent side effect of the harness."""
        try:
            f.flush()
        except (OSError, ValueError):
            pass
        if not self.opts.get("use_fsync", False):
            return
        try:
            os.fsync(f.fileno())
        except (OSError, ValueError):
            pass

    # -- synchronising with the server's deferred flush --------------------
    #
    # A member written over NFS is buffered in the pending-member pool and only
    # reaches the PDS on COMMIT or the idle sweep; until the slot is released the
    # server also still holds the allocation + SPFEDIT enqueue.  So an FTP check
    # fired immediately after a write legitimately sees "no such member" (550) or
    # cannot open the dataset.  These helpers wait that window out.

    def settle(self):
        """Wait until the server has certainly released its write slots.

        Needed before an OUT-OF-BAND FTP write, which wants exclusive access
        to the PDS while the server may still hold an allocation + SPFEDIT
        enqueue from a recent NFS write.

        The sleep is SKIPPED when nothing has been written over NFS since the
        last settle, because only a write creates a pending slot -- reads,
        stats and deletes do not.  That matters in test 2.1, which settles
        once per iteration but writes over NFS only when FTP preparation is
        unavailable: without this it paid the full wait four times over for
        three settles that had nothing to wait for.

        Deliberately conservative: any NFS write re-arms the wait, so the
        exclusive-access guarantee is unchanged -- it is only the redundant
        sleeps that go."""
        if not self._dirty_since_settle:
            return
        time.sleep(float(self.opts.get("settle_sec", 5)))
        self._dirty_since_settle = False

    def _deadline(self):
        return time.time() + float(self.opts.get("sync_timeout_sec", 12))

    def wait_visible(self, key, name):
        """Poll the NFS side until the member appears.

        Needed after OUT-OF-BAND preparation (an FTP STOR): the server only
        notices a directory change it did not make on a throttled schedule
        (DIR_REFRESH_THROTTLE_SECS, 10s), and until dir_mtime moves the client
        keeps serving its cached listing.  So 'created over FTP' and 'visible
        over NFS' are seconds apart, by design."""
        end = self._deadline()
        d = str(self.pds_dir(key))
        p = str(self.member_path(key, name))
        while True:
            # 1. Nudge the DIRECTORY.  The server runs its out-of-band change
            #    detection inside the directory's own stat/readdir, so a member
            #    created by FTP needs this to become visible at all -- a bare
            #    lookup can be answered from the client's cached negative entry
            #    forever and the detection never fires.  The result is ignored
            #    (Windows may serve the enumeration from its own cache).
            try:
                os.listdir(d)
            except OSError:
                pass
            # 2. Judge by actually OPENING it.  This is the authoritative check
            #    and covers both cases: a member still pending in the write pool
            #    (readable via the pool, but NOT yet in the PDS directory, so
            #    invisible to listdir) and one already stowed.
            try:
                with open(p, "r"):
                    return True
            except OSError:
                pass
            if time.time() >= end:
                return False
            time.sleep(1.0)

    def wait_mtime(self, key, name, target, tol):
        """Poll until the member's mtime reaches 'target' (within tol).

        Distinguishes two very different outcomes: a value that settles late is
        attribute caching, whereas one that never arrives means the SETATTR was
        not applied at all.  Returns (ok, last_seen)."""
        end = self._deadline()
        last = None
        while True:
            try:
                last = os.stat(str(self.member_path(key, name))).st_mtime
                if abs(last - target) <= tol:
                    return True, last
            except OSError:
                pass
            if time.time() >= end:
                return False, last
            time.sleep(0.5)

    def wait_exists(self, key, name, want=True):
        """Poll the backend until the member's existence matches 'want'.
        Returns True if it did, False on timeout."""
        end = self._deadline()
        while True:
            try:
                if self.backend.exists(self, key, name) == want:
                    return True
            except Exception:            # noqa -- dataset briefly unopenable
                pass
            if time.time() >= end:
                return False
            time.sleep(0.5)

    def read_member(self, key, name):
        """Read a member over NFS.

        Deliberately BINARY + latin-1, not text mode.  A corrupt member can
        contain EBCDIC or raw RPC-wire bytes, and a text-mode read then dies
        with UnicodeDecodeError deep inside the test -- which both loses the
        evidence and destroys the member (teardown only preserves on a content
        mismatch).  latin-1 maps bytes 1:1 onto U+0000-U+00FF, so the data
        survives byte-exact for diffing and hex dumping, and the corruption
        surfaces as a normal mismatch that the post-mortem can report on."""
        with open(str(self.member_path(key, name)), "rb") as f:
            raw = f.read()
        return raw.decode("latin-1")

    def track(self, key, name):
        self._to_clean.append((key, name))

    def remove_member(self, key, name):
        try:
            self.member_path(key, name).unlink()
        except OSError:
            pass

    def reset(self, key, *names):
        """Delete members so a test is idempotent across reruns."""
        for n in names:
            self.remove_member(key, n)

    # -- setup / verification helpers -------------------------------------
    def ensure_dirs(self):
        """Plain mode only: create the per-dataset directories under the mount.
        MVS PDSs are pre-created by the JCL and cannot be made via NFS MKDIR."""
        if self.mode != "plain":
            return
        for key in self.keys():
            self.pds_dir(key).mkdir(parents=True, exist_ok=True)

    def check(self, cond, msg):
        if not cond:
            raise AssertionError(msg)

    def verify_member(self, key, name, expected_text):
        """Queue a backend verification for the END of the test.

        Verification is DEFERRED rather than performed here, because the
        server flushes a member to the PDS only on COMMIT or the idle sweep.
        Checking immediately after each write means paying that flush latency
        once per member, serially -- and every premature fetch costs more than
        a wait: a 550 leaves this FTP server's control connection out of step,
        so the backend drops and re-logs-in the session (see _reset).  A test
        that writes five members therefore paid five flush waits and up to
        five reconnects.

        Deferring means the writes all happen first, so by the time the batch
        is drained the earlier members are usually already stowed: one wait
        covers them all and the connection is reused.

        Safe because every verify_member call in the suite is the last
        operation on that member -- nothing renames or deletes it afterwards.
        Where an immediate check IS required (asserting a member is readable
        before the test does something else to it), call verify_now()."""
        self._pending_verify.append((key, name, expected_text))

    def verify_now(self, key, name, expected_text):
        """Verify immediately, bypassing the queue.  Use only when the test
        depends on the check having happened before it continues."""
        self._verify_one(key, name, expected_text, self._deadline())

    def drain_verifications(self):
        """Run every queued verification.  Called by the runner once the test
        body returns, before teardown deletes anything.

        The first member absorbs the flush wait; the rest are normally
        readable straight away, which is the point of batching."""
        pending = self._pending_verify
        self._pending_verify = []
        for key, name, expected_text in pending:
            self._verify_one(key, name, expected_text, self._deadline())

    def _verify_one(self, key, name, expected_text, end):
        """Assert the backend's copy of the member matches expected_text.

        Retries while the member is not yet fetchable: right after an NFS write
        the flush may not have happened, so the first fetch can legitimately
        fail with 550.  Once it IS fetchable a content mismatch fails at once --
        we only retry the "not there yet" case, never a wrong-content one."""
        last = None
        while True:
            try:
                actual = self.backend.fetch(self, key, name)
            except Exception as e:           # noqa -- not stowed / not openable
                last = e
                if time.time() >= end:
                    raise AssertionError(
                        "member %s(%s) never became readable over the"
                        " verification backend within %ss (last error: %s). The"
                        " server buffers writes until COMMIT or the idle sweep"
                        " -- if this persists, the flush is not happening."
                        % (self.ds(key)["nfs_dir"], name,
                           self.opts.get("sync_timeout_sec", 12), last))
                time.sleep(0.5)
                continue

            # Fetch SUCCEEDED.  A content mismatch now is a real defect, not a
            # timing issue -- do not retry it; capture evidence and preserve the
            # member so it can be inspected on the server (see _corruption).
            d = textutil.diff_summary(expected_text, actual)
            if not d:
                return
            self._preserve.add((key, name))
            raise AssertionError("member %s(%s) mismatch: %s\n%s"
                                 % (self.ds(key)["nfs_dir"], name, d,
                                    self._corruption(key, name, expected_text, actual)))

    def _corruption(self, key, name, expected, actual):
        """Build a post-mortem for a content mismatch: the exact byte offset,
        a hex window from both sides, and an independent NFS read-back to say
        whether the on-disk member is corrupt or only the fetch path is."""
        off = textutil.first_byte_diff(expected, actual)
        lines = []
        lines.append("  first byte diff:")
        lines.append("    expected: " + textutil.hexdump_around(expected, off))
        lines.append("    fetched : " + textutil.hexdump_around(actual, off))

        # MVS FTP reads the PDS member straight off disk, bypassing dino-nfs, so
        # a corrupt fetch already means the on-disk member is corrupt (the write
        # path).  Cross-read via NFS too -- if it ALSO differs the member is
        # definitely bad; if it matches, suspect the fetch/download path.  (NFS
        # client caching can mask a bad disk here, so FTP remains authoritative.)
        try:
            nfs = self.read_member(key, name)
            if textutil.first_byte_diff(expected, nfs) < 0:
                lines.append("  NFS read-back MATCHES (fetch path suspect; but "
                             "FTP reads disk directly, so more likely a caching "
                             "artifact)")
            else:
                lines.append("  NFS read-back ALSO differs -> the PDS member on "
                             "disk is corrupt (server WRITE/flush path)")
        except Exception as e:                       # noqa
            lines.append("  NFS read-back failed: %s" % e)

        dsname = self.ds(key).get("dsname", "?")
        lines.append("  member PRESERVED for inspection: ISPF browse %s(%s) on "
                     "MVS (or FTP GET) to see the raw record" % (dsname, self.mvs_name(name)))
        return "\n".join(lines)

    def require_mvs(self):
        if self.mode != "mvs":
            raise TestSkip("MVS-only")

    # -- per-test teardown ------------------------------------------------
    def mark_failed(self):
        """Called by the runner when a test raises.  Every member the test
        touched is then kept, not just one a verify_member call flagged: a
        failure anywhere (a bad read, an exception mid-test) is exactly when
        the on-disk evidence matters, and it is unrecoverable once deleted."""
        self._failed = True

    def after_test(self):
        for key, name in self._to_clean:
            if self._failed or (key, name) in self._preserve:
                self._preserve.add((key, name))   # keep for post-mortem
                continue
            self.remove_member(key, name)
        if self._preserve:
            kept = ", ".join("%s(%s)" % (self.ds(k)["nfs_dir"], n)
                             for k, n in sorted(self._preserve))
            print("      NOTE: preserved for inspection (NOT deleted): %s" % kept)
            print("            re-run deletes them at that test's start, so look now")
        self._to_clean = []
        self._preserve = set()
        self._failed   = False
        self._pending_verify = []
