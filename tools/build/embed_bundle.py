#!/usr/bin/env python3
"""Pack a directory of files into a zlib-compressed concat blob and emit it
as a C++ byte array in embedded_<namespace>.{h,cc}.

Usage: embed_bundle.py <source_dir> <output_dir> <namespace>

Format (after decompression):
  [u32_le count]
  for each entry:
    [u32_le name_len][name bytes (utf-8)]
    [u32_le data_len][data bytes]
"""
import os
import struct
import sys
import zlib


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    src_dir, out_dir, ns = sys.argv[1], sys.argv[2], sys.argv[3]

    parts = [b""]  # placeholder for count
    count = 0
    if os.path.isdir(src_dir):
        # Recurse so nested layouts (e.g. locale/<lang>/<domain>.mo) keep
        # their structure in the entry name. Flat dirs land as basenames.
        files = []
        for root, _, fs in os.walk(src_dir):
            for f in fs:
                full = os.path.join(root, f)
                rel = os.path.relpath(full, src_dir).replace(os.sep, "/")
                files.append((rel, full))
        for rel, full in sorted(files):
            name = rel.encode("utf-8")
            with open(full, "rb") as f:
                data = f.read()
            parts.append(struct.pack("<I", len(name)))
            parts.append(name)
            parts.append(struct.pack("<I", len(data)))
            parts.append(data)
            count += 1
    else:
        print(f"warning: {src_dir} not found, emitting empty bundle",
              file=sys.stderr)
    parts[0] = struct.pack("<I", count)
    raw = b"".join(parts)
    compressed = zlib.compress(raw, 9)

    full_ns = f"xe::embedded_bundle_{ns}"
    os.makedirs(out_dir, exist_ok=True)
    header_path = os.path.join(out_dir, f"embedded_bundle_{ns}.h")
    source_path = os.path.join(out_dir, f"embedded_bundle_{ns}.cc")

    header = (
        f"// Auto-generated from {src_dir}.\n"
        f"#pragma once\n#include <cstddef>\n"
        f"namespace {full_ns} {{\n"
        f"extern const unsigned char kBundleData[];\n"
        f"extern const size_t kBundleSize;\n"
        f"}}  // namespace {full_ns}\n"
    )

    rows = []
    for i in range(0, len(compressed), 16):
        row = compressed[i:i + 16]
        rows.append("  " + ",".join(f"0x{b:02x}" for b in row) + ",")
    source = (
        f"// Auto-generated from {src_dir}: {count} files, "
        f"{len(raw)} raw, {len(compressed)} compressed.\n"
        f'#include "embedded_bundle_{ns}.h"\n'
        f"namespace {full_ns} {{\n"
        f"const unsigned char kBundleData[] = {{\n"
        + "\n".join(rows) + "\n"
        f"}};\n"
        f"const size_t kBundleSize = sizeof(kBundleData);\n"
        f"}}  // namespace {full_ns}\n"
    )

    # Only rewrite if content changed so make doesn't recompile the multi-MB
    # source on every reconfigure.
    def write_if_changed(path, content):
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                if f.read() == content:
                    return
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)

    write_if_changed(header_path, header)
    write_if_changed(source_path, source)


if __name__ == "__main__":
    main()
