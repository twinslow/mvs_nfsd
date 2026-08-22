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


def _alt_text(ctx, key, n, tag):
    """n lines sized for the dataset's RECFM, with a caller-chosen tag.

    ctx.gen() is fixed at 6 lines / 'large_lines' and one tag, which is fine
    when a test writes a member once.  A test that writes the SAME member
    twice needs the two contents to differ in both text and LENGTH, so a
    stale tail or a half-replaced member cannot pass by accident."""
    if ctx.is_vb(key):
        return textutil.gen_var_lines(n, base=20, tag=tag)
    return textutil.gen_fixed_lines(n, int(ctx.opts["line_width"]), tag=tag)


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
    """Fill a tiny PDS and prove the SERVER SURVIVES it.

    The pass condition is deliberately *not* "a write failed".  A flush that
    lands on the idle sweep has no client request in flight to fail, so the
    ENOSPC may never reach us at all (design_nfs_write.md Sec 7.3, "the
    reporting gap") -- the server nonetheless has to stay up.  So: hammer the
    full dataset a bounded number of times, then prove the server is still
    serving by writing and verifying a member in a healthy dataset.

    The attempt count is kept small on purpose: every failed member costs a
    real D37+B14 abend on the host, so 300 attempts (the original figure) meant
    minutes of console spam for no extra signal.
    """
    ctx.require_mvs()
    _have(ctx, "small", "fb")
    key = "small"
    data = ctx.gen(key, "large")
    attempts, wrote, refused = 12, 0, 0

    for i in range(attempts):
        name = "FULL%03d" % i
        try:
            with open(str(ctx.member_path(key, name)), "w", newline="\n") as f:
                f.write(data)
            wrote += 1
        except OSError:
            refused += 1          # expected once the server remembers it is full
    # A PREALLOCATE against the full dataset, which is the shape a real
    # client uses and which reached the flush ungated until 2026-08 (design
    # Sec 9.3.4).  Refusal is the expected answer; what must NOT happen is
    # an abend, so the outcome is not asserted -- only survival, below.
    try:
        p = str(ctx.member_path(key, "FULLTRNC"))
        ctx.track(key, "FULLTRNC")
        with open(p, "w", newline="\n"):
            pass
        os.truncate(p, len(data.encode("latin-1")))
    except OSError:
        pass

    for i in range(attempts):
        ctx.remove_member(key, "FULL%03d" % i)
    ctx.remove_member(key, "FULLTRNC")

    # Probe 1: the dataset that just filled is still LISTABLE.  This is the
    # sharpest survival check there is -- mvs_open_pds_dir()'s fopen is the
    # call that actually parked in every observed hang
    # (doc/analysis_io_lock_hang.md), and it touches the very dataset the
    # abend happened on.
    try:
        os.listdir(str(ctx.pds_dir(key)))
    except OSError as e:
        raise AssertionError(
            "server did not survive the full dataset: listing %s afterwards "
            "failed (%s)" % (ctx.ds(key)["nfs_dir"], e))

    # Probe 2: the server is alive and still serving other datasets.
    probe = "ALIVE"
    text  = ctx.gen("fb", "small")
    ctx.reset("fb", probe)
    ctx.track("fb", probe)
    try:
        ctx.write_member("fb", probe, text, track=False)
    except OSError as e:
        raise AssertionError(
            "server did not survive the full dataset: writing to a healthy "
            "dataset afterwards failed (%s). %d of %d attempts were refused."
            % (e, refused, attempts))
    ctx.verify_member("fb", probe, text)


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
    """Prove the DOWNLOAD path: members prepared OUT-OF-BAND are read back
    correctly over NFS.

    Preparation is BATCHED on purpose.  A member created behind the server's
    back becomes visible only once that dataset's out-of-band change detection
    fires, and that is throttled (DIR_REFRESH_THROTTLE_SECS, 10s).  Preparing
    one member and then waiting for it, four times over, pays the throttle
    four times -- which was the whole cost of this test.  Preparing everything
    first means a single refresh per dataset reveals all of its new members,
    so the test waits once instead of once per member.
    """
    items = []
    for key in _text_keys(ctx):
        for kind, name in (("small", "DLS"), ("large", "DLL")):
            items.append((key, name, ctx.gen(key, kind)))

    for key, name, _text in items:
        ctx.reset(key, name)
        ctx.track(key, name)

    # ---- prepare every member first -------------------------------------
    for key, name, text in items:
        # Settle before an FTP STOR, which wants exclusive access to the PDS.
        # This is free unless something was actually written over NFS since
        # the last settle -- which happens only on the fallback path below.
        ctx.settle()
        try:
            ctx.backend.prepare(ctx, key, name, text)
        except Exception as e:                        # noqa
            # Not every MVS FTP server can create a PDS member (this one
            # answers "550 <mem>: Not opened"), and that is a limitation of
            # the verification channel, not of the server under test.  Fall
            # back to preparing over NFS so this test still does its real
            # job -- proving the DOWNLOAD path -- and say so out loud rather
            # than quietly weakening the check.
            print("      note: FTP preparation unavailable (%s);"
                  " preparing %s over NFS instead" % (str(e).strip(), name))
            ctx.write_member(key, name, text, track=False)

    # ---- then read them all back ----------------------------------------
    # The first member of each dataset absorbs that dataset's throttle wait;
    # the rest are already visible, because the same refresh saw them too.
    for key, name, text in items:
        ctx.check(ctx.wait_visible(key, name),
                  "member %s was prepared but never became visible over"
                  " NFS -- out-of-band change detection did not bump"
                  " dir_mtime in time" % name)
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


