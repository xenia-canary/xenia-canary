# third_party/CMakeLists.txt - All third-party library targets.

# Suppress warnings for all third-party code.
if(MSVC)
  add_compile_options(/W0)
else()
  add_compile_options(-w)
endif()

# ==============================================================================
# xbyak_aarch64 (ARM64 JIT assembler)
# ==============================================================================
if(XE_TARGET_AARCH64)
  add_library(xbyak_aarch64 STATIC
    xbyak_aarch64/src/xbyak_aarch64_impl.cpp
    xbyak_aarch64/src/util_impl.cpp
  )
  target_include_directories(xbyak_aarch64 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/xbyak_aarch64
    ${CMAKE_CURRENT_SOURCE_DIR}/xbyak_aarch64/src
    ${CMAKE_CURRENT_SOURCE_DIR}/xbyak_aarch64/xbyak_aarch64
  )
  if(MSVC)
    target_compile_definitions(xbyak_aarch64 PRIVATE NOMINMAX)
    target_compile_options(xbyak_aarch64 PRIVATE /w)
  else()
    target_compile_options(xbyak_aarch64 PRIVATE -w)
  endif()
endif()

# ==============================================================================
# aes_128
# ==============================================================================
add_library(aes_128 STATIC
  aes_128/unroll/aes.c
)
target_include_directories(aes_128 PUBLIC aes_128)

# ==============================================================================
# capstone
# ==============================================================================
add_library(capstone STATIC
  capstone/cs.c
  capstone/MCInst.c
  capstone/MCInstPrinter.c
  capstone/MCInstrDesc.c
  capstone/MCRegisterInfo.c
  capstone/SStream.c
  capstone/utils.c
  capstone/Mapping.c
)
# Architecture-specific capstone files
if(XE_TARGET_AARCH64)
  file(GLOB _capstone_arch_c "capstone/arch/AArch64/*.c")
  file(GLOB _capstone_arch_h "capstone/arch/AArch64/*.h")
  file(GLOB _capstone_arch_inc "capstone/arch/AArch64/*.inc")
  target_sources(capstone PRIVATE ${_capstone_arch_c} ${_capstone_arch_h} ${_capstone_arch_inc})
  target_compile_definitions(capstone PRIVATE
    CAPSTONE_HAS_ARM64
    CAPSTONE_HAS_AARCH64
    CAPSTONE_USE_SYS_DYN_MEM
  )
else()
  file(GLOB _capstone_x86_c "capstone/arch/X86/*.c")
  file(GLOB _capstone_x86_h "capstone/arch/X86/*.h")
  file(GLOB _capstone_x86_inc "capstone/arch/X86/*.inc")
  list(FILTER _capstone_x86_c EXCLUDE REGEX "X86ATTInstPrinter\\.c$")
  list(FILTER _capstone_x86_inc EXCLUDE REGEX "reduce\\.inc$")
  target_sources(capstone PRIVATE ${_capstone_x86_c} ${_capstone_x86_h} ${_capstone_x86_inc})
  target_compile_definitions(capstone PRIVATE
    CAPSTONE_X86_ATT_DISABLE
    CAPSTONE_HAS_X86
    CAPSTONE_USE_SYS_DYN_MEM
  )
endif()
target_include_directories(capstone PUBLIC capstone/include PRIVATE capstone)
# Force all capstone sources to compile as C
file(GLOB_RECURSE _capstone_all_c "capstone/*.c")
set_source_files_properties(${_capstone_all_c} PROPERTIES LANGUAGE C)

# ==============================================================================
# cxxopts (header-only)
# ==============================================================================
add_library(cxxopts INTERFACE)
target_include_directories(cxxopts INTERFACE cxxopts/include)

# ==============================================================================
# tomlplusplus (header-only)
# ==============================================================================
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE tomlplusplus/include)

# ==============================================================================
# dxbc
# ==============================================================================
add_library(dxbc STATIC
  dxbc/DXBCChecksum.cpp
)
target_include_directories(dxbc PUBLIC dxbc)

