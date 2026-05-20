add_library(xenia-cpu-backend-a64 STATIC)
xe_platform_sources(xenia-cpu-backend-a64 ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(xenia-cpu-backend-a64 PRIVATE
  CAPSTONE_HAS_ARM64
  CAPSTONE_USE_SYS_DYN_MEM
)
target_include_directories(xenia-cpu-backend-a64 PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/capstone/include
  ${PROJECT_SOURCE_DIR}/third_party/xbyak_aarch64/xbyak_aarch64
)
target_link_libraries(xenia-cpu-backend-a64 PUBLIC capstone fmt xenia-base xenia-cpu xbyak_aarch64)
xe_target_defaults(xenia-cpu-backend-a64)

# JIT tracing is read only by a64_tracers.cc, so scope the defines to that
# single TU. Toggling --enable-itrace / --enable-dtrace then recompiles just
# this file instead of forcing a full rebuild.
set(_a64_tracer_defs "")
if(XENIA_ENABLE_ITRACE)
  list(APPEND _a64_tracer_defs XE_OPTION_TRACE_INSTR=1)
endif()
if(XENIA_ENABLE_DTRACE)
  list(APPEND _a64_tracer_defs XE_OPTION_TRACE_DATA=1)
endif()
if(XENIA_ENABLE_FTRACE)
  list(APPEND _a64_tracer_defs XE_OPTION_TRACE_FUNC=1)
endif()
if(_a64_tracer_defs)
  set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/a64_tracers.cc
    PROPERTIES COMPILE_DEFINITIONS "${_a64_tracer_defs}")
endif()
