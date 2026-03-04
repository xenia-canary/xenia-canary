xe_test_suite(xenia-cpu-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS
    capstone fmt imgui
    xenia-base xenia-core xenia-cpu xenia-gpu
    xenia-hid-skylander xenia-kernel xenia-ui xenia-patcher
    $<$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x86_64>:xenia-cpu-backend-x64>
)
