if(NOT XENIA_BUILD_TESTS)
  return()
endif()

add_executable(xenia-cpu-ppc-tests
  ${CMAKE_CURRENT_SOURCE_DIR}/ppc_testing_main.cc
)

# Add platform-specific console app main
if(WIN32)
  target_sources(xenia-cpu-ppc-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
else()
  target_sources(xenia-cpu-ppc-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
endif()

# Add .s files but exclude from build (they're test data)
file(GLOB _ppc_asm  "${CMAKE_CURRENT_SOURCE_DIR}/*.s")
target_sources(xenia-cpu-ppc-tests PRIVATE ${_ppc_asm})
set_source_files_properties(${_ppc_asm} PROPERTIES HEADER_FILE_ONLY TRUE)

target_link_libraries(xenia-cpu-ppc-tests PRIVATE
  xenia-apu xenia-apu-nop
  xenia-base xenia-core xenia-cpu
  xenia-gpu xenia-gpu-null
  xenia-hid xenia-hid-nop
  xenia-kernel xenia-patcher xenia-ui xenia-vfs
  capstone fmt imgui mspack
)

# x64 backend
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  target_link_libraries(xenia-cpu-ppc-tests PRIVATE xenia-cpu-backend-x64)
endif()

xe_target_defaults(xenia-cpu-ppc-tests)
