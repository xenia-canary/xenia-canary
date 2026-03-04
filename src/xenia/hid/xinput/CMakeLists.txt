add_library(xenia-hid-xinput STATIC)
xe_platform_sources(xenia-hid-xinput ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-xinput PUBLIC xenia-base xenia-hid)
xe_target_defaults(xenia-hid-xinput)
