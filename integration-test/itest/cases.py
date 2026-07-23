"""
Integration test cases, numbered to match the outline in README.md.

Each test operates on the OS NFS mount (via ctx helpers) and then verifies the
result out-of-band through ctx.backend (FTP on MVS, readback on a plain server).
Members created during a test are tracked and removed by ctx.after_test().

Tests marked requires="mvs" are skipped automatically against a plain server
(they assert behaviour that only a real PDS exhibits: a full dataset, or the
rejection of an invalid member name).
"""

import os
import shutil
import threading
import time
import zipfile

from . import textutil
from .runner import testcase, TestSkip


def _have(ctx, *keys):
    """Skip unless every named dataset key is configured."""
    for k in keys:
        if k not in ctx.cfg["datasets"]:
            raise TestSkip("needs dataset '%s' in config" % k)


def _text_keys(ctx):
    """The fb/vb text datasets that are actually configured."""
    return [k for k in ("fb", "vb") if k in ctx.cfg["datasets"]]


# =====================================================================
# 1. Upload to FB and VB text PDS members
# =====================================================================

@testcase("1.1", "upload_small")
def upload_small(ctx):
    _have(ctx, *(_text_keys(ctx)[:1] or ["fb"]))
    for key in _text_keys(ctx):
        text = ctx.gen(key, "small")
        ctx.reset(key, "SMALL")
        ctx.write_member(key, "SMALL", text)
        ctx.verify_member(key, "SMALL", text)


@testcase("1.2", "upload_large")
def upload_large(ctx):
    for key in _text_keys(ctx):
        text = ctx.gen(key, "large")     # exceeds the server spill threshold
        ctx.reset(key, "LARGE")
        ctx.write_member(key, "LARGE", text)
        ctx.verify_member(key, "LARGE", text)


@testcase("1.3", "upload_full_dataset", requires="mvs")
def upload_full_dataset(ctx):
    ctx.require_mvs()
    _have(ctx, "small")
    key = "small"
    data = ctx.gen(key, "large")
    wrote, failed = 0, False
    for i in range(300):
        name = "FULL%03d" % i
        try:
            with open(str(ctx.member_path(key, name)), "w", newline="\n") as f:
                f.write(data)
            wrote += 1
        except OSError:
            failed = True
            break
    for i in range(wrote + 1):
        ctx.remove_member(key, "FULL%03d" % i)
    ctx.check(failed, "expected the small dataset to fill and a write to fail, "
                      "but wrote %d members without error" % wrote)


