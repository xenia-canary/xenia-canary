add_library(xenia-apu-sdl STATIC)
xe_platform_sources(xenia-apu-sdl ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-apu-sdl PUBLIC xenia-apu xenia-base xenia-helper-sdl SDL2)
xe_target_defaults(xenia-apu-sdl)
