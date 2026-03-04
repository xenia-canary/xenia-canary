add_library(xenia-apu-xaudio2 STATIC)
xe_platform_sources(xenia-apu-xaudio2 ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-apu-xaudio2 PUBLIC xenia-base xenia-apu)
xe_target_defaults(xenia-apu-xaudio2)