# ==============================================================================
# discord-rpc
# ==============================================================================
add_library(discord-rpc STATIC
  discord-rpc/src/connection.h
  discord-rpc/src/discord_rpc.cpp
  discord-rpc/src/msg_queue.h
  discord-rpc/src/rpc_connection.cpp
  discord-rpc/src/rpc_connection.h
  discord-rpc/src/serialization.cpp
  discord-rpc/src/serialization.h
)
if(XE_TARGET_X86_64)
  target_compile_definitions(discord-rpc PRIVATE RAPIDJSON_SSE42)
elseif(XE_TARGET_ARM64)
  target_compile_definitions(discord-rpc PRIVATE RAPIDJSON_NEON)
endif()
target_include_directories(discord-rpc PUBLIC discord-rpc/include PRIVATE rapidjson/include)
if(WIN32)
  target_sources(discord-rpc PRIVATE
    discord-rpc/src/connection_win.cpp
    discord-rpc/src/discord_register_win.cpp
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(discord-rpc PRIVATE
    discord-rpc/src/connection_unix.cpp
    discord-rpc/src/discord_register_linux.cpp
  )
endif()

# ==============================================================================
# fmt
# ==============================================================================
add_library(fmt STATIC
  fmt/src/format.cc
  fmt/src/os.cc
)
target_include_directories(fmt PUBLIC fmt/include)

# ==============================================================================
# glslang-spirv
# ==============================================================================
file(GLOB _glslang_spirv_cpp "glslang/SPIRV/*.cpp")
file(GLOB _glslang_spirv_h "glslang/SPIRV/*.h")
file(GLOB _glslang_spirv_hpp "glslang/SPIRV/*.hpp11")
list(FILTER _glslang_spirv_cpp EXCLUDE REGEX "GlslangToSpv\\.cpp$")
add_library(glslang-spirv STATIC
  ${_glslang_spirv_cpp}
  ${_glslang_spirv_h}
  ${_glslang_spirv_hpp}
  glslang/glslang/Include/visibility.h
)
target_include_directories(glslang-spirv PUBLIC glslang)

# ==============================================================================
# imgui
# ==============================================================================
add_library(imgui STATIC
  imgui/imgui.cpp
  imgui/imgui_demo.cpp
  imgui/imgui_draw.cpp
  imgui/imgui_tables.cpp
  imgui/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC imgui PRIVATE stb)

# ==============================================================================
# mspack
# ==============================================================================
add_library(mspack STATIC
  mspack/logging.cc
  mspack/lzxd.c
  mspack/system.c
)
target_compile_definitions(mspack PRIVATE HAVE_CONFIG_H)
target_include_directories(mspack PUBLIC mspack)
target_link_libraries(mspack PUBLIC xenia-base)

# ==============================================================================
# pugixml
# ==============================================================================
add_library(pugixml STATIC
  pugixml/src/pugixml.cpp
)
target_include_directories(pugixml PUBLIC pugixml/src)

# ==============================================================================
# snappy
# ==============================================================================
# Generate snappy-stubs-public.h if missing
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/snappy/snappy-stubs-public.h")
  if(XE_TARGET_X86_64)
    set(_snappy_avx_flag "-DSNAPPY_REQUIRE_AVX=ON")
  else()
    set(_snappy_avx_flag "-DSNAPPY_REQUIRE_AVX=OFF")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND}
      -DSNAPPY_BUILD_TESTS=OFF
      -DSNAPPY_BUILD_BENCHMARKS=OFF
      ${_snappy_avx_flag}
      ${CMAKE_CURRENT_SOURCE_DIR}/snappy
      -B${CMAKE_CURRENT_SOURCE_DIR}/snappy
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
endif()
add_library(snappy STATIC
  snappy/snappy-internal.h
  snappy/snappy-sinksource.cc
  snappy/snappy-sinksource.h
  snappy/snappy-stubs-internal.cc
  snappy/snappy-stubs-internal.h
  snappy/snappy-stubs-public.h
  snappy/snappy.cc
  snappy/snappy.h
)
target_include_directories(snappy PUBLIC snappy)

# ==============================================================================
# xxhash
# ==============================================================================
add_library(xxhash STATIC
  xxhash/xxhash.c
)
target_include_directories(xxhash PUBLIC xxhash)

