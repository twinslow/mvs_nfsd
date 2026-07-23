"""
Configuration loading for the dino-nfs integration tests.

The config is JSON (stdlib only -- no third-party YAML dependency), so the
harness runs on a bare Python 3 install on Linux or Windows.  See
config.example.json (MVS) and config.plain.example.json (non-MVS) for the
full shape and per-field notes.
"""

import json
import os
import sys


DEFAULTS = {
    "mode": "mvs",
    "options": {"large_lines": 1500, "line_width": 72, "mtime_tolerance_sec": 120},
}


def _require(d, path):
    cur = d
    for key in path.split("."):
        if not isinstance(cur, dict) or key not in cur:
            raise ValueError("config: missing required key '%s'" % path)
        cur = cur[key]
    return cur


def load(path):
    """Load, validate, and default-fill the config file at 'path'."""
    with open(path, "r") as f:
        cfg = json.load(f)

    cfg.setdefault("mode", DEFAULTS["mode"])
    if cfg["mode"] not in ("mvs", "plain"):
        raise ValueError("config: mode must be 'mvs' or 'plain'")

    _require(cfg, "nfs.mount_point")
    _require(cfg, "datasets")
    if not cfg["datasets"]:
        raise ValueError("config: 'datasets' is empty")

    for key, ds in cfg["datasets"].items():
        if "nfs_dir" not in ds:
            raise ValueError("config: datasets.%s missing 'nfs_dir'" % key)
        ds.setdefault("ext", "txt")
        ds.setdefault("recfm", "FB")
        ds.setdefault("lrecl", 80)
        if cfg["mode"] == "mvs" and "dsname" not in ds:
            raise ValueError("config: datasets.%s missing 'dsname' (needed in mvs mode)" % key)

    opts = DEFAULTS["options"].copy()
    opts.update(cfg.get("options", {}))
    cfg["options"] = opts

    if cfg["mode"] == "mvs":
        _require(cfg, "ftp.host")
        cfg["ftp"].setdefault("port", 21)
        cfg["ftp"].setdefault("user", None)
        cfg["ftp"].setdefault("password", None)

    return cfg


def resolve_ftp_user(cfg):
    """FTP userid precedence: config.ftp.user -> $MVS_USERID -> prompt.

    Credentials really should NOT live in config.json; the environment
    variables (the same ones the mvs_upload/download expect scripts use) are the
    intended source."""
    user = cfg.get("ftp", {}).get("user")
    if user:
        return user
    user = os.environ.get("MVS_USERID")
    if user:
        return user
    if not sys.stdin.isatty():
        raise ValueError("no FTP userid: set $MVS_USERID (preferred) or ftp.user, or run interactively")
    return input("TSO userid: ").strip()


def resolve_ftp_password(cfg):
    """FTP password precedence: config.ftp.password -> $MVS_PASSWORD -> prompt."""
    pw = cfg.get("ftp", {}).get("password")
    if pw:
        return pw
    pw = os.environ.get("MVS_PASSWORD")
    if pw:
        return pw
    if not sys.stdin.isatty():
        raise ValueError("no FTP password: set $MVS_PASSWORD (preferred) or ftp.password, or run interactively")
    import getpass
    return getpass.getpass("FTP password: ")
