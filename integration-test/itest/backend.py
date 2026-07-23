"""
Verification backends -- the out-of-band "source of truth" for a member.

A test performs an operation through the OS NFS mount, then asks the backend
what actually landed in the PDS:

  MvsFtpBackend  -- the real MVS case: prepare/read members over FTP (text mode),
                    fully independent of the NFS path under test.  Also used for
                    "server preparation" (FTP upload).
  PlainBackend   -- the non-MVS case (e.g. a Linux NFS server): read the member
                    back through the same mount.  Less independent, but enough to
                    prove the harness and the applicable operations work.

Both expose the same small interface, so the tests never branch on mode.
"""

import io


class Backend(object):
    def prepare(self, ctx, key, name, text):
        raise NotImplementedError

    def fetch(self, ctx, key, name):
        raise NotImplementedError

    def exists(self, ctx, key, name):
        raise NotImplementedError

    def members(self, ctx, key):
        raise NotImplementedError

    def close(self):
        pass


class PlainBackend(Backend):
    """Reads/writes members directly through the NFS mount path."""

    def prepare(self, ctx, key, name, text):
        p = ctx.member_path(key, name)
        p.parent.mkdir(parents=True, exist_ok=True)
        with open(str(p), "w", newline="\n") as f:
            f.write(text)

    def fetch(self, ctx, key, name):
        with open(str(ctx.member_path(key, name)), "r") as f:
            return f.read()

    def exists(self, ctx, key, name):
        return ctx.member_path(key, name).exists()

    def members(self, ctx, key):
        d = ctx.pds_dir(key)
        if not d.is_dir():
            return set()
        return set(ctx.mvs_name(p.stem if ctx.ds(key).get("ext") else p.name)
                   for p in d.iterdir() if p.is_file())


class MvsFtpBackend(Backend):
    """Reaches the PDS over MVS FTP (independent of the NFS server under test)."""

    def __init__(self, host, port, user, password):
        self.host, self.port = host, port
        self.user, self.password = user, password
        self.ftp = None

    def _conn(self):
        import ftplib
        if self.ftp is not None:
            try:
                self.ftp.voidcmd("NOOP")
                return self.ftp
            except Exception:
                self.ftp = None
        ftp = ftplib.FTP()
        ftp.connect(self.host, self.port, timeout=30)
        ftp.login(self.user, self.password)
        ftp.voidcmd("TYPE A")
        self.ftp = ftp
        return ftp

    def _cd(self, ctx, key):
        ftp = self._conn()
        ftp.voidcmd("TYPE A")
        ftp.cwd("'%s'" % ctx.ds(key)["dsname"])
        return ftp

    def prepare(self, ctx, key, name, text):
        ftp = self._cd(ctx, key)
        body = "\r\n".join(text.replace("\r\n", "\n").split("\n"))
        if not body.endswith("\r\n"):
            body += "\r\n"
        ftp.storlines("STOR %s" % ctx.mvs_name(name),
                      io.BytesIO(body.encode("latin-1", "replace")))

    def fetch(self, ctx, key, name):
        ftp = self._cd(ctx, key)
        lines = []
        ftp.retrlines("RETR %s" % ctx.mvs_name(name), lines.append)
        return "\n".join(lines) + "\n"

    def exists(self, ctx, key, name):
        return ctx.mvs_name(name) in self.members(ctx, key)

    def members(self, ctx, key):
        import ftplib
        ftp = self._cd(ctx, key)
        names = []
        try:
            ftp.retrlines("NLST", names.append)
        except ftplib.error_perm:
            return set()   # 550 = empty / no members
        # NLST may return fully-qualified names; keep only the member part.
        out = set()
        for n in names:
            n = n.strip()
            if not n:
                continue
            if "(" in n and n.endswith(")"):
                n = n[n.index("(") + 1:-1]
            out.add(ctx.mvs_name(n))
        return out

    def close(self):
        if self.ftp is not None:
            try:
                self.ftp.quit()
            except Exception:
                pass
            self.ftp = None


def build(cfg):
    if cfg["mode"] == "mvs":
        from . import config as _cfg
        user = _cfg.resolve_ftp_user(cfg)
        pw = _cfg.resolve_ftp_password(cfg)
        f = cfg["ftp"]
        return MvsFtpBackend(f["host"], f.get("port", 21), user, pw)
    return PlainBackend()
