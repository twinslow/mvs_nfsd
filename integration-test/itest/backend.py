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
    """Reaches the PDS over MVS FTP (independent of the NFS server under test).

    MVS FTP servers differ in how a dataset is named, so two independent knobs
    (ftp.use_cwd / ftp.quote_dsn) cover the four dialects seen in practice:

        use_cwd=true,  quote_dsn=true   CWD 'DSN'      then  RETR MEMBER
        use_cwd=true,  quote_dsn=false  CWD DSN        then  RETR MEMBER
        use_cwd=false, quote_dsn=true   RETR 'DSN(MEMBER)'
        use_cwd=false, quote_dsn=false  RETR DSN(MEMBER)

    The quoted-CWD default is the IBM z/OS convention; run
    `run_tests.py --probe-ftp` to find out what YOUR server accepts.
    """

    def __init__(self, host, port, user, password,
                 use_cwd=True, quote_dsn=True, dsn_prefix=""):
        self.host, self.port = host, port
        self.user, self.password = user, password
        self.use_cwd = use_cwd
        self.quote_dsn = quote_dsn
        self.dsn_prefix = dsn_prefix
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

    def _reset(self):
        """Drop the control connection.

        A failed data transfer (e.g. RETR of a member that is not stowed yet ->
        550) can leave this server's control connection out of step, after which
        every later command answers '500 Huh?'.  Reusing such a session poisons
        all subsequent operations -- including the retries that are supposed to
        wait for the flush -- so any failure discards it and the next call logs
        in again.  close() rather than quit(): QUIT needs a healthy session."""
        if self.ftp is not None:
            try:
                self.ftp.close()
            except Exception:
                pass
            self.ftp = None

    def _q(self, s):
        """Render a dataset name in this server's dialect.

        dsn_prefix="/" gives the path style ("CWD /TEMP.ITEST.FB") that
        mvs_upload.expect uses and that this server accepts for WRITES -- a
        quoted CWD may be good enough for RETR yet still refuse STOR."""
        s = self.dsn_prefix + s
        return "'%s'" % s if self.quote_dsn else s

    def _target(self, ctx, key, name):
        """Connect, position if needed, and return the RETR/STOR argument."""
        ftp = self._conn()
        ftp.voidcmd("TYPE A")
        dsn = ctx.ds(key)["dsname"]
        member = ctx.mvs_name(name)
        if self.use_cwd:
            ftp.cwd(self._q(dsn))
            return ftp, member
        return ftp, self._q("%s(%s)" % (dsn, member))

    def _list_arg(self, ctx, key):
        """Connect, position if needed, and return the NLST argument ('' = none)."""
        ftp = self._conn()
        ftp.voidcmd("TYPE A")
        dsn = ctx.ds(key)["dsname"]
        if self.use_cwd:
            ftp.cwd(self._q(dsn))
            return ftp, ""
        return ftp, " " + self._q(dsn)

    def prepare(self, ctx, key, name, text):
        try:
            ftp, target = self._target(ctx, key, name)
            body = "\r\n".join(text.replace("\r\n", "\n").split("\n"))
            if not body.endswith("\r\n"):
                body += "\r\n"
            ftp.storlines("STOR %s" % target,
                          io.BytesIO(body.encode("latin-1", "replace")))
        except Exception:
            self._reset()
            raise

    def fetch(self, ctx, key, name):
        try:
            ftp, target = self._target(ctx, key, name)
            lines = []
            ftp.retrlines("RETR %s" % target, lines.append)
            return "\n".join(lines) + "\n"
        except Exception:
            self._reset()
            raise

    def exists(self, ctx, key, name):
        """Existence via RETR, deliberately NOT via NLST.

        NLST is not reliable on every MVS FTP server (on at least one it fails
        outright), and members() answers a failed listing with an empty set --
        so an NLST-based exists() reports False forever and every caller just
        polls until it times out.  RETR is the path the fetch tests already
        prove works, so existence is 'can we read it'."""
        import ftplib
        try:
            self.fetch(ctx, key, name)
            return True
        except ftplib.error_perm:
            return False          # 550 = no such member

    def members(self, ctx, key):
        """Directory listing.  Diagnostic only -- exists() does NOT use this,
        because a server that rejects NLST would otherwise look like an empty
        dataset rather than an error."""
        import ftplib
        names = []
        try:
            ftp, arg = self._list_arg(ctx, key)
            ftp.retrlines("NLST" + arg, names.append)
        except ftplib.error_perm:
            self._reset()  # 550 = empty / no members, but the session is dirty
            return set()
        except Exception:
            self._reset()
            raise
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
                self._reset()
                return
            self.ftp = None


