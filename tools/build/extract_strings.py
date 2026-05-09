#!/usr/bin/env python3
"""Extract translatable strings from C++ source into a gettext .pot template.

Usage: extract_strings.py <output.pot> <source-file-or-glob> [...]

Recognises calls of the form _("...") and wxTRANSLATE("..."), including
multi-segment string-literal concatenation (e.g. _("foo " "bar")). Skips
content inside line/block comments and inside other string literals.

Pure stdlib so we don't take a system gettext dep — gettext's xgettext is not
guaranteed to be on Windows build machines.
"""
import datetime
import glob
import os
import re
import sys


_KEYWORDS = ("_", "wxTRANSLATE", "wxPLURAL")

# A "string literal token" — handles common escapes and adjacent concatenation.
_STR = re.compile(r'"((?:\\.|[^"\\])*)"', re.DOTALL)
_KEYWORD_CALL = re.compile(
    r"\b(" + "|".join(re.escape(k) for k in _KEYWORDS) + r")\s*\("
)


def _strip_noncode(src):
    """Replace comments and string contents with spaces so keyword matching
    won't pick up text inside `// _("...")` or `"x = _(...)"`. Preserves byte
    offsets so line numbers from finditer remain accurate."""
    out = list(src)
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            # Line comment — blank out to end-of-line.
            j = src.find("\n", i)
            if j < 0:
                j = n
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            # Block comment — blank out through "*/".
            j = src.find("*/", i + 2)
            if j < 0:
                j = n
            else:
                j += 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == '"':
            # Skip past the closing quote, accounting for escapes.
            j = i + 1
            while j < n:
                if src[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if src[j] == '"':
                    j += 1
                    break
                j += 1
            i = j
        elif c == "'":
            # Char literal — same treatment.
            j = i + 1
            while j < n:
                if src[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if src[j] == "'":
                    j += 1
                    break
                j += 1
            i = j
        else:
            i += 1
    return "".join(out)


def _line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def _scan_string_args(src, start):
    """Starting at the position right after a `(` of a recognised call, read a
    sequence of adjacent C++ string literals (separated only by whitespace /
    L / u / u8 / U prefixes) and return their concatenated logical content,
    plus the offset just past the last quote. Returns (None, start) if the
    first non-whitespace char isn't a string literal."""
    i = start
    n = len(src)

    def skip_ws():
        nonlocal i
        while i < n and src[i] in " \t\r\n":
            i += 1
        # Skip an optional encoding prefix (L"...", u"...", u8"...", U"...").
        if i < n and src[i] in "LuU":
            j = i + 1
            if j < n and src[j] == "8":
                j += 1
            if j < n and src[j] == '"':
                i = j

    parts = []
    skip_ws()
    if i >= n or src[i] != '"':
        return None, start
    while i < n and src[i] == '"':
        m = _STR.match(src, i)
        if not m:
            return None, start
        parts.append(_unescape(m.group(1)))
        i = m.end()
        skip_ws()
    return "".join(parts), i


def _unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({
                "n": "\n", "t": "\t", "r": "\r",
                "\\": "\\", '"': '"', "'": "'",
                "0": "\0",
            }.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _po_escape(s):
    return (s.replace("\\", "\\\\")
             .replace('"', '\\"')
             .replace("\t", "\\t")
             .replace("\r", "\\r")
             .replace("\n", "\\n"))


def _format_msgid(msgid):
    # If the string contains \n in the middle, emit it as multi-line for
    # readability matching xgettext's output.
    escaped = _po_escape(msgid)
    if "\\n" in escaped and not escaped.endswith("\\n"):
        # Multi-line form: empty initial "", then one "...\n" per segment.
        segs = escaped.split("\\n")
        lines = ['""']
        for s in segs[:-1]:
            lines.append(f'"{s}\\n"')
        if segs[-1]:
            lines.append(f'"{segs[-1]}"')
        return "\n".join(lines)
    return f'"{escaped}"'


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    out_path = sys.argv[1]
    inputs = []
    for arg in sys.argv[2:]:
        # Both literal paths and globs work; on Windows shells don't expand
        # globs by default so we expand here.
        matched = glob.glob(arg, recursive=True)
        inputs.extend(matched if matched else [arg])

    # msgid -> list of (file, line) references.
    entries = {}
    for path in sorted(set(inputs)):
        if not os.path.isfile(path):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            src = f.read()
        stripped = _strip_noncode(src)
        for m in _KEYWORD_CALL.finditer(stripped):
            arg, _end = _scan_string_args(stripped, m.end())
            if arg is None:
                continue
            line = _line_of(src, m.start())
            ref = f"{path}:{line}"
            entries.setdefault(arg, []).append(ref)

    today = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%d %H:%M+0000")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Translatable strings for xenia-edge.\n")
        f.write("# Generated by tools/build/extract_strings.py — do not edit.\n")
        f.write("#\n")
        f.write('msgid ""\n')
        f.write('msgstr ""\n')
        f.write('"Project-Id-Version: xenia-edge\\n"\n')
        f.write(f'"POT-Creation-Date: {today}\\n"\n')
        f.write('"MIME-Version: 1.0\\n"\n')
        f.write('"Content-Type: text/plain; charset=UTF-8\\n"\n')
        f.write('"Content-Transfer-Encoding: 8bit\\n"\n')
        f.write("\n")

        # Stable order: by msgid for diffability.
        for msgid in sorted(entries):
            for ref in entries[msgid]:
                # Forward slashes in references for cross-platform diff stability.
                f.write(f"#: {ref.replace(os.sep, '/')}\n")
            f.write(f"msgid {_format_msgid(msgid)}\n")
            f.write('msgstr ""\n')
            f.write("\n")


if __name__ == "__main__":
    main()
