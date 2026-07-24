#!/usr/bin/env python3
"""
dino-nfs automated integration test runner.

Drives file operations through the OS NFS client against a mounted dino-nfs
export (or any NFS server) and verifies the results out-of-band -- over FTP for
a real MVS server, or by reading the member back for a plain NFS server.

Usage:
    python run_tests.py --config config.json
    python run_tests.py --config config.json --list
    python run_tests.py --config config.json --section 1 --section 2
    python run_tests.py --config config.json --filter zip --filter rename

See README.md for setup (dataset JCL, mounting, config).
"""

import argparse
import os
import sys
import tempfile

# Allow running as "python run_tests.py" from this directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from itest import config as cfgmod          # noqa: E402
from itest import backend as backendmod      # noqa: E402
from itest import nfsmount                    # noqa: E402
from itest.context import Context             # noqa: E402
from itest import runner                      # noqa: E402
from itest import cases                       # noqa: F401,E402  (import to register tests)


def main(argv=None):
    ap = argparse.ArgumentParser(description="dino-nfs integration tests")
    ap.add_argument("--config", "-c", required=True, help="path to the JSON config file")
    ap.add_argument("--list", "-l", action="store_true", help="list the tests and exit")
    ap.add_argument("--section", "-s", action="append", default=[],
                    help="run only this outline section (e.g. 1, 1.2); repeatable")
    ap.add_argument("--filter", "-f", action="append", default=[],
                    help="run only tests whose name contains this substring; repeatable")
    ap.add_argument("--probe-ftp", action="store_true",
                    help="diagnose which dataset-naming dialect the MVS FTP "
                         "server accepts, print the matching config, and exit")
    ap.add_argument("--repeat", "-r", type=int, default=1, metavar="N",
                    help="run the selected tests up to N times, STOPPING at the "
                         "first failure (leaves the corrupt member preserved) -- "
                         "for catching intermittent bugs, e.g. "
                         "-f upload_small -f upload_large -r 40")
    args = ap.parse_args(argv)

    if args.list:
        for e in sorted(runner.REGISTRY, key=lambda x: [int(i) for i in x["section"].split(".")]):
            req = "  (mvs-only)" if e["requires"] == "mvs" else ""
            print("[%-4s] %s%s" % (e["section"], e["name"], req))
        return 0

    cfg = cfgmod.load(args.config)

    if args.probe_ftp:
        if cfg["mode"] != "mvs":
            print("--probe-ftp only applies to mode 'mvs'")
            return 1
        return backendmod.probe_ftp(cfg)

    print("dino-nfs integration tests  |  mode=%s  mount=%s"
          % (cfg["mode"], cfg["nfs"]["mount_point"]))

    nfsmount.ensure_available(cfg)
    backend = backendmod.build(cfg)

    rc = 1
    workdir = tempfile.mkdtemp(prefix="dinonfs-itest-")
    try:
        ctx = Context(cfg, backend, workdir)
        ctx.ensure_dirs()                      # plain mode: create dataset dirs
        rc = runner.run_all(ctx, filters=args.filter, sections=args.section,
                             repeat=args.repeat)
    finally:
        backend.close()
        nfsmount.cleanup(cfg)
        try:
            import shutil
            shutil.rmtree(workdir, ignore_errors=True)
        except Exception:
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
