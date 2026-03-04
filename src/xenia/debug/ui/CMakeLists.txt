add_library(xenia-debug-ui STATIC)
xe_platform_sources(xenia-debug-ui ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-debug-ui PUBLIC imgui xenia-base xenia-cpu xenia-ui)
xe_target_defaults(xenia-debug-ui)
