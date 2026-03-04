add_library(xenia-helper-sdl STATIC)
xe_platform_sources(xenia-helper-sdl ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-helper-sdl PUBLIC SDL2)
xe_target_defaults(xenia-helper-sdl)
