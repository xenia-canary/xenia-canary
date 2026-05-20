cmake_minimum_required(VERSION 3.20)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# Must be set before project() so the toolchain bakes -mmacosx-version-min
# into every compile/link; setting it later only updates the variable.
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
endif()

# Suppress macOS ranlib warnings for empty object files. wxWidgets's vendored
# libpng compiles Intel/MIPS/PowerPC sources that are empty on ARM64.
if(APPLE)
  set(CMAKE_C_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>")
  set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>")
  set(CMAKE_OBJC_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>")
  set(CMAKE_OBJCXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>")
endif()

if(APPLE)
  # OBJC needed for SDL2's .m sources (Cocoa video, CoreAudio, MFi joystick).
  project(xenia LANGUAGES C CXX OBJC OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
else()
  project(xenia LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
# We don't use C++20 modules; skip the per-target dyndep scan Ninja runs by
# default for CXX_STANDARD>=20.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Detect target architecture.
# - VS generator: CMAKE_GENERATOR_PLATFORM (from -A flag) takes priority.
# - macOS: CMAKE_OSX_ARCHITECTURES — single-arch only (universal builds
#   would need both backends compiled, which we don't support).
# - Ninja/Makefiles: CMAKE_SYSTEM_PROCESSOR (set via -DCMAKE_SYSTEM_NAME
#   + -DCMAKE_SYSTEM_PROCESSOR for cross-compile, or auto-detected natively).
if(CMAKE_GENERATOR_PLATFORM)
  if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
    set(XE_TARGET_AARCH64 TRUE)
    set(XE_TARGET_X86_64 FALSE)
  else()
    set(XE_TARGET_AARCH64 FALSE)
    set(XE_TARGET_X86_64 TRUE)
  endif()
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _xe_osx_arch_count)
  if(_xe_osx_arch_count GREATER 1)
    message(FATAL_ERROR "CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}': universal builds are not supported, set a single arch.")
  endif()
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(XE_TARGET_AARCH64 TRUE)
    set(XE_TARGET_X86_64 FALSE)
  else()
    set(XE_TARGET_AARCH64 FALSE)
    set(XE_TARGET_X86_64 TRUE)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
  set(XE_TARGET_AARCH64 TRUE)
  set(XE_TARGET_X86_64 FALSE)
else()
  set(XE_TARGET_AARCH64 FALSE)
  set(XE_TARGET_X86_64 TRUE)
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  set(_xe_arch_source "CMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
elseif(CMAKE_GENERATOR_PLATFORM)
  set(_xe_arch_source "CMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}")
else()
  set(_xe_arch_source "CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
endif()
message(STATUS "Target architecture: XE_TARGET_AARCH64=${XE_TARGET_AARCH64} XE_TARGET_X86_64=${XE_TARGET_X86_64} (${_xe_arch_source})")

option(XENIA_BUILD_TESTS "Build test suites" OFF)
option(XENIA_BUILD_MISC "Build misc subprojects (trace viewers, shader compiler, vfs-dump, demos)" OFF)
option(XENIA_ENABLE_LTO "Enable link-time optimization in Release (slow link)" ON)
option(XENIA_ENABLE_PROFILER "Enable profiler (UI is Debug-only)" OFF)
option(XENIA_ENABLE_ITRACE "Enable JIT per-instruction tracing to the log (slow)" OFF)
option(XENIA_ENABLE_DTRACE "Enable JIT per-operation data tracing to the log (very slow)" OFF)
option(XENIA_ENABLE_FTRACE "Enable JIT per-function-call tracing to the log" OFF)

if(XENIA_ENABLE_PROFILER)
  # UI is Debug-only — shutdown dump spams the log otherwise.
  add_compile_definitions(XE_OPTION_PROFILING=1)
  add_compile_definitions($<$<CONFIG:Debug>:XE_OPTION_PROFILING_UI=1>)
endif()

# XENIA_ENABLE_ITRACE / XENIA_ENABLE_DTRACE / XENIA_ENABLE_FTRACE are consumed
# per-file in the x64 and a64 backend CMakeLists (only the *_tracers.cc TUs read
# them), so toggling them doesn't trigger a full rebuild.

if(XENIA_BUILD_TESTS)
  enable_testing()
  include(${PROJECT_SOURCE_DIR}/third_party/catch/contrib/Catch.cmake)
endif()

get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_is_multi_config)
  set(CMAKE_CONFIGURATION_TYPES "Checked;Debug;Release" CACHE STRING "")
endif()

# Checked inherits Debug's flags as a starting point; the platform blocks
# below layer sanitizers on top.
set(CMAKE_C_FLAGS_CHECKED "${CMAKE_C_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_CHECKED "${CMAKE_CXX_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_CHECKED "${CMAKE_EXE_LINKER_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_CHECKED "${CMAKE_SHARED_LINKER_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_CHECKED "${CMAKE_STATIC_LINKER_FLAGS_DEBUG}" CACHE STRING "" FORCE)

if(WIN32)
  set(XE_PLATFORM_NAME "Windows")
elseif(APPLE)
  set(XE_PLATFORM_NAME "macOS")
else()
  set(XE_PLATFORM_NAME "Linux")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${XE_PLATFORM_NAME}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${XE_PLATFORM_NAME}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/obj/${XE_PLATFORM_NAME}")

file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/scratch")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Debug/Checked compile against the release CRT (/MD) since the product
# ships against release-only system DLLs and prebuilts. Force any imported
# library to resolve to its Release variant in those configs too.
# NOCONFIG is the fallback for imported targets that ship without a
# configuration suffix (FindOpenGL on Linux creates OpenGL::OpenGL this
# way — without NOCONFIG in the map we fail to resolve it in Debug et al).
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG "Release;NOCONFIG" CACHE STRING "" FORCE)
set(CMAKE_MAP_IMPORTED_CONFIG_CHECKED "Release;NOCONFIG" CACHE STRING "" FORCE)

# find_package targets like Python3::Interpreter (and various Find*-module
# imports — FindOpenGL, FindXKB, etc.) ship with only a bare IMPORTED_LOCATION
# and no IMPORTED_CONFIGURATIONS. Multi-config generators (VS, Ninja
# Multi-Config) can't reliably resolve those through MAP_IMPORTED_CONFIG_*'s
# NOCONFIG fallback and fail with "IMPORTED_LOCATION not set ... configuration
# Debug". Re-tag every such target as Release so the existing MAP resolves.
get_property(_imported_targets DIRECTORY PROPERTY IMPORTED_TARGETS)
foreach(_tgt IN LISTS _imported_targets)
  get_target_property(_loc ${_tgt} IMPORTED_LOCATION)
  if(_loc AND NOT _loc MATCHES "-NOTFOUND$")
    get_target_property(_confs ${_tgt} IMPORTED_CONFIGURATIONS)
    if(NOT _confs OR _confs MATCHES "-NOTFOUND$")
      set_target_properties(${_tgt} PROPERTIES
        IMPORTED_CONFIGURATIONS "Release"
        IMPORTED_LOCATION_RELEASE "${_loc}"
      )
    endif()
  endif()
endforeach()

include(cmake/XeniaHelpers.cmake)

include_directories(
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src
  # Build directory — for `#include "version.h"` (generated by xenia-build.py
  # into CMAKE_BINARY_DIR, which varies per configure: build/, build-vs/, ...).
  ${CMAKE_BINARY_DIR}
)
# Third-party headers as SYSTEM to suppress warnings under -Werror
include_directories(SYSTEM
  ${PROJECT_SOURCE_DIR}/third_party
)

add_compile_definitions(
  VULKAN_HPP_NO_TO_STRING
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS
  IMGUI_DISABLE_DEFAULT_FONT
  USE_CPP17  # Tabulate
  _LIB
)

# Prevent ASan error from ImGUI
add_compile_definitions($<$<NOT:$<CONFIG:Checked>>:IMGUI_USE_STB_SPRINTF>)

# ==============================================================================
# Platform-specific global settings
# ==============================================================================

if(MSVC)
  # CRT runtime selection via CMP0091. Debug and Release use the release
  # CRT (/MD); Checked uses the debug CRT (/MDd) because it pairs /RTCsu
  # with ASan.
  set(CMAKE_MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Checked>:Debug>DLL"
      CACHE STRING "" FORCE)

  # Patch the cached CMAKE_C/CXX_FLAGS_DEBUG so /MDd → /MD for Debug builds.
  # Required for vendored wxWidgets: its CMakeLists calls cmake_minimum_required
  # (VERSION 3.5), leaving CMP0091 at OLD/WARN inside its scope. Under OLD,
  # MSVC_RUNTIME_LIBRARY is ignored and wxWidgets uses CMAKE_CXX_FLAGS_DEBUG —
  # so removing /MDd here is what makes wx's Debug objects link /MD, matching
  # our /MD-pinned root and avoiding LNK2038 RuntimeLibrary mismatches.
  string(REPLACE "/MDd" "/MD" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
  string(REPLACE "/MDd" "/MD" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}" CACHE STRING "" FORCE)

  add_compile_options(
    /utf-8             # non-Latin-codepage-safe source parsing
    /Zc:__cplusplus    # correct __cplusplus macro value
    /Zc:preprocessor   # conformant preprocessor
    /wd4201            # nameless struct/unions are ok
    /MP
  )
  if(XE_TARGET_X86_64)
    add_compile_options(/arch:AVX)
  endif()
  add_compile_definitions(
    _CRT_NONSTDC_NO_DEPRECATE
    _CRT_SECURE_NO_WARNINGS
    WIN32
    _WIN64=1
    _AMD64=1
    UNICODE
    _UNICODE
  )
  add_link_options(
    /ignore:4006  # duplicate symbols in static libs
    /ignore:4221  # empty obj files
  )
  link_libraries(
    ntdll wsock32 ws2_32 xinput comctl32 shcore shlwapi dxguid bcrypt
  )

  # Checked: /RTCsu + ASan + debug symbols. /RTC1 from CMake's default
  # Debug flags conflicts with /RTCsu, so strip it first.
  string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_CHECKED "${CMAKE_C_FLAGS_CHECKED}")
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_CHECKED "${CMAKE_CXX_FLAGS_CHECKED}")
  string(APPEND CMAKE_C_FLAGS_CHECKED " /RTCsu /fsanitize=address /Zi")
  string(APPEND CMAKE_CXX_FLAGS_CHECKED " /RTCsu /fsanitize=address /Zi")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_CHECKED " /INCREMENTAL:NO")
  add_compile_definitions(
    $<$<CONFIG:Checked>:DEBUG>
    $<$<CONFIG:Checked>:NDEBUG>
  )

  # Debug: /RTC1 requires the debug CRT, so strip it — we're on /MD. Pin
  # _ITERATOR_DEBUG_LEVEL=0 so our STL object layouts match release-CRT
  # prebuilts and we don't hit LNK2038.
  string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  add_compile_definitions(
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:_NO_DEBUG_HEAP=1>
    $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>
  )

  add_compile_definitions(
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:Release>:_NO_DEBUG_HEAP=1>
  )
  add_compile_options(
    $<$<CONFIG:Release>:/Gw>
    $<$<CONFIG:Release>:/GS->   # NoBufferSecurityCheck
  )
  # /Ob3 is a stronger inline heuristic than CMake's default /Ob2.
  string(REPLACE "/Ob2" "/Ob3" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
  string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  # LTCG (whole-program optimization). Disabled by -DXENIA_ENABLE_LTO=OFF
  # for faster Release link times during iterative work.
  if(XENIA_ENABLE_LTO)
    add_compile_options($<$<CONFIG:Release>:/GL>)
    add_link_options($<$<CONFIG:Release>:/LTCG>)
    set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
  endif()
  # Release has no PDB.
  string(REPLACE "/Zi" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
  string(REPLACE "/Zi" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  string(REPLACE "/DEBUG" "" CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE}")

else()
  # GCC/Clang (Linux)
  if(XE_TARGET_X86_64)
    add_compile_options(-mavx)
  endif()

  add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-switch>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-attributes>
  )

  # Clang-specific C++ warnings (matches AppleClang too)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-register>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-volatile>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-absolute-value>
    )
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 20)
      add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-literal-operator>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-nontrivial-memcall>
      )
    endif()
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 21)
      add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-character-conversion>
      )
    endif()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-result>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-template-id-cdtor>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-return-type>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated>
      -fpermissive  # HACK
    )
    add_link_options(-fpermissive)
  endif()

  add_compile_options(
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
    $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
  )

  if(APPLE)
    # macOS-specific settings
    add_compile_options(
      -Wno-unknown-warning-option
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-shorten-64-to-32>
      -Wno-deprecated-declarations
      -Wno-character-conversion
      -fno-common
    )
    add_link_options(-Wl,-no_warn_duplicate_libraries)
    if(XE_TARGET_X86_64)
      # macOS 64-bit binaries default to a 4GB __PAGEZERO, which makes every
      # address below 4GB unmapped. The x64 backend places its constant
      # table (PlaceConstData) sub-2GB and its guest trampolines sub-4GB,
      # so we shrink __PAGEZERO to one page to free up that range.
      add_link_options(-Wl,-pagezero_size,0x1000)
    endif()
    link_libraries(
      "-framework CoreFoundation"
      "-framework Foundation"
      "-framework IOKit"
      dl
      pthread
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-x11-3.0)
    include_directories(${GTK3_INCLUDE_DIRS})
    link_directories(${GTK3_LIBRARY_DIRS})
    add_compile_options(${GTK3_CFLAGS_OTHER})

    link_libraries(stdc++fs dl lz4 m pthread rt)

    # Link group so executables resolve circular static-lib deps.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--start-group")
    set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -Wl,--end-group")
  endif()

  # Checked: UBSan.
  string(APPEND CMAKE_C_FLAGS_CHECKED " -fsanitize=undefined")
  string(APPEND CMAKE_CXX_FLAGS_CHECKED " -fsanitize=undefined")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_CHECKED " -fsanitize=undefined")
  add_compile_definitions(
    $<$<CONFIG:Checked>:DEBUG>
    $<$<CONFIG:Checked>:NDEBUG>
  )

  add_compile_definitions(
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:_NO_DEBUG_HEAP=1>
  )

  # Release keeps -g so crashes in the field have resolvable frames.
  add_compile_definitions(
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:Release>:_NO_DEBUG_HEAP=1>
  )
  add_compile_options(
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-finline-functions>
    $<$<CONFIG:Release>:-funroll-loops>
    $<$<CONFIG:Release>:-fomit-frame-pointer>
    $<$<CONFIG:Release>:-g>
  )
  # ThinLTO. On Linux/Windows use lld (faster and what LLVM tests ThinLTO
  # against upstream). On macOS use Apple's ld64, which handles ThinLTO
  # natively and avoids an lld install requirement. Gated behind
  # XENIA_ENABLE_LTO so -DXENIA_ENABLE_LTO=OFF produces a Release binary
  # with no LTO, which links an order of magnitude faster during iteration.
  if(XENIA_ENABLE_LTO)
    add_compile_options($<$<CONFIG:Release>:-flto=thin>)
    add_link_options($<$<CONFIG:Release>:-flto=thin>)
    if(NOT APPLE)
      add_link_options($<$<CONFIG:Release>:-fuse-ld=lld>)
    endif()
    # Use llvm-ar / llvm-ranlib when available to avoid LTO plugin
    # version mismatch between system ar/ranlib and clang's bitcode.
    find_program(LLVM_AR NAMES llvm-ar)
    find_program(LLVM_RANLIB NAMES llvm-ranlib)
    if(LLVM_AR)
      set(CMAKE_AR "${LLVM_AR}" CACHE FILEPATH "" FORCE)
    endif()
    if(LLVM_RANLIB)
      set(CMAKE_RANLIB "${LLVM_RANLIB}" CACHE FILEPATH "" FORCE)
    endif()
  endif()

  # Silence Apple ar/ranlib's "has no symbols" warnings (e.g. zlib-ng's
  # CRC32 SIMD TUs that compile empty when the CPU-feature guard is
  # inactive). ar warns at archive-create time and ranlib warns again at
  # finish; ar's `S` flag skips the symtab so only ranlib writes it, and
  # ranlib's -no_warning_for_no_symbols silences that pass.
  # Only applies when we'll actually use Apple's tools — llvm-ranlib
  # rejects this Apple-specific flag.
  if(APPLE AND NOT CMAKE_RANLIB MATCHES "llvm-ranlib")
    set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> Scr <TARGET> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> Scr <TARGET> <OBJECTS>")
    set(CMAKE_C_ARCHIVE_FINISH
        "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
    set(CMAKE_CXX_ARCHIVE_FINISH
        "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
  endif()

endif()

# ==============================================================================
# Subdirectories
# ==============================================================================

add_subdirectory(third_party)

add_subdirectory(src/xenia)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT xenia-app)