# ==============================================================================
# zstd
# ==============================================================================
file(GLOB_RECURSE _zstd_common "zstd/lib/common/*.c" "zstd/lib/common/*.h")
file(GLOB_RECURSE _zstd_compress "zstd/lib/compress/*.c" "zstd/lib/compress/*.h")
file(GLOB_RECURSE _zstd_decompress "zstd/lib/decompress/*.c" "zstd/lib/decompress/*.h")
add_library(zstd STATIC
  zstd/lib/zstd.h
  ${_zstd_common}
  ${_zstd_compress}
  ${_zstd_decompress}
)
target_include_directories(zstd PUBLIC zstd/lib PRIVATE zstd/lib/common)
target_compile_definitions(zstd PRIVATE
  XXH_NAMESPACE=ZSTD_
  ZSTD_LEGACY_SUPPORT=0
)
if(NOT MSVC)
  target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM)
endif()

# ==============================================================================
# zarchive
# ==============================================================================
add_library(zarchive STATIC
  zarchive/include/zarchive/zarchivecommon.h
  zarchive/include/zarchive/zarchivereader.h
  zarchive/include/zarchive/zarchivewriter.h
  zarchive/src/zarchivereader.cpp
  zarchive/src/zarchivewriter.cpp
  zarchive/src/sha_256.c
  zarchive/src/sha_256.h
)
target_include_directories(zarchive PUBLIC zarchive/include PRIVATE zstd/lib)
target_link_libraries(zarchive PUBLIC zstd)

# ==============================================================================
# zlib-ng
# ==============================================================================
# Generate headers if missing
set(_zlibng_needs_configure FALSE)
foreach(_hdr zlib-ng.h zconf-ng.h zlib_name_mangling-ng.h gzread.c)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/zlib-ng/${_hdr}")
    set(_zlibng_needs_configure TRUE)
    break()
  endif()
