add_library(xenia-ui-d3d12 STATIC)
xe_platform_sources(xenia-ui-d3d12 ${CMAKE_CURRENT_SOURCE_DIR})
xe_shader_rules_dxbc(xenia-ui-d3d12 ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
target_link_libraries(xenia-ui-d3d12 PUBLIC xenia-base xenia-ui)
xe_target_defaults(xenia-ui-d3d12)

if(XENIA_BUILD_MISC)
  # D3D12 window demo
  add_executable(xenia-ui-window-d3d12-demo WIN32
    ${CMAKE_CURRENT_SOURCE_DIR}/../window_demo.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/d3d12_window_demo.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_win.cc
  )
  target_link_libraries(xenia-ui-window-d3d12-demo PRIVATE
    fmt imgui xenia-base xenia-core xenia-gpu xenia-kernel xenia-patcher xenia-ui xenia-ui-d3d12
    $<$<BOOL:${XE_TARGET_X86_64}>:xenia-cpu-backend-x64>
    $<$<BOOL:${XE_TARGET_AARCH64}>:xenia-cpu-backend-a64>
  )
  xe_target_defaults(xenia-ui-window-d3d12-demo)
endif()
