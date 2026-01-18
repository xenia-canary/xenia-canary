if not os.istarget("macosx") then
  return
end

group("third_party")
project("dxilconv")
  uuid("9b1b4a3d-5c3b-4c1e-8db5-4c1e9937f2f8")
  kind("Utility")
  language("C++")

  local dxilconv_root =
      path.getabsolute("third_party/DirectXShaderCompiler", _MAIN_SCRIPT_DIR)
  local dxilconv_build_arm64 =
      path.join(dxilconv_root, "build_dxilconv_macos")
  local dxilconv_build_x86_64 =
      path.join(dxilconv_root, "build_dxilconv_macos_x86_64")
  local directx_headers_dir =
      path.getabsolute("third_party/directx-headers/include", _MAIN_SCRIPT_DIR)
  local cxx_flags = "-stdlib=libc++ -Wno-deprecated-declarations "
      .. "-Wno-deprecated"

  local function quote(value)
    return "\"" .. value .. "\""
  end

  local function cmake_config_cmd(build_dir, arch, target_triple)
    local args = {
      "cmake",
      "-S", quote(dxilconv_root),
      "-B", quote(build_dir),
      "-DD3D12_macOS_INCLUDE_DIR=" .. quote(directx_headers_dir),
      "-DCMAKE_BUILD_TYPE=Release",
      "-DCMAKE_OSX_ARCHITECTURES=" .. arch,
      "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0",
      "-DLLVM_TARGETS_TO_BUILD=None",
      "-DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=",
      "-DLLVM_DEFAULT_TARGET_TRIPLE=" .. target_triple,
      "-DLLVM_ENABLE_THREADS=ON",
      "-DLLVM_ENABLE_PIC=ON",
      "-DLLVM_BUILD_32_BITS=OFF",
      "-DBUILD_SHARED_LIBS=OFF",
      "-DLLVM_OPTIMIZED_TABLEGEN=ON",
      "-DLLVM_USE_INTEL_JITEVENTS=OFF",
      "-DLLVM_ENABLE_ZLIB=ON",
      "-DLLVM_ENABLE_LIBXML2=OFF",
      "-DCLANG_BUILD_EXAMPLES=OFF",
      "-DLLVM_INCLUDE_TESTS=OFF",
      "-DLLVM_INCLUDE_EXAMPLES=OFF",
      "-DLLVM_INCLUDE_DOCS=OFF",
      "-DCLANG_INCLUDE_TESTS=OFF",
      "-DHLSL_INCLUDE_TESTS=OFF",
      "-DENABLE_SPIRV_CODEGEN=OFF",
      "-DSPIRV_BUILD_TESTS=OFF",
      "-DCLANG_ENABLE_STATIC_ANALYZER=OFF",
      "-DCLANG_ENABLE_ARCMT=OFF",
      "-DLLVM_ENABLE_BINDINGS=OFF",
      "-DLLVM_ENABLE_EH=ON",
      "-DLLVM_ENABLE_RTTI=ON",
      "-DLLVM_REQUIRES_EH=ON",
      "-DLLVM_REQUIRES_RTTI=ON",
      "-DCMAKE_C_COMPILER=clang",
      "-DCMAKE_CXX_COMPILER=clang++",
      "-DCMAKE_CXX_STANDARD=17",
      "-DCMAKE_CXX_STANDARD_REQUIRED=ON",
      "-DCMAKE_CXX_EXTENSIONS=OFF",
      "-DCMAKE_C_FLAGS=" .. quote(""),
      "-DCMAKE_CXX_FLAGS=" .. quote(cxx_flags),
      "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
      "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    }
    return table.concat(args, " ")
  end

  local function cmake_build_cmd(build_dir, target)
    local args = {
      "cmake --build", quote(build_dir),
      "--target", target,
      "--config Release",
      "-j$(sysctl -n hw.ncpu)",
    }
    return table.concat(args, " ")
  end

  local function dxilconv_build_cmd(build_dir, arch, target_triple)
    local dylib = path.join(build_dir, "lib/libdxilconv.dylib")
    local dxbc2dxil = path.join(build_dir, "bin/dxbc2dxil")
    local commands = {
      "if [ ! -f " .. quote(dylib) .. " ] || [ ! -f "
          .. quote(dxbc2dxil) .. " ]; then",
      "mkdir -p " .. quote(build_dir) .. ";",
      cmake_config_cmd(build_dir, arch, target_triple) .. ";",
      cmake_build_cmd(build_dir, "dxilconv") .. ";",
      cmake_build_cmd(build_dir, "dxbc2dxil") .. ";",
      "fi",
    }
    return table.concat(commands, " ")
  end

  filter({"system:macosx", "architecture:arm64"})
    prebuildmessage("Building dxilconv (dxbc2dxil)")
    prebuildcommands({
      dxilconv_build_cmd(dxilconv_build_arm64, "arm64",
                         "arm64-apple-darwin"),
    })
  filter({"system:macosx", "architecture:x86_64"})
    prebuildmessage("Building dxilconv (dxbc2dxil)")
    prebuildcommands({
      dxilconv_build_cmd(dxilconv_build_x86_64, "x86_64",
                         "x86_64-apple-darwin"),
    })
  filter({})
