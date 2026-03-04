add_library(xenia-hid-nop STATIC)
xe_platform_sources(xenia-hid-nop ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-nop PUBLIC xenia-base xenia-hid)
xe_target_defaults(xenia-hid-nop)
