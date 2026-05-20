add_library(xenia-cpu-backend-x64 STATIC)
xe_platform_sources(xenia-cpu-backend-x64 ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(xenia-cpu-backend-x64 PRIVATE
  CAPSTONE_X86_ATT_DISABLE
  CAPSTONE_HAS_X86
  CAPSTONE_USE_SYS_DYN_MEM
  XBYAK_NO_OP_NAMES
  XBYAK_ENABLE_OMITTED_OPERAND
)
# Optional VTune support
if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/vtune")
  target_compile_definitions(xenia-cpu-backend-x64 PRIVATE ENABLE_VTUNE=1)
endif()
target_include_directories(xenia-cpu-backend-x64 PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/capstone/include
)
target_link_libraries(xenia-cpu-backend-x64 PUBLIC capstone fmt xenia-base xenia-cpu)
xe_target_defaults(xenia-cpu-backend-x64)

# JIT tracing is read only by x64_tracers.cc, so scope the defines to that
# single TU. Toggling --enable-itrace / --enable-dtrace then recompiles just
# this file instead of forcing a full rebuild.
set(_x64_tracer_defs "")
if(XENIA_ENABLE_ITRACE)
  list(APPEND _x64_tracer_defs XE_OPTION_TRACE_INSTR=1)
endif()
if(XENIA_ENABLE_DTRACE)
  list(APPEND _x64_tracer_defs XE_OPTION_TRACE_DATA=1)
endif()
if(XENIA_ENABLE_FTRACE)
  list(APPEND _x64_tracer_defs XE_OPTION_TRACE_FUNC=1)
endif()
if(_x64_tracer_defs)
  set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/x64_tracers.cc
    PROPERTIES COMPILE_DEFINITIONS "${_x64_tracer_defs}")
endif()
