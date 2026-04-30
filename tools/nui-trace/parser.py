#!/usr/bin/env python3
# Copyright 2026 Kinectix Project. Licensed under BSD-3-Clause.
"""
nui-trace parser — consumes Xenia logs produced with --nui_telemetry and emits:

  summary.txt       — functions ranked by call frequency (desc)
  init_sequence.txt — first N unique calls in observed order (the title's
                      NUI bootstrap path)
  diff_a_vs_b.txt   — symmetric diff between trace A and trace B (only when
                      both traces are supplied)
  mermaid_seq.md    — Mermaid sequenceDiagram of the first ~N calls, ready
                      to paste into a GitHub issue

Input lines look like (xenia XELOGI prefix is tolerated, not required):

    i> 00001234 [nui] XamNuiGetDeviceStatus(status_ptr=82A40C00)
    [nui] XamIsNuiUIActive()

Lines that do not contain the literal "[nui] " token are ignored, so it is
safe to pass a full xenia log file directly.

Stdlib-only by design — the user runs this from cmd.exe on Windows with the
system Python interpreter; no pip install required.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, OrderedDict
from pathlib import Path
from typing import Iterable, Iterator

# Matches "[nui] FuncName(" anywhere on the line. The function name is captured
# in group 1; arguments are intentionally not parsed — call frequency and order
# are the only things we care about for Stage 2 analysis.
NUI_LINE_RE = re.compile(r"\[nui\]\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def iter_calls(path: Path) -> Iterator[str]:
    """Yield function names in the order they appear in `path`."""
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = NUI_LINE_RE.search(line)
            if m:
                yield m.group(1)


def collect(path: Path) -> tuple[list[str], Counter[str]]:
    """Return (ordered call list, frequency counter) for a trace file."""
    calls: list[str] = list(iter_calls(path))
    return calls, Counter(calls)


def write_summary(out: Path, counter: Counter[str], label: str) -> None:
    total = sum(counter.values())
    unique = len(counter)
    width = max((len(name) for name in counter), default=10)
    lines = [
        f"# nui-trace summary — {label}",
        f"# total calls: {total}    unique functions: {unique}",
        "",
        f"{'function'.ljust(width)}  count",
        f"{'-' * width}  -----",
    ]
    for name, n in counter.most_common():
        lines.append(f"{name.ljust(width)}  {n}")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_init_sequence(out: Path, calls: Iterable[str], limit: int,
                        label: str) -> None:
    """First N *unique* calls preserving observed order."""
    seen: "OrderedDict[str, None]" = OrderedDict()
    for name in calls:
        if name not in seen:
            seen[name] = None
            if len(seen) >= limit:
                break
    lines = [
        f"# nui-trace init sequence — {label}",
        f"# first {len(seen)} unique calls (limit={limit})",
        "",
    ]
    for i, name in enumerate(seen, 1):
        lines.append(f"{i:3d}. {name}")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_mermaid(out: Path, calls: list[str], limit: int, label: str) -> None:
    """Mermaid sequenceDiagram of the first `limit` calls (not unique).

    Title is the actor that calls into XAM. Lines are flattened to
    `Title->>XAM: FuncName` so the diagram is readable when pasted into a
    GitHub comment.
    """
    chunk = calls[:limit]
    lines = [
        f"# Mermaid sequence — {label}",
        f"First {len(chunk)} calls in observed order (limit={limit}).",
        "",
        "```mermaid",
        "sequenceDiagram",
        "    autonumber",
        "    participant Title",
        "    participant XAM",
    ]
    for name in chunk:
        lines.append(f"    Title->>XAM: {name}")
    lines.append("```")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_diff(out: Path,
               counter_a: Counter[str], counter_b: Counter[str],
               label_a: str, label_b: str) -> None:
    """Symmetric diff: only-in-A, only-in-B, count delta for shared names."""
    keys_a = set(counter_a)
    keys_b = set(counter_b)
    only_a = sorted(keys_a - keys_b)
    only_b = sorted(keys_b - keys_a)
    shared = sorted(keys_a & keys_b)

    lines: list[str] = [
        f"# nui-trace diff — {label_a}  vs  {label_b}",
        f"# {label_a}: {sum(counter_a.values())} calls / "
        f"{len(counter_a)} unique",
        f"# {label_b}: {sum(counter_b.values())} calls / "
        f"{len(counter_b)} unique",
        "",
        f"## only in {label_a} ({len(only_a)})",
    ]
    if only_a:
        for name in only_a:
            lines.append(f"  - {name}  ({counter_a[name]})")
    else:
        lines.append("  (none)")

    lines += ["", f"## only in {label_b} ({len(only_b)})"]
    if only_b:
        for name in only_b:
            lines.append(f"  + {name}  ({counter_b[name]})")
    else:
        lines.append("  (none)")

    lines += ["", f"## count delta on shared functions ({len(shared)})",
              "## (positive = called more in B than in A)"]
    deltas = sorted(
        ((name, counter_b[name] - counter_a[name]) for name in shared),
        key=lambda kv: (-abs(kv[1]), kv[0]),
    )
    width = max((len(n) for n in shared), default=10)
    lines.append(f"{'function'.ljust(width)}  {label_a:>6}  {label_b:>6}  "
                 f"{'delta':>6}")
    lines.append(f"{'-' * width}  ------  ------  ------")
    for name, delta in deltas:
        lines.append(
            f"{name.ljust(width)}  "
            f"{counter_a[name]:>6}  {counter_b[name]:>6}  {delta:+6d}"
        )

    out.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="nui-trace parser",
        description="Parse xenia --nui_telemetry logs into reports.",
    )
    p.add_argument("--trace-a", required=True, type=Path,
                   help="Path to xenia log A (e.g. trace_a.log — no init).")
    p.add_argument("--trace-b", type=Path, default=None,
                   help="Path to xenia log B (e.g. trace_b.log — "
                        "allow_nui_initialization=true). Optional; "
                        "without it the diff step is skipped.")
    p.add_argument("--out-dir", type=Path, default=Path("out"),
                   help="Directory for report files (default: ./out).")
    p.add_argument("--init-count", type=int, default=50,
                   help="Max unique functions in init_sequence.txt "
                        "(default: 50).")
    p.add_argument("--seq-count", type=int, default=50,
                   help="Max calls in mermaid_seq.md (default: 50).")
    args = p.parse_args(argv)

    if not args.trace_a.is_file():
        print(f"error: trace-a not found: {args.trace_a}", file=sys.stderr)
        return 2
    if args.trace_b is not None and not args.trace_b.is_file():
        print(f"error: trace-b not found: {args.trace_b}", file=sys.stderr)
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)

    label_a = args.trace_a.stem
    calls_a, counter_a = collect(args.trace_a)
    if not calls_a:
        print(f"warning: no [nui] lines found in {args.trace_a}",
              file=sys.stderr)

    write_summary(args.out_dir / "summary.txt", counter_a, label_a)
    write_init_sequence(args.out_dir / "init_sequence.txt", calls_a,
                        args.init_count, label_a)
    write_mermaid(args.out_dir / "mermaid_seq.md", calls_a,
                  args.seq_count, label_a)

    if args.trace_b is not None:
        label_b = args.trace_b.stem
        calls_b, counter_b = collect(args.trace_b)
        if not calls_b:
            print(f"warning: no [nui] lines found in {args.trace_b}",
                  file=sys.stderr)
        # Append B-side reports next to A-side, suffixed.
        write_summary(args.out_dir / f"summary_{label_b}.txt",
                      counter_b, label_b)
        write_init_sequence(args.out_dir / f"init_sequence_{label_b}.txt",
                            calls_b, args.init_count, label_b)
        write_mermaid(args.out_dir / f"mermaid_seq_{label_b}.md",
                      calls_b, args.seq_count, label_b)
        write_diff(args.out_dir / "diff_a_vs_b.txt",
                   counter_a, counter_b, label_a, label_b)

    print(f"wrote reports to {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