@testcase("1.4", "upload_concurrent")
def upload_concurrent(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    names = ["CONC%02d" % i for i in range(5)]
    ctx.reset(key, *names)
    for n in names:
        ctx.track(key, n)
    payloads = {n: ("OWNER %s\n" % n) + ctx.gen(key, "small") for n in names}
    errors = []

    def worker(n):
        try:
            with open(str(ctx.member_path(key, n)), "w", newline="\n") as f:
                f.write(payloads[n])
        except Exception as e:                       # noqa
            errors.append((n, repr(e)))

    threads = [threading.Thread(target=worker, args=(n,)) for n in names]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    ctx.check(not errors, "concurrent write errors: %s" % errors)
    for n in names:                                  # cross-talk shows as a mismatch
        ctx.verify_member(key, n, payloads[n])


@testcase("1.5", "upload_invalid_name", requires="mvs")
def upload_invalid_name(ctx):
    ctx.require_mvs()
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    ext = ctx.ds(key).get("ext", "txt")
    bad_ext = "zzz" if ext != "zzz" else "qqq"       # not this dataset's extension
    bad = ctx.pds_dir(key) / ("bad.%s" % bad_ext)
    raised = False
    try:
        with open(str(bad), "w", newline="\n") as f:
            f.write("x\n")
    except OSError:
        raised = True
    if not raised:
        try:
            bad.unlink()
        except OSError:
            pass
    ctx.check(raised, "creating a member with an unexpected extension should be rejected")


# =====================================================================
# 2. Download of FB and VB text PDS members
# =====================================================================

@testcase("2.1", "download_small_large")
def download_small_large(ctx):
    for key in _text_keys(ctx):
        for kind, name in (("small", "DLS"), ("large", "DLL")):
            text = ctx.gen(key, kind)
            ctx.reset(key, name)
            ctx.track(key, name)
            ctx.backend.prepare(ctx, key, name, text)     # server preparation (FTP upload)
            got = ctx.read_member(key, name)              # read back through NFS
            d = textutil.diff_summary(text, got)
            ctx.check(not d, "NFS download of %s/%s mismatch: %s" % (key, name, d))


@testcase("2.2", "upload_then_download")
def upload_then_download(ctx):
    for key in _text_keys(ctx):
        for kind, name in (("small", "UDS"), ("large", "UDL")):
            text = ctx.gen(key, kind)
            ctx.reset(key, name)
            ctx.write_member(key, name, text)             # NFS upload
            got = ctx.read_member(key, name)              # immediate NFS download
            d = textutil.diff_summary(text, got)
            ctx.check(not d, "round-trip of %s/%s mismatch: %s" % (key, name, d))
            ctx.verify_member(key, name, text)            # and independently


# =====================================================================
# 3. Create member via touch
# =====================================================================

@testcase("3", "touch_create")
def touch_create(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    name = "TOUCHED"
    ctx.reset(key, name)
    ctx.track(key, name)
    ctx.member_path(key, name).touch()
    ctx.check(ctx.backend.exists(ctx, key, name),
              "member should exist after touch")


# =====================================================================
# 4. Update existing member stats (touch preserves content, moves mtime)
# =====================================================================

@testcase("4", "update_stats")
def update_stats(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    name = "STATS"
    ctx.reset(key, name)
    text = ctx.gen(key, "small")
    ctx.write_member(key, name, text)
    target = time.time() - 3600.0                        # clearly different from "now"
    os.utime(str(ctx.member_path(key, name)), (target, target))
    st = os.stat(str(ctx.member_path(key, name)))
    tol = float(ctx.opts["mtime_tolerance_sec"])
    ctx.check(abs(st.st_mtime - target) <= tol,
              "mtime not updated (got %d, wanted %d within %ds -- a large gap can "
              "mean a server timezone problem)" % (st.st_mtime, target, tol))
    ctx.verify_member(key, name, text)                   # content unchanged


# =====================================================================
# 5. Delete member
# =====================================================================

@testcase("5.1", "delete_exists")
def delete_exists(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    name = "DELME"
    ctx.reset(key, name)
    ctx.write_member(key, name, ctx.gen(key, "small"), track=False)
    ctx.member_path(key, name).unlink()
    ctx.check(not ctx.backend.exists(ctx, key, name),
              "member should be gone after delete")


@testcase("5.2", "delete_missing")
def delete_missing(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    ctx.reset(key, "NOSUCH")
    raised = False
    try:
        ctx.member_path(key, "NOSUCH").unlink()
    except OSError:
        raised = True
    ctx.check(raised, "deleting a non-existent member should fail")


# =====================================================================
# 6. Rename member
# =====================================================================

@testcase("6.1", "rename_within_pds")
def rename_within_pds(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    a, b = "RENSRC", "RENDST"
    ctx.reset(key, a, b)
    ctx.track(key, a)
    ctx.track(key, b)
    text = ctx.gen(key, "small")
    ctx.write_member(key, a, text, track=False)
    os.rename(str(ctx.member_path(key, a)), str(ctx.member_path(key, b)))
    ctx.check(not ctx.backend.exists(ctx, key, a), "source should be gone after rename")
    ctx.verify_member(key, b, text)


@testcase("6.2", "rename_cross_pds")
def rename_cross_pds(ctx):
    _have(ctx, "fb", "fb2")
    src, dst, name = "fb", "fb2", "MVSRC"
    ctx.reset(src, name)
    ctx.reset(dst, name)
    ctx.track(src, name)
    ctx.track(dst, name)
    text = ctx.gen(src, "small")
    ctx.write_member(src, name, text, track=False)
    sp, dp = str(ctx.member_path(src, name)), str(ctx.member_path(dst, name))
    try:
        os.rename(sp, dp)
    except OSError:
        # Cross-dataset rename may be unsupported (EXDEV): fall back to the
        # copy+delete that /bin/mv does across filesystems.
        shutil.copyfile(sp, dp)
        os.unlink(sp)
    ctx.check(not ctx.backend.exists(ctx, src, name), "source should be gone after move")
    ctx.verify_member(dst, name, text)


# =====================================================================
# 7. Copy from local files to PDS
# =====================================================================

@testcase("7", "copy_local_to_pds")
def copy_local_to_pds(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    name = "CPYLOC"
    ctx.reset(key, name)
    ctx.track(key, name)
    text = ctx.gen(key, "small")
    local = ctx.workdir / "cpyloc.txt"
    with open(str(local), "w", newline="\n") as f:
        f.write(text)
    shutil.copyfile(str(local), str(ctx.member_path(key, name)))
    ctx.verify_member(key, name, text)


# =====================================================================
# 8. Copy from one dataset to another
# =====================================================================

@testcase("8", "copy_pds_to_pds")
def copy_pds_to_pds(ctx):
    _have(ctx, "fb", "fb2")
    name = "CPYDS"
    text = ctx.gen("fb", "small")
    ctx.reset("fb", name)
    ctx.reset("fb2", name)
    ctx.track("fb", name)
    ctx.track("fb2", name)
    ctx.write_member("fb", name, text, track=False)
    shutil.copyfile(str(ctx.member_path("fb", name)), str(ctx.member_path("fb2", name)))
    ctx.verify_member("fb2", name, text)


# =====================================================================
# 9. Unzip to a dataset
# =====================================================================

@testcase("9", "unzip_to_dataset")
def unzip_to_dataset(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    names = ["UZ1", "UZ2", "UZ3"]
    ctx.reset(key, *names)
    for n in names:
        ctx.track(key, n)
    payloads = {n: ("ENTRY %s\n" % n) + ctx.gen(key, "small") for n in names}
    zpath = ctx.workdir / "unzip1.zip"
    with zipfile.ZipFile(str(zpath), "w", zipfile.ZIP_DEFLATED) as z:
        for n in names:
            z.writestr(ctx.filename(key, n), payloads[n])
    # "unzip -d <pds>": each entry becomes a member in the dataset directory.
    with zipfile.ZipFile(str(zpath)) as z:
        for info in z.infolist():
            data = z.read(info.filename).decode("latin-1")
            with open(str(ctx.pds_dir(key) / info.filename), "w", newline="\n") as f:
                f.write(data)
    for n in names:
        ctx.verify_member(key, n, payloads[n])


# =====================================================================
# 10. Unzip to multiple datasets
# =====================================================================

@testcase("10", "unzip_to_multiple")
def unzip_to_multiple(ctx):
    _have(ctx, "fb", "vb")
    plan = {"fb": ["UM1", "UM2"], "vb": ["UM3", "UM4"]}
    payloads = {}
    zpath = ctx.workdir / "unzip_multi.zip"
    with zipfile.ZipFile(str(zpath), "w", zipfile.ZIP_DEFLATED) as z:
        for key, names in plan.items():
            for n in names:
                ctx.reset(key, n)
                ctx.track(key, n)
                txt = ("MULTI %s/%s\n" % (key, n)) + ctx.gen(key, "small")
                payloads[(key, n)] = txt
                # entry path carries the dataset directory, so extraction to the
                # mount root lands each member in the right dataset.
                z.writestr("%s/%s" % (ctx.ds(key)["nfs_dir"], ctx.filename(key, n)), txt)
    with zipfile.ZipFile(str(zpath)) as z:
        for info in z.infolist():
            data = z.read(info.filename).decode("latin-1")
            with open(str(ctx.mount_root / info.filename), "w", newline="\n") as f:
                f.write(data)
    for (key, n), txt in payloads.items():
        ctx.verify_member(key, n, txt)


# =====================================================================
# 11. Create a zip archive from a dataset
# =====================================================================

@testcase("11", "zip_from_dataset")
def zip_from_dataset(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    names = ["ZP1", "ZP2", "ZP3"]
    ctx.reset(key, *names)
    for n in names:
        ctx.track(key, n)
    payloads = {n: ("ZIPSRC %s\n" % n) + ctx.gen(key, "small") for n in names}
    for n in names:
        ctx.write_member(key, n, payloads[n], track=False)
    zpath = ctx.workdir / "from_ds.zip"
    with zipfile.ZipFile(str(zpath), "w", zipfile.ZIP_DEFLATED) as z:
        for n in names:
            z.writestr(ctx.filename(key, n), ctx.read_member(key, n))
    _verify_zip(ctx, zpath, {ctx.filename(key, n): payloads[n] for n in names})


# =====================================================================
# 12. Create a zip archive from multiple datasets
# =====================================================================

@testcase("12", "zip_from_multiple")
def zip_from_multiple(ctx):
    _have(ctx, "fb", "vb")
    plan = {"fb": ["ZM1", "ZM2"], "vb": ["ZM3"]}
    entries = {}
    for key, names in plan.items():
        for n in names:
            ctx.reset(key, n)
            ctx.track(key, n)
            txt = ("ZM %s/%s\n" % (key, n)) + ctx.gen(key, "small")
            ctx.write_member(key, n, txt, track=False)
            entries["%s/%s" % (ctx.ds(key)["nfs_dir"], ctx.filename(key, n))] = txt
    zpath = ctx.workdir / "from_multi.zip"
    with zipfile.ZipFile(str(zpath), "w", zipfile.ZIP_DEFLATED) as z:
        for key, names in plan.items():
            for n in names:
                arc = "%s/%s" % (ctx.ds(key)["nfs_dir"], ctx.filename(key, n))
                z.writestr(arc, ctx.read_member(key, n))
    _verify_zip(ctx, zpath, entries)


def _verify_zip(ctx, zpath, expected):
    """Assert the archive holds exactly 'expected' {arcname: text}."""
    with zipfile.ZipFile(str(zpath)) as z:
        got = set(z.namelist())
        for arc, text in expected.items():
            ctx.check(arc in got, "zip missing entry %r (have %s)" % (arc, sorted(got)))
            d = textutil.diff_summary(text, z.read(arc).decode("latin-1"))
            ctx.check(not d, "zip entry %r content mismatch: %s" % (arc, d))
