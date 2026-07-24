"""
Text generation and comparison helpers for the dino-nfs integration tests.

Members are text.  A fixed-length (RECFM=FB) member is stored space-padded to
the record length, and both the NFS read path and an FTP text retrieval strip
those trailing blanks and normalise the line terminator.  So a byte-for-byte
comparison is the wrong tool: two texts are considered equal when, line by line,
they match after stripping trailing whitespace and ignoring trailing blank
lines.  Generated data therefore never contains meaningful trailing spaces.
"""

_ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"


def _line(i, width, tag):
    """One deterministic line of exactly <width> printable, non-space chars."""
    head = "%s%06d:" % (tag, i)
    if width <= len(head):
        return head[:width] if width > 0 else "X"
    body = "".join(_ALPHA[(i * 7 + j) % len(_ALPHA)] for j in range(width - len(head)))
    return head + body


def gen_fixed_lines(n, width, tag="ITF"):
    """n fixed-width lines (for RECFM=FB).  width must be <= the LRECL."""
    return "\n".join(_line(i, width, tag) for i in range(n)) + "\n"


def gen_var_lines(n, base=20, tag="ITV"):
    """n variable-width lines (for RECFM=VB): lengths cycle base..base+99."""
    return "\n".join(_line(i, base + (i % 100), tag) for i in range(n)) + "\n"


def normalize(text):
    """Split into lines, strip trailing whitespace, drop trailing blank lines."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = [ln.rstrip() for ln in text.split("\n")]
    while lines and lines[-1] == "":
        lines.pop()
    return lines


def diff_summary(expected, actual):
    """Return a short human description of the first difference, or ''."""
    e = normalize(expected)
    a = normalize(actual)
    if e == a:
        return ""
    if len(e) != len(a):
        head = "line count differs: expected %d, got %d" % (len(e), len(a))
    else:
        head = "content differs"
    for i in range(min(len(e), len(a))):
        if e[i] != a[i]:
            return "%s; first diff at line %d:\n    expected: %r\n    actual:   %r" % (
                head, i + 1, e[i][:80], a[i][:80])
    # same prefix, one is longer
    idx = min(len(e), len(a))
    longer = "expected" if len(e) > len(a) else "actual"
    extra = (e if len(e) > len(a) else a)[idx][:80] if idx < max(len(e), len(a)) else ""
    return "%s; %s has extra line %d: %r" % (head, longer, idx + 1, extra)


def text_equal(expected, actual):
    return normalize(expected) == normalize(actual)


def first_byte_diff(a, b):
    """Offset of the first differing raw byte, or -1 if a == b."""
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n if len(a) != len(b) else -1


def hexdump_around(data, offset, radius=32):
    """Hex of data[offset-radius : offset+radius] with the offset marked."""
    lo = max(0, offset - radius)
    hi = min(len(data), offset + radius)
    out = []
    for i in range(lo, hi):
        b = ord(data[i]) & 0xFF if isinstance(data, str) else data[i]
        out.append(("[%02x]" if i == offset else "%02x") % b)
    return "off %d, bytes[%d:%d]: %s" % (offset, lo, hi, " ".join(out))
