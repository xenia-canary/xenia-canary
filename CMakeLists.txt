cmake_minimum_required(VERSION 3.20)
if(APPLE)
  project(xenia LANGUAGES C CXX OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
else()
  project(xenia LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Detect target architecture.
# - VS generator: CMAKE_GENERATOR_PLATFORM (from -A flag) takes priority.
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
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
  set(XE_TARGET_AARCH64 TRUE)
  set(XE_TARGET_X86_64 FALSE)
else()
  set(XE_TARGET_AARCH64 FALSE)
  set(XE_TARGET_X86_64 TRUE)
endif()
message(STATUS "Target architecture: XE_TARGET_AARCH64=${XE_TARGET_AARCH64} XE_TARGET_X86_64=${XE_TARGET_X86_64} (CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR})")

# Options
option(XENIA_BUILD_TESTS "Build test suites" OFF)
option(XENIA_BUILD_MISC "Build misc subprojects (trace viewers, shader compiler, vfs-dump, demos)" OFF)

if(XENIA_BUILD_TESTS)
  enable_testing()
  include(${PROJECT_SOURCE_DIR}/third_party/catch/contrib/Catch.cmake)
endif()

# Multi-config generators (VS, Ninja Multi-Config)
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_is_multi_config)
  set(CMAKE_CONFIGURATION_TYPES "Checked;Debug;Release" CACHE STRING "" FORCE)
endif()

# Custom Checked configuration (ASan + debug runtime)
set(CMAKE_C_FLAGS_CHECKED "${CMAKE_C_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_CHECKED "${CMAKE_CXX_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_CHECKED "${CMAKE_EXE_LINKER_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_CHECKED "${CMAKE_SHARED_LINKER_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_CHECKED "${CMAKE_STATIC_LINKER_FLAGS_DEBUG}" CACHE STRING "" FORCE)

# Platform name for output paths
if(WIN32)
  set(XE_PLATFORM_NAME "Windows")
elseif(APPLE)
  set(XE_PLATFORM_NAME "macOS")
else()
  set(XE_PLATFORM_NAME "Linux")
endif()

# Output directories — strip any vs-* nesting from CMAKE_BINARY_DIR so that
# VS and Ninja builds for the same architecture share an output directory,
# while different architectures (build/ vs build-arm64/) stay separate.
string(REGEX REPLACE "/vs-[^/]+$" "" XE_OUTPUT_ROOT "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${XE_OUTPUT_ROOT}/bin/${XE_PLATFORM_NAME}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${XE_OUTPUT_ROOT}/bin/${XE_PLATFORM_NAME}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/obj/${XE_PLATFORM_NAME}")

# Create scratch/ if needed
file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/scratch")

# Python for shader compilation scripts
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Include helpers
include(cmake/XeniaHelpers.cmake)

# Global include directories (these apply to all targets via directory scope)
include_directories(
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}
)
# Third-party headers as SYSTEM to suppress warnings under -Werror
include_directories(SYSTEM
  ${PROJECT_SOURCE_DIR}/third_party
)

# Global defines
add_compile_definitions(
  VULKAN_HPP_NO_TO_STRING
  IMGUI_DISABLE_DEFAULT_FONT
  USE_CPP17  # Tabulate
)

# Prevent ASan error from ImGUI
add_compile_definitions($<$<NOT:$<CONFIG:Checked>>:IMGUI_USE_STB_SPRINTF>)

# Static library define
add_compile_definitions(_LIB)

# ==============================================================================
# Platform-specific global settings
# ==============================================================================

if(MSVC)
  # Global MSVC settings
  add_compile_options(
    /utf-8       # Build correctly on systems with non-Latin codepages
    /wd4201      # Nameless struct/unions are ok
    /MP          # Multi-processor compilation
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
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
  )
  add_link_options(
    /ignore:4006  # Ignores complaints about empty obj files
    /ignore:4221
  )

  # Global Windows system libraries
  link_libraries(
    ntdll wsock32 ws2_32 xinput comctl32 shcore shlwapi dxguid bcrypt
  )
  # Windows manifest for executables is handled per-target

  # --- Per-configuration MSVC flags ---
  # Checked
  string(APPEND CMAKE_C_FLAGS_CHECKED " /RTCsu /fsanitize=address")
  string(APPEND CMAKE_CXX_FLAGS_CHECKED " /RTCsu /fsanitize=address")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_CHECKED " /INCREMENTAL:NO")
  add_compile_definitions($<$<CONFIG:Checked>:DEBUG>)

  # Debug: use release runtime (/MD) for heap performance, matching premake
  string(REPLACE "/MDd" "/MD" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
  string(REPLACE "/MDd" "/MD" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  add_compile_definitions($<$<CONFIG:Debug>:DEBUG>)
  add_compile_definitions($<$<CONFIG:Debug>:_NO_DEBUG_HEAP=1>)

  # Checked or Debug: full symbols
  string(APPEND CMAKE_C_FLAGS_CHECKED " /Zi")
  string(APPEND CMAKE_CXX_FLAGS_CHECKED " /Zi")
  # Debug already has /Zi by default

  # Release
  add_compile_definitions($<$<CONFIG:Release>:NDEBUG>)
  add_compile_definitions($<$<CONFIG:Release>:_NO_DEBUG_HEAP=1>)
  add_compile_options($<$<CONFIG:Release>:/Gw>)
  add_compile_options($<$<CONFIG:Release>:/GS->)  # NoBufferSecurityCheck
  add_compile_options($<$<CONFIG:Release>:/Zi>)    # Generate PDB debug info
  add_link_options($<$<CONFIG:Release>:/DEBUG>)     # Emit .pdb for the linked binary
  add_link_options($<$<CONFIG:Release>:/OPT:REF>)   # Remove unreferenced functions/data
  add_link_options($<$<CONFIG:Release>:/OPT:ICF>)   # Fold identical COMDATs
  # Replace /Ob2 with /Ob3 in CMake's default Release flags
  string(REPLACE "/Ob2" "/Ob3" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
  string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  # Link-time optimization (whole program optimization)
  add_compile_options($<$<CONFIG:Release>:/GL>)
  add_link_options($<$<CONFIG:Release>:/LTCG>)
  set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")

else()
  # GCC/Clang (Linux)
  if(XE_TARGET_X86_64)
    add_compile_options(-mavx)
  endif()

  # Disable specific warnings for C++
  add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-switch>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-attributes>
  )

  # Clang-specific C++ warnings
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-register>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-volatile>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>
    )

    # Check Clang version for newer warnings
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
    # GCC-specific
    add_compile_options(
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-result>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-template-id-cdtor>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-return-type>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated>
      -fpermissive  # HACK
    )
    add_link_options(-fpermissive)
    # GCC: do not use -Werror
  endif()

  # C-specific warnings
  add_compile_options(
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
  )

  if(APPLE)
    # macOS-specific settings
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
    add_compile_options(
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-shorten-64-to-32>
      -Wno-deprecated-declarations
      -Wno-character-conversion
    )
    add_link_options(-Wl,-no_warn_duplicate_libraries)
    link_libraries(
      "-framework CoreFoundation"
      "-framework Foundation"
      "-framework IOKit"
      dl
      pthread
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # GTK3 via pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-x11-3.0)
    include_directories(${GTK3_INCLUDE_DIRS})
    link_directories(${GTK3_LIBRARY_DIRS})
    add_compile_options(${GTK3_CFLAGS_OTHER})

    # Global Linux libraries
    link_libraries(stdc++fs dl lz4 pthread rt)

    # Link groups for executables (resolves circular deps)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--start-group")
    set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -Wl,--end-group")
  endif()

  # --- Per-configuration flags ---
  # Checked
  string(APPEND CMAKE_C_FLAGS_CHECKED " -fsanitize=address")
  string(APPEND CMAKE_CXX_FLAGS_CHECKED " -fsanitize=address")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_CHECKED " -fsanitize=address")
  add_compile_definitions(
    $<$<CONFIG:Checked>:DEBUG>
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions($<$<CONFIG:Checked>:_GLIBCXX_DEBUG>)
  endif()

  # Debug
  add_compile_definitions(
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:_NO_DEBUG_HEAP=1>
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions($<$<CONFIG:Debug>:_GLIBCXX_DEBUG>)
  endif()

  # Release
  add_compile_definitions(
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:Release>:_NO_DEBUG_HEAP=1>
  )
  add_compile_options($<$<CONFIG:Release>:-O3>)
  # Link-time optimization (use lld and llvm-ar/ranlib to avoid system
  # linker/archiver LTO plugin version mismatch with clang's bitcode)
  add_compile_options($<$<CONFIG:Release>:-flto=thin>)
  add_link_options($<$<CONFIG:Release>:-flto=thin>)
  add_link_options($<$<CONFIG:Release>:-fuse-ld=lld>)
  find_program(LLVM_AR NAMES llvm-ar)
  find_program(LLVM_RANLIB NAMES llvm-ranlib)
  if(LLVM_AR)
    set(CMAKE_AR "${LLVM_AR}" CACHE FILEPATH "" FORCE)
  endif()
  if(LLVM_RANLIB)
    set(CMAKE_RANLIB "${LLVM_RANLIB}" CACHE FILEPATH "" FORCE)
  endif()
  # No symbols in release on non-Windows
  string(REPLACE "-g" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
  string(REPLACE "-g" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

# ==============================================================================
# Subdirectories
# ==============================================================================
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Third-party libraries first (no -Werror applied to these)
set(CMAKE_FOLDER "third_party")
add_subdirectory(third_party)

# Xenia source modules (xe_target_defaults applies -Werror)
set(CMAKE_FOLDER "src")
add_subdirectory(src/xenia)

set(CMAKE_FOLDER "")

# Set default startup project for Visual Studio
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT xenia-app)
