xe_test_suite(xenia-hid-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS
    capstone fmt imgui SDL2
    xenia-base xenia-core xenia-cpu xenia-gpu
    xenia-helper-sdl xenia-hid xenia-hid-sdl
    xenia-kernel xenia-ui xenia-patcher
    $<$<BOOL:${XE_TARGET_X86_64}>:xenia-cpu-backend-x64>
    $<$<BOOL:${XE_TARGET_AARCH64}>:xenia-cpu-backend-a64>
)