endforeach()
if(_zlibng_needs_configure)
  execute_process(
    COMMAND ${CMAKE_COMMAND}
      -DZLIB_ENABLE_TESTS=OFF
      -DWITH_GTEST=OFF
      ${CMAKE_CURRENT_SOURCE_DIR}/zlib-ng
      -B${CMAKE_CURRENT_SOURCE_DIR}/zlib-ng
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
endif()
file(GLOB _zlibng_c "zlib-ng/*.c")
file(GLOB _zlibng_generic "zlib-ng/arch/generic/*.c")
if(XE_TARGET_AARCH64)
  file(GLOB _zlibng_arch "zlib-ng/arch/arm/*.c")
  add_library(zlib-ng STATIC
    ${_zlibng_c}
    ${_zlibng_arch}
    ${_zlibng_generic}
  )
  target_compile_definitions(zlib-ng PRIVATE
    WITH_GZFILEOP
  )
else()
  file(GLOB _zlibng_x86 "zlib-ng/arch/x86/*.c")
  add_library(zlib-ng STATIC
    ${_zlibng_c}
    ${_zlibng_x86}
    ${_zlibng_generic}
  )
  target_compile_definitions(zlib-ng PRIVATE
    X86_FEATURES
    X86_HAVE_XSAVE_INTRIN
    X86_SSSE3
    X86_SSE42
    WITH_GZFILEOP
  )
  if(WIN32)
    target_compile_definitions(zlib-ng PRIVATE
      X86_SSE2
      X86_AVX2
      X86_AVX512
      X86_AVX512VNNI
      X86_PCLMULQDQ_CRC
      X86_VPCLMULQDQ_CRC
    )
  else()
    # Remove AVX2/AVX512 files on non-Windows
    set(_zlibng_exclude_patterns
      "adler32_avx2" "adler32_avx512" "adler32_avx512_vnni"
      "chunkset_avx2" "compare256_avx2"
      "crc32_pclmulqdq" "crc32_vpclmulqdq"
      "slide_hash_avx2"
    )
    foreach(_pat ${_zlibng_exclude_patterns})
      list(FILTER _zlibng_x86 EXCLUDE REGEX "${_pat}")
    endforeach()
    # Re-set sources without excluded files
    get_target_property(_zlibng_srcs zlib-ng SOURCES)
    set_target_properties(zlib-ng PROPERTIES SOURCES "")
    target_sources(zlib-ng PRIVATE ${_zlibng_c} ${_zlibng_x86} ${_zlibng_generic})
  endif()
endif()
target_include_directories(zlib-ng PUBLIC zlib-ng)

# ==============================================================================
# libusb (Windows only)
# ==============================================================================
if(WIN32)
  add_library(libusb STATIC
    libusb/libusb/core.c
    libusb/libusb/descriptor.c
    libusb/libusb/hotplug.c
    libusb/libusb/io.c
    libusb/libusb/strerror.c
    libusb/libusb/sync.c
    libusb/libusb/os/events_windows.c
    libusb/libusb/os/events_windows.h
    libusb/libusb/os/threads_windows.c
    libusb/libusb/os/threads_windows.h
    libusb/libusb/os/windows_common.c
    libusb/libusb/os/windows_common.h
    libusb/libusb/os/windows_usbdk.c
    libusb/libusb/os/windows_usbdk.h
    libusb/libusb/os/windows_winusb.c
    libusb/libusb/os/windows_winusb.h
  )
  target_include_directories(libusb PUBLIC libusb/libusb PRIVATE libusb/msvc)
endif()

# ==============================================================================
# SDL2
# ==============================================================================
if(WIN32)
  # Build SDL2 from source on Windows
  file(GLOB _sdl2_sources
    "SDL2/src/*.c"
    "SDL2/src/atomic/*.c"
    "SDL2/src/audio/*.c"
    "SDL2/src/audio/directsound/*.c"
    "SDL2/src/audio/disk/*.c"
    "SDL2/src/audio/dummy/*.c"
    "SDL2/src/audio/wasapi/*.c"
    "SDL2/src/audio/winmm/*.c"
    "SDL2/src/core/windows/*.c"
    "SDL2/src/cpuinfo/*.c"
    "SDL2/src/dynapi/*.c"
    "SDL2/src/events/*.c"
    "SDL2/src/file/*.c"
    "SDL2/src/filesystem/windows/*.c"
    "SDL2/src/haptic/*.c"
    "SDL2/src/haptic/dummy/*.c"
    "SDL2/src/haptic/windows/*.c"
    "SDL2/src/hidapi/*.c"
    "SDL2/src/joystick/*.c"
    "SDL2/src/joystick/dummy/*.c"
    "SDL2/src/joystick/hidapi/*.c"
    "SDL2/src/joystick/virtual/*.c"
    "SDL2/src/joystick/windows/*.c"
    "SDL2/src/libm/*.c"
    "SDL2/src/loadso/windows/*.c"
    "SDL2/src/locale/*.c"
    "SDL2/src/locale/windows/*.c"
    "SDL2/src/misc/*.c"
    "SDL2/src/misc/windows/*.c"
    "SDL2/src/power/*.c"
    "SDL2/src/power/windows/*.c"
    "SDL2/src/render/*.c"
    "SDL2/src/render/direct3d/*.c"
    "SDL2/src/render/direct3d11/*.c"
    "SDL2/src/render/direct3d12/*.c"
    "SDL2/src/render/opengl/*.c"
    "SDL2/src/render/opengles2/*.c"
    "SDL2/src/render/software/*.c"
    "SDL2/src/sensor/*.c"
    "SDL2/src/sensor/dummy/*.c"
    "SDL2/src/sensor/windows/*.c"
    "SDL2/src/stdlib/*.c"
    "SDL2/src/thread/*.c"
    "SDL2/src/thread/generic/SDL_syscond.c"
    "SDL2/src/thread/windows/*.c"
    "SDL2/src/timer/*.c"
    "SDL2/src/timer/windows/*.c"
    "SDL2/src/video/*.c"
    "SDL2/src/video/dummy/*.c"
    "SDL2/src/video/windows/*.c"
    "SDL2/src/video/yuv2rgb/*.c"
  )
  add_library(SDL2 STATIC ${_sdl2_sources})
  target_compile_definitions(SDL2 PRIVATE
    HAVE_LIBC
    SDL_LEAN_AND_MEAN
    SDL_RENDER_DISABLED
  )
  target_include_directories(SDL2 PUBLIC SDL2/include)
  target_link_libraries(SDL2 PRIVATE setupapi winmm imm32 version)
else()
  # Use system SDL2 on Linux
  add_library(SDL2 INTERFACE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2_PKG REQUIRED sdl2)
  target_include_directories(SDL2 INTERFACE ${SDL2_PKG_INCLUDE_DIRS})
  target_link_libraries(SDL2 INTERFACE ${SDL2_PKG_LDFLAGS})
  target_compile_options(SDL2 INTERFACE ${SDL2_PKG_CFLAGS_OTHER})
endif()

# ==============================================================================
# FFmpeg (source in FFmpeg submodule, config overlay in ffmpeg-xenia)
# ==============================================================================
set(XE_HAS_GAS_ASM FALSE)
if(XE_TARGET_AARCH64)
  if(MSVC)
    # MSVC can't assemble GAS-syntax .S files. Use clang as the ASM compiler
    # if available — the resulting .obj files link fine with MSVC-compiled code.
    # Pick clang from the host architecture directory so it can actually
    # run on this machine (clang is a cross-compiler — it handles the
    # target arch via --target regardless of host).
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "ARM64")
      set(_clang_host_dir "ARM64")
    else()
      set(_clang_host_dir "x64")
    endif()
    find_program(CLANG_EXE clang HINTS
      "$ENV{VCINSTALLDIR}/Tools/Llvm/${_clang_host_dir}/bin"
      "$ENV{VCINSTALLDIR}/Tools/Llvm/bin"
      "C:/Program Files/LLVM/bin"
    )
    if(CLANG_EXE)
      set(XE_HAS_GAS_ASM TRUE)
      set(XE_CLANG_ASM "${CLANG_EXE}")
      message(STATUS "Using clang for ARM64 assembly: ${CLANG_EXE}")
    else()
      message(WARNING "clang not found — FFmpeg NEON assembly disabled on MSVC ARM64. "
                      "Install LLVM/clang to enable optimized audio decoding.")
    endif()
  else()
    enable_language(ASM)
    set(XE_HAS_GAS_ASM TRUE)
  endif()
