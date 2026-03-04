add_library(xenia-patcher STATIC)
xe_platform_sources(xenia-patcher ${CMAKE_CURRENT_SOURCE_DIR} RECURSIVE)
target_link_libraries(xenia-patcher PUBLIC xenia-base)
xe_target_defaults(xenia-patcher)
