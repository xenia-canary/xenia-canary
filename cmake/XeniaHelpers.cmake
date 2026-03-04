# XeniaHelpers.cmake - Helper functions for the Xenia build system.
# Replaces tools/build/scripts/*.lua

include(CMakeParseArguments)

# Platform suffix lists for file filtering
set(XE_PLATFORM_SUFFIXES
  _win _linux _posix _gnulinux _x11 _gtk _android _mac
)

# xe_platform_sources(target base_path [RECURSIVE])
#
# Adds platform-filtered source files to a target.
# Globs *.h, *.cc, *.cpp, *.c, *.inc from base_path.
# Excludes all platform suffixes, then adds back the ones matching the current platform.
# Always excludes *_main.cc and *_test.cc.
function(xe_platform_sources target base_path)
  set(options RECURSIVE)
  cmake_parse_arguments(ARG "${options}" "" "" ${ARGN})

  if(ARG_RECURSIVE)
    set(glob_mode GLOB_RECURSE)
    set(pattern "**")
  else()
    set(glob_mode GLOB)
    set(pattern "*")
  endif()

  # Glob all source files
  file(${glob_mode} _all_sources    "${base_path}/*.h"
    "${base_path}/*.cc"
    "${base_path}/*.cpp"
    "${base_path}/*.c"
    "${base_path}/*.inc"
  )

  # Build exclude patterns
  set(_excluded)
  foreach(src ${_all_sources})
    get_filename_component(_name_we ${src} NAME_WE)
    # Exclude *_main.cc, *_test.cc, and *_demo.cc
    if(_name_we MATCHES "_main$" OR _name_we MATCHES "_test$" OR _name_we MATCHES "_demo$")
      list(APPEND _excluded ${src})
      continue()
    endif()
    # Exclude all platform suffixes
    foreach(suffix ${XE_PLATFORM_SUFFIXES})
      if(_name_we MATCHES "${suffix}$")
        list(APPEND _excluded ${src})
        break()
      endif()
    endforeach()
  endforeach()

  # Remove excluded files
  set(_sources ${_all_sources})
  if(_excluded)
    list(REMOVE_ITEM _sources ${_excluded})
  endif()

  # Add back platform-specific files
  if(WIN32)
    file(${glob_mode} _plat_sources      "${base_path}/*_win.h"
      "${base_path}/*_win.cc"
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(${glob_mode} _plat_sources      "${base_path}/*_posix.h"
      "${base_path}/*_posix.cc"
      "${base_path}/*_linux.h"
      "${base_path}/*_linux.cc"
      "${base_path}/*_gnulinux.h"
      "${base_path}/*_gnulinux.cc"
      "${base_path}/*_x11.h"
      "${base_path}/*_x11.cc"
      "${base_path}/*_gtk.h"
      "${base_path}/*_gtk.cc"
    )
  endif()

  list(APPEND _sources ${_plat_sources})
  target_sources(${target} PRIVATE ${_sources})
endfunction()

# xe_target_defaults(target)
#
# Applies xenia-specific compile settings to a target:
# - Fatal warnings (-Werror / /WX)
# - Common include directories
function(xe_target_defaults target)
  target_include_directories(${target} PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/third_party
  )
  if(MSVC)
    target_compile_options(${target} PRIVATE /WX)
  elseif(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC is too noisy for -Werror; only apply to Clang
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# xe_force_c(files...)
#
# Forces the given source files to compile as C.
function(xe_force_c)
  set_source_files_properties(${ARGN} PROPERTIES LANGUAGE C)
endfunction()

# xe_force_cxx(files...)
#
# Forces the given source files to compile as C++.
function(xe_force_cxx)
  set_source_files_properties(${ARGN} PROPERTIES LANGUAGE CXX)
endfunction()

# xe_test_suite(name base_path LINKS lib1 lib2 ...)
#
# Creates a Catch2 test executable from *_test.cc files in base_path.
function(xe_test_suite name base_path)
  cmake_parse_arguments(ARG "" "" "LINKS" ${ARGN})

  file(GLOB _test_sources CONFIGURE_DEPENDS "${base_path}/*_test.cc")

  if(NOT _test_sources)
    return()
  endif()

  add_executable(${name}
    ${_test_sources}
    ${PROJECT_SOURCE_DIR}/tools/build/src/test_suite_main.cc
  )

  # Add platform-specific console app main
  if(WIN32)
    target_sources(${name} PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
  else()
    target_sources(${name} PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
  endif()

  target_compile_definitions(${name} PRIVATE
    "XE_TEST_SUITE_NAME=\"${name}\""
  )
  target_include_directories(${name} PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/build
    ${PROJECT_SOURCE_DIR}/tools/build/src
    ${PROJECT_SOURCE_DIR}/tools/build/third_party/catch/include
  )
  if(ARG_LINKS)
    target_link_libraries(${name} PRIVATE ${ARG_LINKS})
  endif()
  xe_target_defaults(${name})

  if(MSVC)
    # Disable Edit and Continue - breaks Catch2 __LINE__ usage
    target_compile_options(${name} PRIVATE /Zi)
  endif()
endfunction()