endif()

# Helper: compile GAS-syntax .S files with clang on MSVC, or natively on
# GCC/Clang. Adds the resulting objects to the target.
function(xe_add_gas_sources target)
  if(NOT XE_HAS_GAS_ASM)
    return()
  endif()
  foreach(src ${ARGN})
    if(XE_CLANG_ASM)
      # MSVC: use clang to assemble .S -> .obj
      get_filename_component(src_name "${src}" NAME_WE)
      get_filename_component(src_abs "${src}" ABSOLUTE)
      set(obj "${CMAKE_CURRENT_BINARY_DIR}/${src_name}.obj")
      add_custom_command(
        OUTPUT "${obj}"
        COMMAND "${XE_CLANG_ASM}" --target=aarch64-pc-windows-msvc
                -I "${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg"
                -I "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-xenia"
                -c "${src_abs}" -o "${obj}"
        DEPENDS "${src_abs}"
        COMMENT "Assembling ${src} with clang"
      )
      target_sources(${target} PRIVATE "${obj}")
    else()
      # GCC/Clang: native ASM support
      target_sources(${target} PRIVATE "${src}")
    endif()
  endforeach()
endfunction()

# Common function to apply shared FFmpeg settings to a target.
function(ffmpeg_common target)
  target_compile_definitions(${target} PRIVATE
    HAVE_AV_CONFIG_H
    _USE_MATH_DEFINES
  )
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-xenia
    ${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg
  )

  if(WIN32)
    target_include_directories(${target} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg/compat/atomics/win32
    )
    target_link_libraries(${target} PRIVATE bcrypt)
    # Size optimization on Debug/Checked (dead code elimination is mandatory)
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Debug>:/Os>
      $<$<CONFIG:Checked>:/Os>
    )
  else()
    target_include_directories(${target} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg/compat/atomics/gcc
    )
  endif()

  # Suppress all warnings for FFmpeg code
  if(MSVC)
    target_compile_options(${target} PRIVATE /w)
  else()
    target_compile_options(${target} PRIVATE -w)
  endif()

  # All FFmpeg source is C
  set_target_properties(${target} PROPERTIES LINKER_LANGUAGE C)
endfunction()

