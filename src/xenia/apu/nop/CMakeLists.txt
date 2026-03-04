add_library(xenia-apu-nop STATIC)
xe_platform_sources(xenia-apu-nop ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-apu-nop PUBLIC xenia-base xenia-apu)
xe_target_defaults(xenia-apu-nop)
