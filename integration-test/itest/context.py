"""
Context -- the single object every test receives.

It hides the mode-specific bits (mount layout, MVS member-name mapping, the
verification backend) behind a small helper API, so a test body reads as plain
file operations plus verify() calls and never branches on mvs/plain.
"""

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
        with open(str(p), "w", newline="\n") as f:
            f.write(text)
        if track:
            self._to_clean.append((key, name))

    def read_member(self, key, name):
        with open(str(self.member_path(key, name)), "r") as f:
            return f.read()

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
        """Assert the backend's copy of the member matches expected_text."""
        actual = self.backend.fetch(self, key, name)
        d = textutil.diff_summary(expected_text, actual)
        if d:
            raise AssertionError("member %s(%s) mismatch: %s" % (self.ds(key)["nfs_dir"], name, d))

    def require_mvs(self):
        if self.mode != "mvs":
            raise TestSkip("MVS-only")

    # -- per-test teardown ------------------------------------------------
    def after_test(self):
        for key, name in self._to_clean:
            self.remove_member(key, name)
        self._to_clean = []
