"""
Minimal test registry and runner.

Tests register themselves with the @testcase decorator and are plain functions
that take a Context.  A test signals:
  - PASS  by returning normally,
  - SKIP  by raising TestSkip (e.g. an MVS-only test in plain mode),
  - FAIL  by raising AssertionError (or any other exception).

The runner keeps registration order, so the printed report follows the numbered
outline in the README.
"""

import time
import traceback


REGISTRY = []


class TestSkip(Exception):
    """Raised by a test (or ctx.require_mvs) to skip rather than fail."""


def testcase(section, name, requires=None):
    """Register a test.  'section' is the outline number (e.g. '1.2'); 'requires'
    may be 'mvs' to auto-skip when not running against a real MVS server."""
    def deco(fn):
        REGISTRY.append({"section": section, "name": name,
                         "requires": requires, "fn": fn})
        return fn
    return deco


def _selected(entry, filters, sections):
    if sections and not any(entry["section"] == s or entry["section"].startswith(s + ".")
                            for s in sections):
        return False
    if filters and not any(f.lower() in entry["name"].lower()
                           or f == entry["section"] for f in filters):
        return False
    return True


def run_all(ctx, filters=None, sections=None, repeat=1):
    """Run the selected tests against ctx.  Returns the process exit code.

    repeat > 1 loops the selected tests up to that many passes and STOPS at the
    first failure (leaving the offending member preserved, see Context) -- the
    way to catch an intermittent bug: e.g.
        run_tests.py -c config.json -f upload_small -f upload_large --repeat 40
    """
    passed = failed = skipped = 0
    failures = []

    ordered = sorted(REGISTRY, key=lambda e: [int(x) for x in e["section"].split(".")])
    selected = [e for e in ordered if _selected(e, filters, sections)]
    if not selected:
        print("no tests matched the given --section/--filter")
        return 0

    stop = False
    for it in range(1, repeat + 1):
        if repeat > 1:
            print("\n----- pass %d/%d -----" % (it, repeat))
        for entry in selected:
            label = "[%s] %s" % (entry["section"], entry["name"])
            if entry["requires"] == "mvs" and ctx.mode != "mvs":
                print("SKIP %-42s (MVS-only)" % label)
                skipped += 1
                continue
            t0 = time.time()
            try:
                entry["fn"](ctx)
                print("PASS %-42s (%.2fs)" % (label, time.time() - t0))
                passed += 1
            except TestSkip as e:
                print("SKIP %-42s (%s)" % (label, e))
                skipped += 1
            except Exception as e:
                print("FAIL %-42s (%.2fs) -- %s" % (label, time.time() - t0, e))
                failures.append(("pass %d: %s" % (it, label), traceback.format_exc()))
                failed += 1
                stop = (repeat > 1)   # loop-until-fail: stop at the first one
            finally:
                ctx.after_test()
            if stop:
                break
        if stop:
            print("\nfailure detected on pass %d -- stopping (member preserved)"
                  % it)
            break

    print("\n" + "=" * 60)
    tail = " over %d pass(es)" % it if repeat > 1 else ""
    print("Results: %d passed, %d failed, %d skipped%s"
          % (passed, failed, skipped, tail))
    if failures:
        print("-" * 60)
        for label, tb in failures:
            print("FAILURE %s\n%s" % (label, tb))
    return 1 if failed else 0
