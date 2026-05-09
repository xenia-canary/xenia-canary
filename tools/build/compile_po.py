#!/usr/bin/env python3
"""Compile a gettext .po file into the binary .mo format.

Usage: compile_po.py <input.po> <output.mo>

Implements the GNU MO format documented at
https://www.gnu.org/software/gettext/manual/html_node/MO-Files.html so we
don't take a build-time dependency on the gettext msgfmt CLI.
"""
import struct
import sys


def _parse_po(path):
    """Yield (msgid, msgstr) pairs from a .po file. Skips fuzzy entries and
    the empty-msgid header. Plural forms (msgid_plural) are not supported —
    extend here when we need them."""
    msgid = None
    msgstr = None
    msgctxt = None
    flags = set()
    in_id = in_str = False

    def flush(out):
        nonlocal msgid, msgstr, msgctxt, flags, in_id, in_str
        if msgid is not None and msgstr is not None and msgid != "":
            if "fuzzy" not in flags:
                key = ("\x04".join((msgctxt, msgid))
                       if msgctxt is not None else msgid)
                out.append((key, msgstr))
        msgid = msgstr = msgctxt = None
        flags = set()
        in_id = in_str = False

    out = []
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line or line.startswith("#~"):
                # Blank or obsolete entry separator.
                if not line:
                    flush(out)
                continue
            if line.startswith("#,"):
                flags.update(s.strip() for s in line[2:].split(","))
                continue
            if line.startswith("#"):
                continue
            if line.startswith("msgctxt "):
                flush(out)
                msgctxt = _unquote(line[len("msgctxt "):])
                in_id = in_str = False
                continue
            if line.startswith("msgid "):
                if msgstr is not None:
                    flush(out)
                msgid = _unquote(line[len("msgid "):])
                in_id, in_str = True, False
                continue
            if line.startswith("msgstr "):
                msgstr = _unquote(line[len("msgstr "):])
                in_id, in_str = False, True
                continue
            if line.startswith('"') and line.endswith('"'):
                chunk = _unquote(line)
                if in_id:
                    msgid = (msgid or "") + chunk
                elif in_str:
                    msgstr = (msgstr or "") + chunk
                continue
        flush(out)
    return out


def _unquote(s):
    s = s.strip()
    if not (s.startswith('"') and s.endswith('"')):
        raise ValueError(f"expected quoted string, got: {s!r}")
    s = s[1:-1]
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            out.append({
                "n": "\n", "t": "\t", "r": "\r",
                "\\": "\\", '"': '"', "0": "\0",
            }.get(nxt, nxt))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    in_path, out_path = sys.argv[1], sys.argv[2]
    entries = _parse_po(in_path)
    # Empty-msgid header is required; wxLocale uses it to detect UTF-8.
    entries.insert(0, ("", "Content-Type: text/plain; charset=UTF-8\n"))
    entries.sort(key=lambda kv: kv[0].encode("utf-8"))

    keys = [k.encode("utf-8") for k, _ in entries]
    vals = [v.encode("utf-8") for _, v in entries]
    n = len(entries)
    header_size = 28
    keys_table = []
    vals_table = []
    keys_blob = bytearray()
    vals_blob = bytearray()
    keys_start = header_size + 16 * n
    for k in keys:
        keys_table.append((len(k), keys_start + len(keys_blob)))
        keys_blob += k + b"\x00"
    vals_start = keys_start + len(keys_blob)
    for v in vals:
        vals_table.append((len(v), vals_start + len(vals_blob)))
        vals_blob += v + b"\x00"

    with open(out_path, "wb") as f:
        f.write(struct.pack(
            "<IIIIIII",
            0x950412DE,             # magic
            0,                      # version
            n,                      # nstrings
            header_size,            # orig_offset
            header_size + 8 * n,    # trans_offset
            0,                      # hash_size (omitted)
            0,                      # hash_offset
        ))
        for length, off in keys_table:
            f.write(struct.pack("<II", length, off))
        for length, off in vals_table:
            f.write(struct.pack("<II", length, off))
        f.write(keys_blob)
        f.write(vals_blob)


if __name__ == "__main__":
    main()
