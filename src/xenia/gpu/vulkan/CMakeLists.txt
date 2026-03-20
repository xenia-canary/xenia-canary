add_library(xenia-gpu-vulkan STATIC)
xe_platform_sources(xenia-gpu-vulkan ${CMAKE_CURRENT_SOURCE_DIR})
xe_shader_rules_spirv(xenia-gpu-vulkan ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
target_include_directories(xenia-gpu-vulkan PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
  ${PROJECT_SOURCE_DIR}/third_party/glslang
)
target_link_libraries(xenia-gpu-vulkan PUBLIC
  fmt glslang-spirv xxhash xenia-base xenia-gpu xenia-ui xenia-ui-vulkan
)
xe_target_defaults(xenia-gpu-vulkan)

if(XENIA_BUILD_MISC)
  # Vulkan trace viewer
  add_executable(xenia-gpu-vulkan-trace-viewer
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_trace_viewer_main.cc
  )
  if(WIN32)
    target_sources(xenia-gpu-vulkan-trace-viewer PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_win.cc)
  else()
    target_sources(xenia-gpu-vulkan-trace-viewer PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_posix.cc)
  endif()
  target_include_directories(xenia-gpu-vulkan-trace-viewer PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
    ${PROJECT_SOURCE_DIR}/third_party/glslang
  )
  target_link_libraries(xenia-gpu-vulkan-trace-viewer PRIVATE
    xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-gpu-vulkan xenia-hid xenia-hid-nop
    xenia-kernel xenia-patcher xenia-ui xenia-ui-vulkan xenia-vfs
    aes_128 capstone fmt glslang-spirv imgui libavcodec libavutil mspack snappy xxhash
  )
  if(XE_TARGET_X86_64)
    target_link_libraries(xenia-gpu-vulkan-trace-viewer PRIVATE xenia-cpu-backend-x64)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(xenia-gpu-vulkan-trace-viewer PRIVATE X11 xcb X11-xcb)
  endif()
  xe_target_defaults(xenia-gpu-vulkan-trace-viewer)

  # Vulkan trace dump
  add_executable(xenia-gpu-vulkan-trace-dump
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_trace_dump_main.cc
  )
  if(WIN32)
    target_sources(xenia-gpu-vulkan-trace-dump PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
  else()
    target_sources(xenia-gpu-vulkan-trace-dump PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
  endif()
  target_include_directories(xenia-gpu-vulkan-trace-dump PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
    ${PROJECT_SOURCE_DIR}/third_party/glslang
  )
  target_link_libraries(xenia-gpu-vulkan-trace-dump PRIVATE
    xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-gpu-vulkan xenia-hid xenia-hid-nop
    xenia-kernel xenia-patcher xenia-ui xenia-ui-vulkan xenia-vfs
    aes_128 capstone fmt glslang-spirv imgui libavcodec libavutil mspack snappy xxhash
  )
  if(XE_TARGET_X86_64)
    target_link_libraries(xenia-gpu-vulkan-trace-dump PRIVATE xenia-cpu-backend-x64)
  endif()
  xe_target_defaults(xenia-gpu-vulkan-trace-dump)
endif()