@testcase("2.3", "update_stowed_member")
def update_stowed_member(ctx):
    """Overwrite a member that is ALREADY IN THE PDS, and prove both paths
    see the new content.

    Every other write test starts from reset(), so it only ever exercises
    creating a member.  Here the first copy is confirmed stowed (verify_now)
    before the second write, so the flush is a STOW over an existing
    directory entry rather than an add.

    The replacement is deliberately SHORTER than the original: if any part of
    the first copy survived -- a stale tail, a directory entry still carrying
    the old size -- the comparison fails instead of quietly passing."""
    for key in _text_keys(ctx):
        name  = "UPDSTOW"
        first = _alt_text(ctx, key, 9, "IT1")

        ctx.reset(key, name)
        ctx.write_member(key, name, first)
        ctx.verify_now(key, name, first)     # must be stowed before we update

        second = _alt_text(ctx, key, 4, "IT2")
        ctx.write_member(key, name, second)

        got = ctx.read_member(key, name)
        d = textutil.diff_summary(second, got)
        ctx.check(not d, "NFS read after updating stowed %s/%s: %s"
                         % (key, name, d))
        ctx.verify_member(key, name, second, updated=True)


@testcase("2.4", "rewrite_pending_member")
def rewrite_pending_member(ctx):
    """Rewrite a member while the server still has it PENDING.

    Deliberately no verify_now and no settle between the two writes, so the
    second one lands while the first is still buffered in the write pool.
    That takes a different route through the server than 2.3: the slot is
    found already in use, so the truncate-to-zero goes through the
    "re-create over an existing pending member" path rather than allocating
    a fresh slot -- and the member must end up as the SECOND content only,
    with nothing left of the first.

    Both sizes, because a pending member may be held in memory or in the
    spill dataset, and the reset path differs."""
    for key in _text_keys(ctx):
        for kind, name in (("small", "REWRS"), ("large", "REWRL")):
            first  = ctx.gen(key, kind)
            second = _alt_text(ctx, key, 5, "IT3")

            ctx.reset(key, name)
            ctx.write_member(key, name, first)
            ctx.write_member(key, name, second)   # first copy still pending

            got = ctx.read_member(key, name)
            d = textutil.diff_summary(second, got)
            ctx.check(not d, "NFS read after rewriting pending %s/%s: %s"
                             % (key, name, d))
            ctx.verify_member(key, name, second, updated=True)


@testcase("2.5", "append_no_data_loss", requires="mvs")
def append_no_data_loss(ctx):
    """An append must never leave the client thinking data was stored that
    was not.  EITHER outcome is acceptable; only losing data is not:

      refused  -- the write raises, and the member is byte-for-byte the
                  original.  This is what the server does when it cannot
                  honour the write: it has no pending buffer for the member
                  and never reads one back, so satisfying a write at a
                  non-zero offset would mean inventing the bytes before it
                  and the flush would replace the member with zeros.
      accepted -- the write succeeds, and the member is the original plus the
                  appended text.  Nothing was lost, so nothing is wrong.

    Which one happens is up to the CLIENT, not us, and that is why the test
    accepts both.  NFS clients are page-granular: to append they must first
    read the page they are about to modify.  For a member smaller than a page
    that means reading the whole thing and rewriting it from offset 0 -- an
    append at the API level that is a plain rewrite on the wire, which the
    server handles normally.  Only once the member spans several pages does
    the client write just the dirty tail page, at a non-zero offset, which is
    the case the refusal exists for.

    Hence the LARGE original: a small one is normalised into a rewrite by the
    client and never reaches the code under test.  Even so the outcome is not
    guaranteed -- wsize, caching and client version all bear on it -- so the
    assertion is on the invariant that actually matters rather than on which
    path was taken.

    The settle() is LOAD-BEARING, not tidiness.  A flush stows the member but
    KEEPS the slot, so an append arriving straight afterwards still finds the
    buffer holding the original content and is then perfectly satisfiable.
    Only once the idle sweep has released the slot is there nothing left to
    append to.
    """
    for key in _text_keys(ctx):
        name     = "APPEND"
        original = ctx.gen(key, "large")      # must span several pages
        extra    = _alt_text(ctx, key, 2, "IT5")

        ctx.reset(key, name)
        ctx.write_member(key, name, original)
        ctx.verify_now(key, name, original)   # stowed in the PDS...
        ctx.settle()                          # ...and the slot released

        raised = None
        try:
            with open(str(ctx.member_path(key, name)), "a", newline="\n") as f:
                f.write(extra)
                f.flush()
                os.fsync(f.fileno())          # force it to the server now
        except (OSError, IOError) as e:       # noqa -- IOError is an alias
            raised = e

        if raised is not None:
            ctx.verify_member(key, name, original)      # refused: unchanged
        else:
            ctx.verify_member(key, name, original + extra,   # accepted: intact
                              updated=True)


# =====================================================================
# 3. Create member via touch
# =====================================================================

