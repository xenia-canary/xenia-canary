#!/usr/bin/env python3

"""Compiles a single shader to SPIR-V and generates a C header.

Usage: compile_shader_spirv.py <input_path> <output_path>

Pipeline:
  1. glslangValidator -> unoptimized .spv
  2. spirv-opt -> optimized .spv
  3. spirv-dis -> disassembly .txt
  4. Generate .h with disassembly comment + uint32_t array
"""

import os
import struct
import subprocess
import sys


SPIRV_STAGES = {
    "vs": "vert", "hs": "tesc", "ds": "tese",
    "gs": "geom", "ps": "frag", "cs": "comp",
}

XESL_WRAPPER = (
    "#version 460\n"
    "#extension GL_EXT_control_flow_attributes : require\n"
    "#extension GL_EXT_samplerless_texture_functions : require\n"
    "#extension GL_GOOGLE_include_directive : require\n"
    "#include \"%s\"\n"
)


def find_vulkan_tools():
    """Find Vulkan SDK tools via VULKAN_SDK env or PATH."""
    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk:
        bin_dir = os.path.join(vulkan_sdk, "bin")
        if os.path.isdir(bin_dir):
            return (
                os.path.join(bin_dir, "glslangValidator"),
                os.path.join(bin_dir, "spirv-opt"),
                os.path.join(bin_dir, "spirv-dis"),
            )

    # Fall back to PATH
    return ("glslangValidator", "spirv-opt", "spirv-dis")


def parse_stage(filename):
    """Extract the 2-char shader stage from filename like 'foo.cs.xesl'."""
    basename = os.path.splitext(filename)[0]  # 'foo.cs'
    identifier = basename.replace(".", "_")    # 'foo_cs'
    stage_key = identifier[-2:]
    if stage_key not in SPIRV_STAGES:
        return None, None
    return stage_key, SPIRV_STAGES[stage_key]


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_path> <output_path>", file=sys.stderr)
        return 1

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    src_name = os.path.basename(input_path)
    src_dir = os.path.dirname(input_path)
    src_is_xesl = src_name.endswith(".xesl")

    stage_key, spirv_stage = parse_stage(src_name)
    if spirv_stage is None:
        print(f"ERROR: cannot determine shader stage from: {src_name}", file=sys.stderr)
        return 1

    # Compute identifier (matches what Lua does: basename with dots -> underscores)
    identifier = os.path.splitext(src_name)[0].replace(".", "_")

    glslang, spirv_opt, spirv_dis = find_vulkan_tools()

    # Create output directory if needed.
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    # Temp file paths next to output.
    base = os.path.splitext(output_path)[0]
    glslang_spv = base + ".glslang.spv"
    opt_spv = base + ".spv"
    dis_txt = base + ".txt"

    try:
        # Step 1: glslangValidator
        glslang_args = [
            glslang,
            "--stdin" if src_is_xesl else input_path,
            "-DSHADING_LANGUAGE_GLSL_XE=1",
            "-S", spirv_stage,
            "-o", glslang_spv,
            "-V",
        ]
        if src_is_xesl:
            glslang_args.append(f"-I{src_dir}")

        stdin_data = (XESL_WRAPPER % src_name) if src_is_xesl else None
        result = subprocess.run(glslang_args, input=stdin_data, text=True)
        if result.returncode != 0:
            print(f"ERROR: glslangValidator failed for {src_name}", file=sys.stderr)
            return 1

        # Step 2: spirv-opt
        result = subprocess.run([
            spirv_opt, "-O", "-O", "--canonicalize-ids",
            glslang_spv, "-o", opt_spv,
        ])
        if result.returncode != 0:
            print(f"ERROR: spirv-opt failed for {src_name}", file=sys.stderr)
            return 1

        # Step 3: spirv-dis
        result = subprocess.run([spirv_dis, "-o", dis_txt, opt_spv])
        if result.returncode != 0:
            print(f"ERROR: spirv-dis failed for {src_name}", file=sys.stderr)
            return 1

        # Step 4: Generate header
        with open(output_path, "w") as out:
            out.write("// Generated with `xb buildshaders`.\n#if 0\n")
            with open(dis_txt, "r") as dis_file:
                dis_data = dis_file.read()
                if dis_data:
                    out.write(dis_data)
                    if dis_data[-1] != "\n":
                        out.write("\n")
            out.write("#endif\n\nconst uint32_t %s[] = {" % identifier)
            with open(opt_spv, "rb") as spv_file:
                index = 0
                while True:
                    word = spv_file.read(4)
                    if len(word) == 0:
                        break
                    if len(word) != 4:
                        print("ERROR: SPIR-V binary is misaligned", file=sys.stderr)
                        return 1
                    if index % 6 == 0:
                        out.write("\n    ")
                    else:
                        out.write(" ")
                    index += 1
                    value = struct.unpack("<I", word)[0]
                    out.write("0x%08X," % value)
            out.write("\n};\n")

    finally:
        # Clean up intermediate files.
        for f in (glslang_spv, opt_spv, dis_txt):
            if os.path.exists(f):
                os.remove(f)

    return 0


if __name__ == "__main__":
    sys.exit(main())
