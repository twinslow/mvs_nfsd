"""
OS NFS-client mount / unmount helpers (Linux and Windows).

Mounting NFS needs the OS NFS client and usually elevated privileges, so it is
the most environment-specific part of the harness.  By default the harness does
NOT mount for you (nfs.auto_mount = false): you mount the export yourself and
point nfs.mount_point at it.  Set auto_mount = true to have the harness run the
platform mount command (best effort) before the run and unmount after.

Either way the test operations only ever touch nfs.mount_point, so a manually
mounted path and an auto-mounted one behave identically.
"""

import os
import subprocess
import sys


def is_windows():
    return os.name == "nt"


def _win_target(mount_point):
    """Drive form the Windows mount/umount commands expect: 'Z:' not 'Z:\\'.

    The config keeps the trailing separator ('Z:\\') because os.path/pathlib need
    it to mean the drive ROOT ('Z:' alone means 'current dir on Z:'), but the
    mount tools reject/misread that trailing separator."""
    stripped = mount_point.rstrip("\\/")
    return stripped if stripped else mount_point


def ensure_available(cfg):
    """Verify the mount point exists (and, if auto, mount it).  Returns nothing;
    raises RuntimeError with an actionable message on failure."""
    nfs = cfg["nfs"]
    mp = nfs["mount_point"]

    if nfs.get("auto_mount"):
        _mount(cfg)

    if not os.path.isdir(mp):
        raise RuntimeError(
            "mount point %r is not an accessible directory.\n"
            "Mount the NFS export there first (or set nfs.auto_mount=true).\n"
            "  Linux:   sudo mount -t nfs -o %s %s %s\n"
            "  Windows: mount -o %s %s %s"
            % (mp,
               nfs.get("mount_options_linux", "vers=3,proto=tcp,nolock"),
               nfs.get("export", "<host>:/<export>"), mp,
               nfs.get("mount_options_windows", "anon,nolock"),
               nfs.get("windows_export", "\\\\<host>\\<export>"), _win_target(mp)))


def _mount(cfg):
    nfs = cfg["nfs"]
    mp = nfs["mount_point"]
    if is_windows():
        export = nfs.get("windows_export")
        opts = nfs.get("mount_options_windows", "anon,nolock")
        if not export:
            raise RuntimeError("auto_mount on Windows needs nfs.windows_export (\\\\host\\export)")
        cmd = ["mount", "-o", opts, export, _win_target(mp)]
    else:
        export = nfs.get("export")
        opts = nfs.get("mount_options_linux", "vers=3,proto=tcp,nolock,soft,timeo=30")
        if not export:
            raise RuntimeError("auto_mount on Linux needs nfs.export (host:/export)")
        if not os.path.isdir(mp):
            os.makedirs(mp, exist_ok=True)
        cmd = ["mount", "-t", "nfs", "-o", opts, export, mp]
        if nfs.get("use_sudo"):
            cmd = ["sudo"] + cmd
    _run(cmd, "mount")


def cleanup(cfg):
    """Unmount, only if the harness mounted it (auto_mount)."""
    nfs = cfg["nfs"]
    if not nfs.get("auto_mount"):
        return
    mp = nfs["mount_point"]
    # Both Services-for-NFS (Windows) and Linux provide `umount <target>`.
    cmd = ["umount", _win_target(mp) if is_windows() else mp]
    if not is_windows() and nfs.get("use_sudo"):
        cmd = ["sudo"] + cmd
    try:
        _run(cmd, "umount")
    except Exception as e:
        sys.stderr.write("warning: unmount failed: %s\n" % e)


def _run(cmd, what):
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except FileNotFoundError:
        raise RuntimeError("%s: command not found (%s). Is the OS NFS client installed?" % (what, cmd[0]))
    except subprocess.CalledProcessError as e:
        out = e.stdout.decode("utf-8", "replace") if e.stdout else ""
        raise RuntimeError("%s failed (exit %d): %s" % (what, e.returncode, out.strip()))