# --- libavutil ---
add_library(libavutil STATIC
  FFmpeg/libavutil/adler32.c
  FFmpeg/libavutil/aes.c
  FFmpeg/libavutil/aes_ctr.c
  FFmpeg/libavutil/audio_fifo.c
  FFmpeg/libavutil/avstring.c
  FFmpeg/libavutil/avsscanf.c
  FFmpeg/libavutil/base64.c
  FFmpeg/libavutil/blowfish.c
  FFmpeg/libavutil/bprint.c
  FFmpeg/libavutil/buffer.c
  FFmpeg/libavutil/cast5.c
  FFmpeg/libavutil/camellia.c
  FFmpeg/libavutil/channel_layout.c
  FFmpeg/libavutil/csp.c
  FFmpeg/libavutil/cpu.c
  FFmpeg/libavutil/crc.c
  FFmpeg/libavutil/des.c
  FFmpeg/libavutil/dict.c
  FFmpeg/libavutil/display.c
  FFmpeg/libavutil/dovi_meta.c
  FFmpeg/libavutil/downmix_info.c
  FFmpeg/libavutil/encryption_info.c
  FFmpeg/libavutil/error.c
  FFmpeg/libavutil/eval.c
  FFmpeg/libavutil/fifo.c
  FFmpeg/libavutil/file.c
  FFmpeg/libavutil/file_open.c
  FFmpeg/libavutil/float_dsp.c
  FFmpeg/libavutil/fixed_dsp.c
  FFmpeg/libavutil/frame.c
  FFmpeg/libavutil/hash.c
  FFmpeg/libavutil/hdr_dynamic_metadata.c
  FFmpeg/libavutil/hmac.c
  FFmpeg/libavutil/hwcontext.c
  FFmpeg/libavutil/imgutils.c
  FFmpeg/libavutil/integer.c
  FFmpeg/libavutil/intmath.c
  FFmpeg/libavutil/lfg.c
  FFmpeg/libavutil/lls.c
  FFmpeg/libavutil/log.c
  FFmpeg/libavutil/log2_tab.c
  FFmpeg/libavutil/mathematics.c
  FFmpeg/libavutil/mastering_display_metadata.c
  FFmpeg/libavutil/md5.c
  FFmpeg/libavutil/mem.c
  FFmpeg/libavutil/murmur3.c
  FFmpeg/libavutil/opt.c
  FFmpeg/libavutil/parseutils.c
  FFmpeg/libavutil/pixdesc.c
  FFmpeg/libavutil/pixelutils.c
  FFmpeg/libavutil/random_seed.c
  FFmpeg/libavutil/rational.c
  FFmpeg/libavutil/reverse.c
  FFmpeg/libavutil/rc4.c
  FFmpeg/libavutil/ripemd.c
  FFmpeg/libavutil/samplefmt.c
  FFmpeg/libavutil/sha.c
  FFmpeg/libavutil/sha512.c
  FFmpeg/libavutil/slicethread.c
  FFmpeg/libavutil/spherical.c
  FFmpeg/libavutil/stereo3d.c
  FFmpeg/libavutil/threadmessage.c
  FFmpeg/libavutil/time.c
  FFmpeg/libavutil/timecode.c
  FFmpeg/libavutil/tree.c
  FFmpeg/libavutil/twofish.c
  FFmpeg/libavutil/utils.c
  FFmpeg/libavutil/xga_font_data.c
  FFmpeg/libavutil/xtea.c
  FFmpeg/libavutil/tea.c
  FFmpeg/libavutil/tx.c
  FFmpeg/libavutil/tx_float.c
  FFmpeg/libavutil/tx_double.c
  FFmpeg/libavutil/tx_int32.c
  FFmpeg/libavutil/video_enc_params.c
  FFmpeg/libavutil/film_grain_params.c
  FFmpeg/libavutil/side_data.c
  FFmpeg/libavutil/refstruct.c
  FFmpeg/libavutil/container_fifo.c
  FFmpeg/libavutil/float_scalarproduct.c
  FFmpeg/libavutil/timecode_internal.c
  FFmpeg/libavutil/iamf.c
  FFmpeg/libavutil/timestamp.c
)
# x86 platform files
if(XE_TARGET_X86_64)
  target_sources(libavutil PRIVATE
    FFmpeg/libavutil/x86/cpu.c
    FFmpeg/libavutil/x86/fixed_dsp_init.c
    FFmpeg/libavutil/x86/float_dsp_init.c
    FFmpeg/libavutil/x86/imgutils_init.c
    FFmpeg/libavutil/x86/lls_init.c
    FFmpeg/libavutil/x86/tx_float_init.c
  )
