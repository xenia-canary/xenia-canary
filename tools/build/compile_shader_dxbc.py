#!/usr/bin/env python3

"""Compiles a single shader to DXBC and generates a C header via FXC.

Usage: compile_shader_dxbc.py <input_path> <output_path>

FXC directly produces the .h file — no post-processing needed.
"""

import os
import subprocess
import sys
from glob import glob


DXBC_STAGES = ["vs", "hs", "ds", "gs", "ps", "cs"]


def find_fxc():
    """Find FXC via FXC_PATH env or Windows SDK search."""
    fxc = os.environ.get("FXC_PATH")
    if fxc:
        return fxc

    # Search Windows Kits.
    program_files_x86 = os.environ.get("ProgramFiles(x86)", "")
    candidates = glob(os.path.join(
        program_files_x86, "Windows Kits", "10", "bin", "*", "x64", "fxc.exe"))
    if candidates:
        return candidates[-1]  # Highest version is last

    return None


def parse_stage(filename):
    """Extract the 2-char shader stage from filename like 'foo.cs.xesl'."""
    basename = os.path.splitext(filename)[0]  # 'foo.cs'
    identifier = basename.replace(".", "_")    # 'foo_cs'
    stage_key = identifier[-2:]
    if stage_key not in DXBC_STAGES:
        return None
    return stage_key


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_path> <output_path>", file=sys.stderr)
        return 1

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    src_name = os.path.basename(input_path)
    src_dir = os.path.dirname(input_path)

    stage = parse_stage(src_name)
    if stage is None:
        print(f"ERROR: cannot determine shader stage from: {src_name}", file=sys.stderr)
        return 1

    identifier = os.path.splitext(src_name)[0].replace(".", "_")

    fxc = find_fxc()
    if not fxc:
        print("ERROR: could not find fxc! Set FXC_PATH environment variable "
              "or install Windows SDK.", file=sys.stderr)
        return 1

    # Create output directory if needed.
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    is_dxc = "dxc" in os.path.basename(fxc).lower()

    # Start with base command — use wine on non-Windows platforms.
    if sys.platform != "win32":
        compiler_args = ["wine", fxc]
    else:
        compiler_args = [fxc]

    if is_dxc:
        # DXC only supports SM 6.0+.
        compiler_args.extend([
            "-T", f"{stage}_6_0",
            "-HV", "2017",
            "-D", "SHADING_LANGUAGE_HLSL_XE=1",
            "-I", src_dir,
            "-Fh", output_path,
            "-Vn", identifier,
            "-nologo",
            input_path,
        ])
    else:
        # FXC uses traditional syntax.
        compiler_args.extend([
            "/D", "SHADING_LANGUAGE_HLSL_XE=1",
            "/I", src_dir,
            "/Fh", output_path,
            "/T", f"{stage}_5_1",
            "/Vn", identifier,
            "/O3",
            "/Qstrip_reflect",
            "/Qstrip_debug",
            "/Qstrip_priv",
            "/all_resources_bound",
            "/Gfp",
            "/nologo",
            input_path,
        ])

    result = subprocess.call(compiler_args, stdout=subprocess.DEVNULL)
    if result != 0:
        print(f"ERROR: failed to compile DXBC shader: {src_name}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
