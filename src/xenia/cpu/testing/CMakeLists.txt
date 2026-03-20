xe_test_suite(xenia-cpu-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS
    capstone fmt imgui
    xenia-base xenia-core xenia-cpu xenia-gpu
    xenia-kernel xenia-ui xenia-patcher
    $<$<BOOL:${XE_TARGET_X86_64}>:xenia-cpu-backend-x64>
    $<$<BOOL:${XE_TARGET_AARCH64}>:xenia-cpu-backend-a64>
)