endif()
# aarch64 platform files
if(XE_TARGET_AARCH64)
  target_sources(libavutil PRIVATE
    FFmpeg/libavutil/aarch64/cpu.c
  )
  if(XE_HAS_GAS_ASM)
    # NEON init files reference symbols from the .S files, so both must
    # be included together. Requires a GAS-compatible assembler (gcc/clang).
    target_sources(libavutil PRIVATE
      FFmpeg/libavutil/aarch64/float_dsp_init.c
      FFmpeg/libavutil/aarch64/tx_float_init.c
    )
    xe_add_gas_sources(libavutil
      FFmpeg/libavutil/aarch64/float_dsp_neon.S
      FFmpeg/libavutil/aarch64/tx_float_neon.S
    )
  endif()
endif()
ffmpeg_common(libavutil)

# --- libavcodec ---
add_library(libavcodec STATIC
  FFmpeg/libavcodec/ac3_parser.c
  FFmpeg/libavcodec/adts_parser.c
  FFmpeg/libavcodec/allcodecs.c
  FFmpeg/libavcodec/avcodec.c
  FFmpeg/libavcodec/avdct.c
  FFmpeg/libavcodec/packet.c
  FFmpeg/libavcodec/bitstream.c
  FFmpeg/libavcodec/bitstream_filters.c
  FFmpeg/libavcodec/bsf.c
  FFmpeg/libavcodec/codec_desc.c
  FFmpeg/libavcodec/codec_par.c
  FFmpeg/libavcodec/d3d11va.c
  FFmpeg/libavcodec/decode.c
  FFmpeg/libavcodec/dirac.c
  FFmpeg/libavcodec/dv_profile.c
  FFmpeg/libavcodec/encode.c
  FFmpeg/libavcodec/imgconvert.c
  FFmpeg/libavcodec/jni.c
  FFmpeg/libavcodec/mathtables.c
  FFmpeg/libavcodec/mediacodec.c
  FFmpeg/libavcodec/mpeg12framerate.c
  FFmpeg/libavcodec/options.c
  FFmpeg/libavcodec/parser.c
  FFmpeg/libavcodec/parsers.c
  FFmpeg/libavcodec/profiles.c
  FFmpeg/libavcodec/qsv_api.c
  FFmpeg/libavcodec/raw.c
  FFmpeg/libavcodec/utils.c
  FFmpeg/libavcodec/vorbis_parser.c
  FFmpeg/libavcodec/xiph.c
  FFmpeg/libavcodec/dct32_fixed.c
  FFmpeg/libavcodec/dct32_float.c
  FFmpeg/libavcodec/faandct.c
  FFmpeg/libavcodec/faanidct.c
  FFmpeg/libavcodec/fdctdsp.c
  FFmpeg/libavcodec/jfdctfst.c
  FFmpeg/libavcodec/jfdctint.c
  FFmpeg/libavcodec/idctdsp.c
  FFmpeg/libavcodec/simple_idct.c
  FFmpeg/libavcodec/jrevdct.c
  FFmpeg/libavcodec/mpegaudio.c
  FFmpeg/libavcodec/mpegaudiodec_common.c
  FFmpeg/libavcodec/mpegaudiodsp.c
  FFmpeg/libavcodec/mpegaudiodsp_data.c
  FFmpeg/libavcodec/mpegaudiodsp_fixed.c
  FFmpeg/libavcodec/mpegaudiodsp_float.c
  FFmpeg/libavcodec/mpegaudiodecheader.c
  FFmpeg/libavcodec/mpegaudiodata.c
  FFmpeg/libavcodec/sinewin.c
  FFmpeg/libavcodec/wma_freqs.c
  FFmpeg/libavcodec/mpegaudiodec_fixed.c
  FFmpeg/libavcodec/mpegaudiodec_float.c
  FFmpeg/libavcodec/wmaprodec.c
  FFmpeg/libavcodec/wma.c
  FFmpeg/libavcodec/wma_common.c
  FFmpeg/libavcodec/wmadec.c
  FFmpeg/libavcodec/aactab.c
  FFmpeg/libavcodec/mpegaudio_parser.c
  FFmpeg/libavcodec/pthread.c
  FFmpeg/libavcodec/pthread_slice.c
  FFmpeg/libavcodec/pthread_frame.c
  FFmpeg/libavcodec/get_buffer.c
  FFmpeg/libavcodec/vlc.c
  FFmpeg/libavcodec/threadprogress.c
  FFmpeg/libavcodec/bsf/null.c
  FFmpeg/libavcodec/exif.c
  FFmpeg/libavcodec/mpegaudiotabs.c
  FFmpeg/libavcodec/tiff_common.c
)
if(WIN32)
  target_sources(libavcodec PRIVATE FFmpeg/libavcodec/file_open.c)
