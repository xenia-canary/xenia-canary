add_library(xenia-avatars STATIC
  ${PLATFORM_SOURCES}
)
xe_platform_sources(xenia-avatars ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(xenia-avatars
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/spirv-tools/external/include
)
target_link_libraries(xenia-avatars PRIVATE xenia-base)
xe_target_defaults(xenia-avatars)
