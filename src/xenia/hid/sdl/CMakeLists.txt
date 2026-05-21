add_library(xenia-hid-sdl STATIC)
xe_platform_sources(xenia-hid-sdl ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-sdl PUBLIC xenia-base xenia-hid xenia-ui SDL2)
xe_target_defaults(xenia-hid-sdl)

# Bake the bundled controller mappings.
xe_embed_compressed_bundle(xenia-hid-sdl
    "${PROJECT_SOURCE_DIR}/build/data_repos/SDL_GameControllerDB/gamecontrollerdb.txt"
    gamecontrollerdb)