endif()
# x86 platform files
if(XE_TARGET_X86_64)
  target_sources(libavcodec PRIVATE
    FFmpeg/libavcodec/x86/constants.c
    FFmpeg/libavcodec/x86/fdctdsp_init.c
    FFmpeg/libavcodec/x86/idctdsp_init.c
    FFmpeg/libavcodec/x86/mpegaudiodsp.c
    FFmpeg/libavcodec/x86/fdct.c
  )
endif()
# aarch64 platform files
if(XE_TARGET_AARCH64)
  if(XE_HAS_GAS_ASM)
    target_sources(libavcodec PRIVATE
      FFmpeg/libavcodec/aarch64/idctdsp_init_aarch64.c
      FFmpeg/libavcodec/aarch64/mpegaudiodsp_init.c
    )
    xe_add_gas_sources(libavcodec
      FFmpeg/libavcodec/aarch64/idctdsp_neon.S
      FFmpeg/libavcodec/aarch64/mpegaudiodsp_neon.S
      FFmpeg/libavcodec/aarch64/neon.S
      FFmpeg/libavcodec/aarch64/simple_idct_neon.S
    )
  endif()
endif()
target_include_directories(libavcodec PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg/libavcodec
)
target_link_libraries(libavcodec PRIVATE libavutil)
ffmpeg_common(libavcodec)

# --- libavformat ---
add_library(libavformat STATIC
  FFmpeg/libavformat/allformats.c
  FFmpeg/libavformat/avio.c
  FFmpeg/libavformat/aviobuf.c
  FFmpeg/libavformat/dump.c
  FFmpeg/libavformat/format.c
  FFmpeg/libavformat/id3v1.c
  FFmpeg/libavformat/id3v2.c
  FFmpeg/libavformat/metadata.c
  FFmpeg/libavformat/mux.c
  FFmpeg/libavformat/options.c
  FFmpeg/libavformat/os_support.c
  FFmpeg/libavformat/protocols.c
  FFmpeg/libavformat/riff.c
  FFmpeg/libavformat/sdp.c
  FFmpeg/libavformat/url.c
  FFmpeg/libavformat/utils.c
  FFmpeg/libavformat/riffdec.c
  FFmpeg/libavformat/asfdec_f.c
  FFmpeg/libavformat/asf.c
  FFmpeg/libavformat/asfcrypt.c
  FFmpeg/libavformat/avlanguage.c
  FFmpeg/libavformat/mp3dec.c
  FFmpeg/libavformat/replaygain.c
  FFmpeg/libavformat/file.c
  FFmpeg/libavformat/demux.c
  FFmpeg/libavformat/avformat.c
  FFmpeg/libavformat/seek.c
  FFmpeg/libavformat/demux_utils.c
  FFmpeg/libavformat/urldecode.c
  FFmpeg/libavformat/asf_tags.c
  FFmpeg/libavformat/to_upper4.c
)
if(WIN32)
  target_sources(libavformat PRIVATE FFmpeg/libavformat/file_open.c)
endif()
target_link_libraries(libavformat PRIVATE libavutil libavcodec)
ffmpeg_common(libavformat)
