add_library(xenia-hid-winkey STATIC)
xe_platform_sources(xenia-hid-winkey ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-winkey PUBLIC xenia-base xenia-hid xenia-ui)
xe_target_defaults(xenia-hid-winkey)
