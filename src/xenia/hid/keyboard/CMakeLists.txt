add_library(xenia-hid-keyboard STATIC)
xe_platform_sources(xenia-hid-keyboard ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-keyboard PUBLIC xenia-base xenia-hid xenia-ui)
xe_target_defaults(xenia-hid-keyboard)
