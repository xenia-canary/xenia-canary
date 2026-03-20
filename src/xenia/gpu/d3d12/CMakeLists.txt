add_library(xenia-gpu-d3d12 STATIC)
xe_platform_sources(xenia-gpu-d3d12 ${CMAKE_CURRENT_SOURCE_DIR})
xe_shader_rules_dxbc(xenia-gpu-d3d12 ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
target_link_libraries(xenia-gpu-d3d12 PUBLIC
  fmt xxhash xenia-base xenia-gpu xenia-ui xenia-ui-d3d12
)
xe_target_defaults(xenia-gpu-d3d12)

if(XENIA_BUILD_MISC)
  # D3D12 trace viewer
  add_executable(xenia-gpu-d3d12-trace-viewer WIN32
    ${CMAKE_CURRENT_SOURCE_DIR}/d3d12_trace_viewer_main.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_win.cc
  )
  target_link_libraries(xenia-gpu-d3d12-trace-viewer PRIVATE
    xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-gpu-d3d12 xenia-hid xenia-hid-nop
    xenia-kernel xenia-patcher xenia-ui xenia-ui-d3d12 xenia-vfs
    aes_128 capstone dxbc fmt imgui libavcodec libavutil mspack snappy xxhash
  )
  if(XE_TARGET_X86_64)
    target_link_libraries(xenia-gpu-d3d12-trace-viewer PRIVATE xenia-cpu-backend-x64)
  endif()
  xe_target_defaults(xenia-gpu-d3d12-trace-viewer)

  # D3D12 trace dump
  add_executable(xenia-gpu-d3d12-trace-dump
    ${CMAKE_CURRENT_SOURCE_DIR}/d3d12_trace_dump_main.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc
  )
  target_link_libraries(xenia-gpu-d3d12-trace-dump PRIVATE
    xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-gpu-d3d12 xenia-hid xenia-hid-nop
    xenia-kernel xenia-patcher xenia-ui xenia-ui-d3d12 xenia-vfs
    aes_128 capstone dxbc fmt imgui libavcodec libavutil mspack snappy xxhash
  )
  if(XE_TARGET_X86_64)
    target_link_libraries(xenia-gpu-d3d12-trace-dump PRIVATE xenia-cpu-backend-x64)
  endif()
  xe_target_defaults(xenia-gpu-d3d12-trace-dump)
endif()
