# nui-trace

Parser of xenia logs produced with `--nui_telemetry`. Turns the firehose of
`[nui] FuncName(...)` lines into four artifacts that are useful for figuring
out a title's NUI bootstrap path.

## Outputs

| file | what |
|---|---|
| `summary.txt` | functions ranked by call frequency (desc) |
| `init_sequence.txt` | first N **unique** calls in observed order |
| `mermaid_seq.md` | Mermaid `sequenceDiagram` of first N calls — paste into a GitHub issue/comment |
| `diff_a_vs_b.txt` | symmetric diff (only-in-A / only-in-B / count delta) — only when both `--trace-a` and `--trace-b` are supplied |

When `--trace-b` is supplied, the B-side `summary` / `init_sequence` /
`mermaid_seq` are also written, suffixed with the trace name.

## Usage

Single trace:

```cmd
python tools\nui-trace\parser.py --trace-a trace_a.log --out-dir out
```

Diff two traces — the standard Stage 2 capture pair (no init vs. fooled into
init):

```cmd
xenia.exe --nui_telemetry --log_file=trace_a.log path\to\title\default.xex
xenia.exe --nui_telemetry --allow_nui_initialization=true --log_file=trace_b.log path\to\title\default.xex

python tools\nui-trace\parser.py --trace-a trace_a.log --trace-b trace_b.log --out-dir out
```

## Input format

Lines are matched on the literal `[nui] ` token followed by an identifier and
`(`. Anything else is ignored, so a full xenia log file can be passed in
directly — no pre-filtering needed.

## Dependencies

Stdlib only. Tested on CPython 3.10+.
