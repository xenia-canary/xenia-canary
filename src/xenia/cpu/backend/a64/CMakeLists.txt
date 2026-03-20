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