def build(cfg):
    if cfg["mode"] == "mvs":
        from . import config as _cfg
        user = _cfg.resolve_ftp_user(cfg)
        pw = _cfg.resolve_ftp_password(cfg)
        f = cfg["ftp"]
        return MvsFtpBackend(f["host"], f.get("port", 21), user, pw,
                             use_cwd=f.get("use_cwd", True),
                             quote_dsn=f.get("quote_dsn", True),
                             dsn_prefix=f.get("dsn_prefix", ""))
    return PlainBackend()


def probe_ftp(cfg):
    """Work out which dataset-naming dialect this MVS FTP server accepts.

    Tries all four combinations of ftp.use_cwd / ftp.quote_dsn against the first
    configured dataset and prints which succeed, so the right settings can be
    copied straight into config.json.  Returns 0 if at least one worked.
    """
    import ftplib
    from . import config as _cfg

    f    = cfg["ftp"]
    host = f["host"]
    port = f.get("port", 21)
    user = _cfg.resolve_ftp_user(cfg)
    pw   = _cfg.resolve_ftp_password(cfg)
    key  = sorted(cfg["datasets"])[0]
    dsn  = cfg["datasets"][key]["dsname"]

    print("Probing FTP %s:%s as %s, dataset %s" % (host, port, user, dsn))
    print("Each dialect is tested for READ (RETR-style) and WRITE (STOR) --")
    print("a server can accept one CWD form for reading yet refuse to write"
          " through it.\n")
    working = []

    def _login():
        ftp = ftplib.FTP()
        ftp.connect(host, port, timeout=30)
        ftp.login(user, pw)
        ftp.voidcmd("TYPE A")
        return ftp

    def _try(label, cfg_line, setup, action):
        ftp = None
        try:
            ftp = _login()
            setup(ftp)
            action(ftp)
            print("  OK    %s" % label)
            return cfg_line
        except Exception as e:
            print("  FAIL  %-34s %s" % (label, str(e).strip()))
            return None
        finally:
            if ftp is not None:
                try:
                    ftp.close()
                except Exception:
                    pass

    # (prefix, quote) -- the third is the path style mvs_upload.expect uses.
    forms = [("", True), ("", False), ("/", False)]

    for prefix, quote in forms:
        raw = prefix + dsn
        q = "'%s'" % raw if quote else raw
        cfg = ('"use_cwd": true, "quote_dsn": %s, "dsn_prefix": "%s"'
               % ("true" if quote else "false", prefix))

        def _cd(ftp, _q=q):
            ftp.cwd(_q)

        def _read(ftp):
            names = []
            try:
                ftp.retrlines("NLST", names.append)
            except ftplib.error_perm:
                pass                       # empty dataset still counts as read
            return names

        def _write(ftp):
            ftp.storlines("STOR ITPROBE",
                          io.BytesIO(b"ITPROBE\r\n"))

        r = _try("CWD %-24s READ" % q, cfg, _cd, _read)
        w = _try("CWD %-24s WRITE" % q, cfg, _cd, _write)
        if r and w:
            working.append(cfg)
        elif r:
            print("        (reads only -- STOR refused through this form)")

    print("")
    if not working:
        print("No dialect supported BOTH read and write.  If a read-only form"
              " worked, use it and let test 2.1 fall back to NFS preparation."
              "  Note mvs_upload.expect writes members using 'cd /<DSN>', so"
              " the path form is the one to expect here.")
        return 1
    print("Put this in the \"ftp\" section of config.json (read + write OK):")
    for w in working:
        print("    %s" % w)
    print("\n(A test member ITPROBE may have been left in %s -- delete it if"
          " you care.)" % dsn)
    return 0
