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
