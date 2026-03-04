add_library(xenia-vfs STATIC)
xe_platform_sources(xenia-vfs ${CMAKE_CURRENT_SOURCE_DIR} RECURSIVE)
# Exclude vfs_dump.cc
file(GLOB _vfs_dump  "${CMAKE_CURRENT_SOURCE_DIR}/vfs_dump.cc")
get_target_property(_vfs_srcs xenia-vfs SOURCES)
if(_vfs_dump AND _vfs_srcs)
  list(REMOVE_ITEM _vfs_srcs ${_vfs_dump})
  set_target_properties(xenia-vfs PROPERTIES SOURCES "${_vfs_srcs}")
endif()
target_link_libraries(xenia-vfs PUBLIC xenia-base zstd zarchive)
xe_target_defaults(xenia-vfs)

if(XENIA_BUILD_MISC)
  # VFS dump tool
  add_executable(xenia-vfs-dump
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs_dump.cc
  )
  if(WIN32)
    target_sources(xenia-vfs-dump PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
  else()
    target_sources(xenia-vfs-dump PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
  endif()
  target_link_libraries(xenia-vfs-dump PRIVATE fmt xenia-base xenia-vfs)
  xe_target_defaults(xenia-vfs-dump)
endif()

if(XENIA_BUILD_TESTS)
  add_subdirectory(testing)
endif()
