# XeniaHelpers.cmake - Helper functions for the Xenia build system.
# Replaces tools/build/scripts/*.lua

include(CMakeParseArguments)

# Platform suffix lists for file filtering
set(XE_PLATFORM_SUFFIXES
  _win _linux _posix _gnulinux _x11 _gtk _android _mac _amd64 _x64 _arm64
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

  # Add back architecture-specific files
  if(XE_TARGET_X86_64)
    file(${glob_mode} _arch_sources "${base_path}/*_amd64.h" "${base_path}/*_amd64.cc"
      "${base_path}/*_x64.h" "${base_path}/*_x64.cc")
  elseif(XE_TARGET_AARCH64)
    file(${glob_mode} _arch_sources "${base_path}/*_arm64.h" "${base_path}/*_arm64.cc")
  endif()
  if(_arch_sources)
    list(APPEND _sources ${_arch_sources})
  endif()

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
    # Recreate directory structure in Visual Studio filters
    get_target_property(_srcs ${target} SOURCES)
    if(_srcs)
      set(_local_srcs)
      set(_external_srcs)
      foreach(_s IN LISTS _srcs)
        get_filename_component(_abs "${_s}" ABSOLUTE)
        string(FIND "${_abs}" "${CMAKE_CURRENT_SOURCE_DIR}" _pos)
        if(_pos EQUAL 0)
          list(APPEND _local_srcs "${_s}")
        else()
          list(APPEND _external_srcs "${_s}")
        endif()
      endforeach()
      if(_local_srcs)
        source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${_local_srcs})
      endif()
      if(_external_srcs)
        source_group(TREE ${PROJECT_SOURCE_DIR} PREFIX "External" FILES ${_external_srcs})
      endif()
    endif()
  elseif(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC is too noisy for -Werror; only apply to Clang
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# xe_shader_rules_spirv(target shader_dir)
#
# Ensures SPIR-V shaders are compiled before the target builds.
# Uses a stamp file; rebuilds when any shader source changes.
function(xe_shader_rules_spirv target shader_dir)
  get_filename_component(shader_dir "${shader_dir}" ABSOLUTE)
  file(GLOB _sources
    "${shader_dir}/*.xesl" "${shader_dir}/*.glsl"
    "${shader_dir}/*.xesli" "${shader_dir}/*.glsli")
  set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_spirv.stamp")
  set(_script "${PROJECT_SOURCE_DIR}/tools/build/compile_shader_spirv.py")
  set(_valid_stages vs hs ds gs ps cs)
  set(_commands)
  set(_bytecode_dir "${shader_dir}/bytecode/vulkan_spirv")
  list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E make_directory "${_bytecode_dir}")
  foreach(src ${_sources})
    get_filename_component(_name ${src} NAME)
    string(REGEX REPLACE "\\.[^.]+$" "" _basename "${_name}")
    string(REPLACE "." "_" _id "${_basename}")
    string(LENGTH "${_id}" _len)
    if(_len LESS 3)
      continue()
    endif()
    math(EXPR _s "${_len} - 2")
    string(SUBSTRING "${_id}" ${_s} 2 _stage)
    if(NOT _stage IN_LIST _valid_stages)
      continue()
    endif()
    list(APPEND _commands COMMAND ${Python3_EXECUTABLE} "${_script}" "${src}" "${_bytecode_dir}/${_id}.h")
  endforeach()
  add_custom_command(
    OUTPUT "${_stamp}"
    ${_commands}
    COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
    DEPENDS ${_sources} "${_script}"
    COMMENT "Compiling SPIR-V shaders for ${target}..."
    VERBATIM
  )
  add_custom_target(${target}-spirv-shaders DEPENDS "${_stamp}")
  add_dependencies(${target} ${target}-spirv-shaders)
  # Add sources for IDE visibility but prevent VS from compiling them itself
  set_source_files_properties(${_sources} PROPERTIES HEADER_FILE_ONLY TRUE)
  target_sources(${target} PRIVATE ${_sources})
endfunction()

# xe_shader_rules_dxbc(target shader_dir)
#
# Ensures DXBC shaders are compiled before the target builds.
# Uses a stamp file; rebuilds when any shader source changes.
function(xe_shader_rules_dxbc target shader_dir)
  get_filename_component(shader_dir "${shader_dir}" ABSOLUTE)
  file(GLOB _sources
    "${shader_dir}/*.xesl" "${shader_dir}/*.hlsl"
    "${shader_dir}/*.xesli" "${shader_dir}/*.hlsli")
  set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_dxbc.stamp")
  set(_script "${PROJECT_SOURCE_DIR}/tools/build/compile_shader_dxbc.py")
  set(_valid_stages vs hs ds gs ps cs)
  set(_commands)
  set(_bytecode_dir "${shader_dir}/bytecode/d3d12_5_1")
  list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E make_directory "${_bytecode_dir}")
  foreach(src ${_sources})
    get_filename_component(_name ${src} NAME)
    string(REGEX REPLACE "\\.[^.]+$" "" _basename "${_name}")
    string(REPLACE "." "_" _id "${_basename}")
    string(LENGTH "${_id}" _len)
    if(_len LESS 3)
      continue()
    endif()
    math(EXPR _s "${_len} - 2")
    string(SUBSTRING "${_id}" ${_s} 2 _stage)
    if(NOT _stage IN_LIST _valid_stages)
      continue()
    endif()
    list(APPEND _commands COMMAND ${Python3_EXECUTABLE} "${_script}" "${src}" "${_bytecode_dir}/${_id}.h")
  endforeach()
  add_custom_command(
    OUTPUT "${_stamp}"
    ${_commands}
    COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
    DEPENDS ${_sources} "${_script}"
    COMMENT "Compiling DXBC shaders for ${target}..."
    VERBATIM
  )
  add_custom_target(${target}-dxbc-shaders DEPENDS "${_stamp}")
  add_dependencies(${target} ${target}-dxbc-shaders)
  # Add sources for IDE visibility but prevent VS from compiling them itself
  set_source_files_properties(${_sources} PROPERTIES HEADER_FILE_ONLY TRUE)
  target_sources(${target} PRIVATE ${_sources})
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

  file(GLOB _test_sources "${base_path}/*_test.cc")

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
  set_target_properties(${name} PROPERTIES FOLDER "tests")

  if(MSVC)
    # Disable Edit and Continue - breaks Catch2 __LINE__ usage
    target_compile_options(${name} PRIVATE /Zi)
  endif()

  catch_discover_tests(${name})
endfunction()