@testcase("3", "touch_create")
def touch_create(ctx):
    key = "fb" if "fb" in ctx.cfg["datasets"] else ctx.keys()[0]
    name = "TOUCHED"
    ctx.reset(key, name)
    ctx.track(key, name)
    ctx.touch_member(key, name, track=False)
    ctx.check(ctx.wait_exists(key, name, True),
              "member should exist after touch (waited for the server flush)")


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

    # The member must be STOWED before we touch its stats: otherwise the
    # still-pending flush lands afterwards and rewrites the changed-date with
    # time(NULL), silently undoing the utime (this showed up as a mtime that was
    # exactly our 3600s offset out).
    ctx.check(ctx.wait_exists(key, name, True),
              "member should be stowed before its stats are touched")
    ctx.settle()

    target = time.time() - 3600.0                        # clearly different from "now"
    os.utime(str(ctx.member_path(key, name)), (target, target))

    # Poll rather than read once: a value that arrives late is attribute
    # caching, whereas one that never arrives means the SETATTR was not applied.
    tol = float(ctx.opts["mtime_tolerance_sec"])
    ok, seen = ctx.wait_mtime(key, name, target, tol)
    ctx.check(ok,
              "mtime never reached the value we set (last seen %s, wanted %d,"
              " tolerance %ds). A gap of ~3600s here means the member kept its"
              " stow time, i.e. the SETATTR was not applied to the ISPF changed"
              " date -- not a timezone problem."
              % (int(seen) if seen is not None else "none", target, tol))
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
    ctx.check(ctx.wait_exists(key, name, True),
              "member should be stowed before it is deleted")
    ctx.member_path(key, name).unlink()
    ctx.check(ctx.wait_exists(key, name, False),
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


# =====================================================================
# 13. SETATTR(size) -- truncate
#
# Until 2026-08 this path was NOT gated by the space prediction, and it is
# far from rare: Linux PREALLOCATES with SETATTR(size) between the CREATE
# and the first WRITE, so a member can reach full size before a single byte
# of data arrives.  Nothing in the suite exercised it.
# =====================================================================

def _byte_len(text):
    """Length of 'text' as the client writes it (LF line endings, no CRLF)."""
    return len(text.encode("latin-1"))


@testcase("13.1", "truncate_stowed_no_silent_lie")
def truncate_stowed_no_silent_lie(ctx):
    """Shrinking a STOWED member: succeed and shorten it, or fail and leave
    it alone.  What must never happen is success with nothing changed.

    Asserting the INVARIANT rather than the mechanism, because os.truncate()
    does not map to one server operation:

      * Linux sends a bare SETATTR(size).  With no pending slot the server
        cannot rewrite the member on disk, so it refuses -- EIO ->
        NFS3ERR_IO, matching a random write at a non-zero offset.
      * Windows re-writes the file through the ordinary WRITE path instead,
        which the server fully supports, so it succeeds and the member
        really is shorter.

    Both are correct.  Earlier versions of this test asserted one mechanism
    or the other and so failed on whichever platform it was not written
    against -- twice.  The thing worth protecting is neither: it is that the
    server never reports success while silently keeping the old, longer
    content, which is what it used to do."""
    for key in _text_keys(ctx):
        name  = "TRUNCSH"
        full  = _alt_text(ctx, key, 12, "TSHRINK")
        lines = full.split("\n")[:-1]
        keep    = "\n".join(lines[:5]) + "\n"
        shorter = _byte_len(keep)

        ctx.reset(key, name)
        ctx.write_member(key, name, full)
        ctx.verify_now(key, name, full)      # stows it, releasing the slot

        try:
            os.truncate(str(ctx.member_path(key, name)), shorter)
        except OSError:
            # Refused.  The member must be untouched -- a refusal that still
            # damaged it would be the worst of both outcomes.
            ctx.verify_member(key, name, full, updated=True)
            continue

        # Accepted.  Then it has to be TRUE: the member really is shorter.
        ctx.verify_member(key, name, keep, updated=True)


@testcase("13.5", "truncate_same_size_ok")
def truncate_same_size_ok(ctx):
    """SETATTR(size) to the size the member ALREADY has must succeed.

    This is not a corner case: it is part of the normal write sequence --
    Windows sends WRITE -> COMMIT -> SETATTR -> COMMIT -- and when the idle
    sweep has already released the slot that trailing SETATTR lands on the
    same path 13.1 exercises.  Refusing it there would break ordinary writes
    at random, depending on whether the sweep got in first.

    Guards the exemption that makes 13.1's refusal safe."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    name = "TRUNCSM"
    text = _alt_text(ctx, key, 10, "TSAME")

    ctx.reset(key, name)
    ctx.write_member(key, name, text)
    ctx.verify_now(key, name, text)          # stows it, releasing the slot

    size = os.stat(str(ctx.member_path(key, name))).st_size
    try:
        os.truncate(str(ctx.member_path(key, name)), size)
    except OSError as e:
        raise AssertionError(
            "%s(%s): SETATTR to the member's CURRENT size (%d) was refused"
            " (%s).  Clients send this after every write, so it has to be"
            " accepted as the no-op it is"
            % (ctx.ds(key)["nfs_dir"], name, size, e))

    ctx.verify_member(key, name, text, updated=True)


@testcase("13.4", "truncate_pending")
def truncate_pending(ctx):
    """Shrink a member that is still PENDING -- the path that IS supported.

    With a slot in the pool, pww_truncate() adjusts the buffered content to
    exactly 'size'.  Whether the slot still exists when the SETATTR lands is
    a race against the idle sweep, so BOTH outcomes are accepted: shortened
    (the slot was there) or unchanged (it had been flushed and released, so
    13.1's no-op applies).  What is NOT accepted is anything else -- a
    partial line or mixed content means the truncate was half-applied."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    name  = "TRUNCPD"
    full  = _alt_text(ctx, key, 12, "TPEND")
    lines = full.split("\n")[:-1]
    keep  = "\n".join(lines[:5]) + "\n"

    ctx.reset(key, name)
    ctx.track(key, name)
    ctx.write_member(key, name, full, track=False)
    os.truncate(str(ctx.member_path(key, name)), _byte_len(keep))

    ctx.settle()                                # let the slot be released
    ctx.check(ctx.wait_exists(key, name, True),
              "%s(%s): member never reached the PDS after the truncate"
              % (ctx.ds(key)["nfs_dir"], name))
    got = ctx.backend.fetch(ctx, key, name)
    if textutil.text_equal(keep, got):
        return                                  # applied: the slot was live
    ctx.check(textutil.text_equal(full, got),
              "%s(%s): after truncating a pending member the content is"
              " neither shortened nor untouched -- it was half applied. %s"
              % (ctx.ds(key)["nfs_dir"], name,
                 textutil.diff_summary(keep, got)))


@testcase("13.2", "truncate_to_zero")
def truncate_to_zero(ctx):
    """Truncate a member to nothing.

    It must survive as an EMPTY member -- not vanish, and not keep its old
    content.  This is the O_TRUNC case every 'overwrite this file' client
    performs before writing."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    name = "TRUNC0"
    text = _alt_text(ctx, key, 8, "TZERO")

    ctx.reset(key, name)
    ctx.write_member(key, name, text)
    ctx.verify_now(key, name, text)

    os.truncate(str(ctx.member_path(key, name)), 0)
    ctx.check(ctx.wait_exists(key, name, True),
              "member should still exist after being truncated to zero")
    ctx.verify_member(key, name, "", updated=True)


@testcase("13.3", "preallocate_then_write")
def preallocate_then_write(ctx):
    """The client's own sequence: CREATE, SETATTR(size), then WRITE.

    This is what a Linux client does for any sizeable file, and it is the
    exact shape that reached the flush ungated and abended SB14 (see
    doc/design_pds_full_prediction.md Sec 9.3.4).  The member must end up
    holding the DATA, not the zero-fill the truncate created."""
    for key in _text_keys(ctx):
        name = "PREALLOC"
        text = _alt_text(ctx, key, 40, "TPREALL")

        ctx.reset(key, name)
        ctx.track(key, name)
        p = str(ctx.member_path(key, name))
        with open(p, "w", newline="\n"):
            pass                            # CREATE
        os.truncate(p, _byte_len(text))     # SETATTR(size) -- preallocate
        with open(p, "w", newline="\n") as f:
            f.write(text)                   # then the real content
        ctx.verify_member(key, name, text, updated=True)


@testcase("13.6", "preallocate_after_flush")
def preallocate_after_flush(ctx):
    """CREATE, WAIT for the idle sweep, then SETATTR(size) and write.

    Notepad's save, and the case 13.3 cannot reach.  13.3 runs the whole
    sequence back to back, so the slot from the CREATE is still in the pool
    when the SETATTR arrives and the pending-member path handles it.  Leave
    a pause -- a user typing for a few seconds -- and the sweep stows the
    empty member and drops the slot first, so the SETATTR lands with nothing
    buffered.  That is a different branch of vfs_truncate() entirely.

    It reached production as every Notepad save reporting an error while the
    file was written correctly anyway (2026-08-20): the grow was refused with
    NFS3ERR_IO because the check compared sizes for INEQUALITY rather than
    testing for a shrink.  A grow is preallocation, not a rewrite."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    name = "PREFLSH"
    text = _alt_text(ctx, key, 3, "TPREFL")

    ctx.reset(key, name)
    ctx.track(key, name)
    ctx.touch_member(key, name, track=False)      # CREATE, empty
    ctx.settle()
    ctx.check(ctx.wait_exists(key, name, True),
              "%s(%s): empty member never reached the PDS, so the slot was"
              " probably still pending and this test proved nothing"
              % (ctx.ds(key)["nfs_dir"], name))

    p = str(ctx.member_path(key, name))
    try:
        os.truncate(p, _byte_len(text))           # SETATTR(size), NO slot
    except OSError as e:
        raise AssertionError(
            "%s(%s): preallocating to %d bytes was refused (%s).  Growing a"
            " stowed member is how Notepad and other preallocating clients"
            " save -- only a SHRINK is"
            " unsupported" % (ctx.ds(key)["nfs_dir"], name,
                              _byte_len(text), e))

    with open(p, "w", newline="\n") as f:
        f.write(text)
    ctx.verify_member(key, name, text, updated=True)


# =====================================================================
# 14. Directory listing (READDIR / READDIRPLUS)
#
# The suite verified member CONTENT thoroughly and never once asserted what
# the client SEES in a listing -- so cookie handling, the directory cache and
# its invalidation had no coverage at all.  A cookie bug here does not corrupt
# data: it repeats entries, drops them, or loops forever.
# =====================================================================

@testcase("14.1", "listing_reflects_changes")
def listing_reflects_changes(ctx):
    """A create, a rename and a delete must each reach the listing."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    a, b = "LSTA", "LSTB"
    text = ctx.gen(key, "small")

    ctx.reset(key, a, b)
    ctx.track(key, a)
    ctx.track(key, b)

    ctx.write_member(key, a, text, track=False)
    ctx.check(ctx.wait_listed(key, a, True),
              "created member %s never appeared in the directory listing" % a)

    os.rename(str(ctx.member_path(key, a)), str(ctx.member_path(key, b)))
    ctx.check(ctx.wait_listed(key, b, True),
              "renamed member %s never appeared in the listing" % b)
    ctx.check(ctx.wait_listed(key, a, False),
              "old name %s is still listed after the rename" % a)

    ctx.remove_member(key, b)
    ctx.check(ctx.wait_listed(key, b, False),
              "deleted member %s is still listed" % b)


def _member_of(ctx, key, filename):
    """The PDS member name behind a listed file name ('lsmatch.txt' -> 'LSMATCH')."""
    ext = ctx.ds(key).get("ext", "")
    stem = filename
    if ext and stem.lower().endswith("." + ext.lower()):
        stem = stem[:-(len(ext) + 1)]
    return ctx.mvs_name(stem)


@testcase("14.2", "listing_matches_backend")
def listing_matches_backend(ctx):
    """Everything NFS lists must really be in the PDS.

    Checked one member at a time with backend.exists(), NOT against
    backend.members().  members() is documented diagnostic-only: it turns a
    550 from NLST into an EMPTY SET, so on an FTP server that will not answer
    NLST for a PDS every real member looks like a phantom and the test fails
    while the server is behaving perfectly.  exists() is the primitive the
    rest of the suite trusts (wait_exists uses it) and it reports errors
    instead of swallowing them.

    The converse direction -- a member that exists but is NOT listed -- is
    covered by the wait_listed() check below and by 14.1."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    name = "LSMATCH"
    text = ctx.gen(key, "small")

    ctx.reset(key, name)
    ctx.write_member(key, name, text)
    ctx.verify_now(key, name, text)          # forces the flush, then settles
    ctx.check(ctx.wait_listed(key, name, True),
              "member %s is not listed even though it is stowed" % name)

    # Bounded: one FTP round trip per entry, and the point is made by a
    # sample.  A phantom entry is a systematic fault, not a rare one.
    listed  = ctx.list_dir(key)[:12]
    phantom = []
    for fn in listed:
        m = _member_of(ctx, key, fn)
        try:
            if not ctx.backend.exists(ctx, key, m):
                phantom.append(fn)
        except Exception as e:              # noqa -- report, do not mask
            raise AssertionError(
                "backend check for listed member %s failed: %s" % (fn, e))

    ctx.check(not phantom,
              "NFS lists %s, which the PDS does not contain -- a READDIR"
              " entry with no member behind it" % phantom)


@testcase("14.3", "listing_large_directory")
def listing_large_directory(ctx):
    """A directory big enough to need SEVERAL READDIR pages.

    The failure this guards is not corruption.  An unstable directory mtime
    once made the Linux client restart from cookie 0 on every page, so the
    listing never ended and the client spun forever.  A cookie that repeats
    or skips shows up here as duplicates or a short list; a cookie that never
    advances shows up as this test timing out."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    n     = int(ctx.opts.get("readdir_members", 40))
    names = ["LSPG%03d" % i for i in range(n)]
    text  = ctx.gen(key, "small")

    ctx.reset(key, *names)
    for nm in names:
        ctx.track(key, nm)
        ctx.write_member(key, nm, text, track=False)

    ctx.check(ctx.wait_listed(key, names[-1], True),
              "the last of %d members never appeared in the listing" % n)

    raw   = os.listdir(str(ctx.pds_dir(key)))     # deliberately NOT a set
    low   = [x.lower() for x in raw]
    dupes = sorted(set(x for x in low if low.count(x) > 1))
    ctx.check(not dupes,
              "listing returned duplicate entries %s -- a READDIR cookie is"
              " repeating" % dupes[:10])

    listed  = set(low)
    missing = [nm for nm in names
               if ctx.filename(key, nm).lower() not in listed]
    ctx.check(not missing,
              "%d of %d members are missing from the listing (e.g. %s) -- a"
              " READDIR cookie is skipping entries"
              % (len(missing), n, missing[:5]))


def _big_dir(ctx, want):
    """A directory under the mount with at least 'want' entries, or None.

    Prefers options.readdir_big_dir when set -- a read-only system PDS such
    as sys1.samplib has hundreds of members and costs nothing to prepare.
    Otherwise takes the largest configured dataset directory, so the test
    still does something useful if the mount happens to point at a big
    export.  Returns (path, entries)."""
    cand = []
    named = ctx.opts.get("readdir_big_dir")
    if named:
        cand.append(ctx.mount_root / named)
    for key in ctx.keys():
        cand.append(ctx.pds_dir(key))

    best = None
    for p in cand:
        try:
            names = os.listdir(str(p))
        except OSError:
            continue
        if best is None or len(names) > len(best[1]):
            best = (p, names)
        if named and p == cand[0] and len(names) >= want:
            return best          # the explicitly named one is good enough
    if best is not None and len(best[1]) >= want:
        return best
    return None


@testcase("14.5", "listing_pages")
def listing_pages(ctx):
    """A listing big enough to span SEVERAL READDIRPLUS replies.

    14.3 creates its own members, which keeps it fast but caps it well under
    one page: the server charges READDIRPLUS_ENTRY_OVERHEAD (184 bytes) plus
    the padded name per entry, so a 32 KB maxcount holds ~167 of them.  This
    test instead LISTS a directory that is already large -- typically a
    read-only system PDS named by options.readdir_big_dir -- so paging is
    exercised for the cost of one listdir and no setup at all.

    Ground truth for a foreign dataset is not available, so the assertions
    are the ones that hold regardless of content: no entry appears twice, the
    listing is reproducible, and names it returned can actually be stat'd.
    Between them those catch a cookie that repeats, one that wanders, and one
    that invents entries.  A cookie that never advances shows up as this test
    hanging, which is exactly what the 2026-07 readdir loop did."""
    want = int(ctx.opts.get("readdir_big_min", 60))
    got  = _big_dir(ctx, want)
    if got is None:
        raise TestSkip(
            "needs a directory of >= %d entries under the mount; set"
            " options.readdir_big_dir to one (e.g. a system PDS)" % want)
    path, first = got

    low   = [x.lower() for x in first]
    dupes = sorted(set(x for x in low if low.count(x) > 1))
    ctx.check(not dupes,
              "%s: listing returned duplicate entries %s across %d total --"
              " a READDIR cookie is repeating"
              % (path.name, dupes[:10], len(first)))

    # Reproducible: a cookie that drifts gives a different set each sweep.
    second = os.listdir(str(path))
    a, b   = set(low), set(x.lower() for x in second)
    ctx.check(a == b,
              "%s: two consecutive listings disagree (%d then %d entries;"
              " only in the first %s, only in the second %s)"
              % (path.name, len(first), len(second),
                 sorted(a - b)[:5], sorted(b - a)[:5]))

    # The names are real: a phantom entry cannot be stat'd.  First, middle
    # and last, because a cookie fault shows at a page boundary, not evenly.
    picks = [first[0], first[len(first) // 2], first[-1]]
    for n in picks:
        try:
            os.stat(str(path / n))
        except OSError as e:
            raise AssertionError(
                "%s: listed entry %r cannot be stat'd (%s) -- READDIR"
                " returned a name with nothing behind it" % (path.name, n, e))


@testcase("14.4", "root_listing")
def root_listing(ctx):
    """The export root must list every configured dataset directory."""
    root = sorted(x.lower() for x in os.listdir(str(ctx.mount_root)))
    for key in ctx.keys():
        d = ctx.ds(key)["nfs_dir"].lower()
        ctx.check(d in root,
                  "dataset directory %r is missing from the export root"
                  " (root lists %s)" % (d, root[:10]))


# =====================================================================
# 15. Long lines -- record wrapping
#
# The server does not build records itself: it writes the byte stream and
# lets JCC split it, WRAPPING a line longer than the record length into
# several records.  mvsblkc.c PREDICTS that split to decide whether a member
# fits.  If the prediction and the runtime ever disagree the result is a
# wrong ENOSPC -- or an SB14 abend.  Nothing verified the split itself.
# =====================================================================

def _reclen(ctx, key):
    """Bytes of DATA one record holds: LRECL, less the 4-byte RDW on V/VB."""
    lrecl = int(ctx.ds(key).get("lrecl", 80))
    return lrecl - 4 if ctx.is_vb(key) else lrecl


def _wrapped(text, width):
    """'text' as it reads back once JCC has split it into 'width' records."""
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()                       # the trailing newline, not a line
    out = []
    for line in lines:
        if not line:
            out.append("")                # an empty line is still one record
            continue
        for i in range(0, len(line), width):
            out.append(line[i:i + width])
    return "\n".join(out) + "\n"


@testcase("15", "long_line_wrap")
def long_line_wrap(ctx):
    """Write lines longer than the record length; assert they WRAP.

    Checked against the record length rather than against 'whatever came
    back', so a server that silently truncated the overflow -- losing data
    with no error at all -- fails here instead of passing quietly."""
    for key in _text_keys(ctx):
        w    = _reclen(ctx, key)
        name = "LONGLN"
        # Well under, exactly on the boundary, then over into 2 and 3
        # records.  The exact-LRECL line is the off-by-one that matters.
        src = "\n".join([
            "S" * (w // 2),
            "E" * w,
            "T" * (w + 1),
            "M" * (2 * w + 7),
        ]) + "\n"

        ctx.reset(key, name)
        ctx.write_member(key, name, src)
        ctx.verify_member(key, name, _wrapped(src, w))


# =====================================================================
# 16. Read-only datasets
#
# `ro` is enforced by mvs_check_writable(), which every mutating operation
# calls.  It is deliberately independent of uid and of the reported mode
# bits, so the meaningful assertion is that the OPERATIONS fail -- not what
# st_mode says.  Needs a dataset the SERVER exports read-only: add one to
# the test export with the `ro` keyword and give it the config key "ro".
# =====================================================================

def _must_fail(ctx, what, fn):
    """Run fn(); assert it did not succeed.  Returns the OSError, or None."""
    try:
        fn()
    except OSError as e:
        return e
    raise AssertionError("%s SUCCEEDED on a read-only dataset" % what)


@testcase("16", "readonly_dataset")
def readonly_dataset(ctx):
    """A read-only dataset must serve reads and refuse every mutation."""
    _have(ctx, "ro")
    key = "ro"

    # Reads work: this is a read-ONLY dataset, not an unreachable one.
    # A raise here fails the test on its own and says so.
    entries = os.listdir(str(ctx.pds_dir(key)))

    _must_fail(ctx, "creating a member",
               lambda: open(str(ctx.member_path(key, "ROTEST")), "w").close())
    ctx.check(ctx.filename(key, "ROTEST").lower()
              not in [x.lower() for x in os.listdir(str(ctx.pds_dir(key)))],
              "a member appeared on the read-only dataset despite the refusal")

    if not entries:
        raise TestSkip("read-only dataset is empty; cannot test"
                       " overwrite/delete of an existing member")

    victim = entries[0]
    path   = str(ctx.pds_dir(key) / victim)
    before = open(path, "rb").read()

    _must_fail(ctx, "overwriting %s" % victim,
               lambda: open(path, "w").close())
    _must_fail(ctx, "deleting %s" % victim,
               lambda: os.unlink(path))

    after = open(path, "rb").read()
    ctx.check(before == after,
              "%s changed on a read-only dataset (%d bytes -> %d)"
              % (victim, len(before), len(after)))


# =====================================================================
# 17. Operations the server answers NFS3ERR_NOTSUPP
#
# Cheap regression guards: these must keep failing.  A future change that
# accidentally makes one succeed would create something the PDS model
# cannot represent.
# =====================================================================

@testcase("17.1", "mkdir_not_supported")
def mkdir_not_supported(ctx):
    """MKDIR inside a PDS directory, and at the export root.  A PDS has no
    sub-directories, so both must be refused."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)

    for label, p in (("inside a PDS", ctx.pds_dir(key) / "SUBDIR"),
                     ("at the export root", ctx.mount_root / "newpds")):
        try:
            os.mkdir(str(p))
        except OSError:
            continue
        try:
            os.rmdir(str(p))                 # tidy up if it somehow worked
        except OSError:
            pass
        raise AssertionError("MKDIR %s SUCCEEDED; it must be NOTSUPP" % label)


@testcase("17.2", "symlink_not_supported")
def symlink_not_supported(ctx):
    """SYMLINK must be refused.

    Skipped on Windows: the client rejects symlink creation locally without
    privileges, so the request never reaches the server and a 'pass' would
    mean nothing."""
    if os.name == "nt":
        raise TestSkip("Windows client refuses symlink locally; not a server test")
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)

    link = str(ctx.pds_dir(key) / "SYMLNK")
    try:
        os.symlink("target", link)
    except OSError:
        return
    try:
        os.unlink(link)
    except OSError:
        pass
    raise AssertionError("SYMLINK SUCCEEDED; it must be NOTSUPP")


@testcase("17.3", "hardlink_not_supported")
def hardlink_not_supported(ctx):
    """LINK must be refused.  A PDS directory entry cannot be aliased."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    src = "LNKSRC"
    ctx.reset(key, src)
    ctx.write_member(key, src, ctx.gen(key, "small"))

    dst = str(ctx.member_path(key, "LNKDST"))
    try:
        os.link(str(ctx.member_path(key, src)), dst)
    except OSError:
        return
    ctx.track(key, "LNKDST")
    raise AssertionError("LINK SUCCEEDED; it must be NOTSUPP")


# =====================================================================
# 18. Member name validation
#
# mvs_member_name_valid(): 1-8 characters, first a letter or national
# (@ # $) and NOT a digit, the rest letters/digits/national.  1.5 covers
# only a mismatched extension; the name rules themselves had no coverage.
# =====================================================================

@testcase("18", "invalid_member_names", requires="mvs")
def invalid_member_names(ctx):
    """Names the PDS cannot hold must be refused, not silently mangled.

    Truncating an over-long name to 8 characters would be the dangerous
    outcome: two different files would collide on one member and the second
    would overwrite the first."""
    ctx.require_mvs()
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)

    cases_ = [
        ("TOOLONGNAME", "over 8 characters"),
        ("1ABC",        "starts with a digit"),
        ("BAD-CHR",     "contains a hyphen"),
    ]
    for stem, why in cases_:
        p = str(ctx.pds_dir(key) / ctx.filename(key, stem))
        try:
            with open(p, "w", newline="\n") as f:
                f.write("x\n")
        except OSError:
            continue                          # refused: correct
        ctx.track(key, ctx.mvs_name(stem))
        raise AssertionError(
            "creating %r (%s) SUCCEEDED; the server must refuse it rather"
            " than mangle it into a valid member name" % (stem, why))

    # Control: a name that IS valid still works, so the test above is not
    # passing merely because every create fails.
    good = "GOODNM"
    ctx.reset(key, good)
    text = ctx.gen(key, "small")
    ctx.write_member(key, good, text)
    ctx.verify_member(key, good, text)


# =====================================================================
# 19. Reported size (GETATTR / the file-size cache)
#
# mvsfsz.c exists solely to report a member's true TEXT-mode size, and
# nothing checked it.  A wrong size is not cosmetic: clients use it to
# decide how much to read, so a short value silently truncates a file.
# =====================================================================

def _stat_size(ctx, key, name):
    return os.stat(str(ctx.member_path(key, name))).st_size


@testcase("19.1", "stat_size_matches_content")
def stat_size_matches_content(ctx):
    """st_size must equal the bytes a client actually reads back.

    Compared against the READ rather than against the text written, because
    those legitimately differ: an FB record is padded to LRECL and the pad
    is stripped on the way out.  What a client cannot survive is st_size
    disagreeing with the readable length."""
    for key in _text_keys(ctx):
        for kind, name in (("small", "SZSMALL"), ("large", "SZLARGE")):
            text = ctx.gen(key, kind)
            ctx.reset(key, name)
            ctx.write_member(key, name, text)
            ctx.verify_now(key, name, text)      # ensure it is stowed

            got  = ctx.read_member(key, name)
            size = _stat_size(ctx, key, name)
            ctx.check(size == len(got),
                      "%s(%s): st_size %d but a read returns %d bytes"
                      % (ctx.ds(key)["nfs_dir"], name, size, len(got)))
            ctx.check(size > 0, "%s(%s): st_size is 0 for a %s member"
                      % (ctx.ds(key)["nfs_dir"], name, kind))


@testcase("19.2", "stat_size_while_pending")
def stat_size_while_pending(ctx):
    """The size of a member still buffered in the write pool.

    Between the write and the flush the member is not in the PDS at all, so
    both the stat and the read are served from the pending-member pool.  They
    still have to agree -- a client that reads immediately after writing
    (2.2 does exactly that) depends on it."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    name = "SZPEND"
    text = ctx.gen(key, "small")

    ctx.reset(key, name)
    ctx.write_member(key, name, text)         # deliberately NO settle/verify

    size = _stat_size(ctx, key, name)
    got  = ctx.read_member(key, name)
    ctx.check(size == len(got),
              "pending %s(%s): st_size %d but a read returns %d bytes"
              % (ctx.ds(key)["nfs_dir"], name, size, len(got)))
    ctx.verify_member(key, name, text)        # and it still stows correctly


# =====================================================================
# 20. Concurrent writers to the SAME member
#
# 1.4 covers distinct members.  This is the other case: several writers
# funnelled through ONE pending slot.
#
# NOTE ON WHAT THIS DOES *NOT* COVER.  The SPFEDIT enqueue -> EACCES path is
# NOT reachable from here.  pww_lock() takes that enqueue once per SLOT, and a
# second NFS writer for the same member finds the existing slot via
# pww_slot_find() and shares it -- no second enqueue, no conflict.  EACCES
# fires only when something OUTSIDE the server holds the member (an ISPF edit
# session, another job), which the harness cannot arrange.  That path stays a
# manual check: open the member in ISPF, then try to write it over NFS.
# =====================================================================

@testcase("20", "concurrent_same_member")
def concurrent_same_member(ctx):
    """Several threads write the SAME member at once; it must stay coherent.

    Every writer sends IDENTICAL content on purpose.  Two clients writing one
    file is genuinely racy at the protocol level -- each open truncates, and
    the WRITEs interleave -- so 'which writer won' is not a property the
    server owes us.  With identical payloads any interleaving still yields
    those same bytes, which leaves exactly one thing being asserted: that
    funnelling concurrent writers through one pending slot does not corrupt
    it.  A slot-state bug shows up as a short, mixed or unreadable member."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    name    = "CSAME"
    payload = ctx.gen(key, "small")
    errors  = []

    ctx.reset(key, name)
    ctx.track(key, name)

    def worker():
        try:
            with open(str(ctx.member_path(key, name)), "w", newline="\n") as f:
                f.write(payload)
        except Exception as e:                       # noqa
            errors.append(repr(e))

    threads = [threading.Thread(target=worker) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    ctx.check(not errors,
              "concurrent writers to one member failed: %s" % errors)
    ctx.verify_member(key, name, payload, updated=True)


# =====================================================================
# 21. Pending-pool exhaustion (LRU eviction)
#
# The pool holds PWW_MAX_PENDING (8) members.  Asking for a ninth slot makes
# pww_slot_take() flush and release the least-recently-used one, then reuse
# it -- a flush that happens with no client request in flight.  1.4 uses five
# members and never reaches it.
# =====================================================================

@testcase("21", "pool_eviction")
def pool_eviction(ctx):
    """Write more members at once than the pool holds; all must survive.

    Written back-to-back with no pause, so the earlier members are still
    pending when the later ones claim slots and force the eviction.  On a
    FILE_SYNC client (Windows) nothing sends COMMIT, so they stay pending
    until the idle sweep -- which makes the eviction path reliable there
    rather than merely likely.

    If the server is fast enough to have flushed them all first, no eviction
    happens and the test still passes: it cannot fail spuriously, it just
    covers less.  Content is what is checked, because a slot reused while its
    previous member was still dirty loses or mixes data."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    n     = int(ctx.opts.get("pool_members", 16))    # 2x PWW_MAX_PENDING
    names = ["EVICT%02d" % i for i in range(n)]

    ctx.reset(key, *names)
    payloads = {}
    for nm in names:
        ctx.track(key, nm)
        # Distinct per member: cross-talk between two slots shows as a
        # mismatch rather than passing because the bytes happened to match.
        payloads[nm] = ("OWNER %s\n" % nm) + ctx.gen(key, "small")

    for nm in names:                                 # no pause: fill the pool
        ctx.write_member(key, nm, payloads[nm], track=False)

    for nm in names:
        ctx.verify_member(key, nm, payloads[nm])


# =====================================================================
# 22. Deleting a member that is still PENDING
#
# vfs_remove() calls pww_discard() BEFORE unlinking, precisely so a flush
# that has not happened yet cannot put the member back afterwards.  Nothing
# tested that ordering: 5.1 deletes a member whose state is unknown.
# =====================================================================

def _assert_gone(ctx, key, name, how):
    """Assert the member is absent AND stays absent past the idle sweep."""
    ctx.check(ctx.wait_exists(key, name, False),
              "%s: %s still exists after the delete" % (how, name))
    ctx.settle()                       # let any queued flush run
    ctx.check(ctx.wait_exists(key, name, False),
              "%s: %s came BACK after the delete -- a pending flush"
              " resurrected it" % (how, name))


@testcase("22.1", "delete_pending_new")
def delete_pending_new(ctx):
    """Delete a brand-new member before it is ever stowed."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    name = "DELPND"

    ctx.reset(key, name)
    ctx.write_member(key, name, ctx.gen(key, "small"), track=False)
    ctx.remove_member(key, name)               # still pending, never stowed
    _assert_gone(ctx, key, name, "new member")


@testcase("22.2", "delete_pending_rewrite")
def delete_pending_rewrite(ctx):
    """Delete a member that EXISTS on disk and is being rewritten.

    The harder of the two: there is a real directory entry to remove as well
    as buffered content to discard, so a flush that outlived the delete would
    re-create a member that was genuinely there before."""
    keys = _text_keys(ctx)
    key  = keys[0] if keys else "fb"
    _have(ctx, key)
    name = "DELPND2"

    ctx.reset(key, name)
    first = _alt_text(ctx, key, 6, "DPFIRST")
    ctx.write_member(key, name, first, track=False)
    ctx.verify_now(key, name, first)           # force it into the PDS

    second = _alt_text(ctx, key, 20, "DPSECND")
    ctx.write_member(key, name, second, track=False)   # now pending again
    ctx.remove_member(key, name)
    _assert_gone(ctx, key, name, "rewritten member")
