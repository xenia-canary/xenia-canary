add_library(xenia-vfs STATIC)
xe_platform_sources(xenia-vfs ${CMAKE_CURRENT_SOURCE_DIR} RECURSIVE)
# Exclude vfs_dump.cc
file(GLOB _vfs_dump  "${CMAKE_CURRENT_SOURCE_DIR}/vfs_dump.cc")
# Exclude omnidrive_sg_probe_main.cc
file(GLOB _vfs_omnidrive_probe "${CMAKE_CURRENT_SOURCE_DIR}/omnidrive_sg_probe_main.cc")
get_target_property(_vfs_srcs xenia-vfs SOURCES)
if((_vfs_dump OR _vfs_omnidrive_probe) AND _vfs_srcs)
  list(REMOVE_ITEM _vfs_srcs ${_vfs_dump})
  list(REMOVE_ITEM _vfs_srcs ${_vfs_omnidrive_probe})
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

  # Omnidrive SG probe tool
  add_executable(xenia-vfs-omnidrive-sg-probe
    ${CMAKE_CURRENT_SOURCE_DIR}/omnidrive_sg_probe_main.cc
  )
  if(WIN32)
    target_sources(xenia-vfs-omnidrive-sg-probe PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
  else()
    target_sources(xenia-vfs-omnidrive-sg-probe PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
  endif()
  target_link_libraries(xenia-vfs-omnidrive-sg-probe PRIVATE fmt xenia-base xenia-vfs)
  xe_target_defaults(xenia-vfs-omnidrive-sg-probe)
endif()

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
endif()
