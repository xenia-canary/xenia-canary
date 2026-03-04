add_library(xenia-ui-vulkan STATIC)
xe_platform_sources(xenia-ui-vulkan ${CMAKE_CURRENT_SOURCE_DIR})
xe_platform_sources(xenia-ui-vulkan ${CMAKE_CURRENT_SOURCE_DIR}/functions)
# Include shader bytecode headers
file(GLOB _vk_ui_shader_headers   "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/bytecode/vulkan_spirv/*.h"
)
target_sources(xenia-ui-vulkan PRIVATE ${_vk_ui_shader_headers})
target_include_directories(xenia-ui-vulkan PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
  ${PROJECT_SOURCE_DIR}/third_party/glslang
)
target_link_libraries(xenia-ui-vulkan PUBLIC xenia-base xenia-ui)
xe_target_defaults(xenia-ui-vulkan)

if(XENIA_BUILD_MISC)
  # Vulkan window demo
  add_executable(xenia-ui-window-vulkan-demo)
  if(WIN32)
    set_target_properties(xenia-ui-window-vulkan-demo PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(xenia-ui-window-vulkan-demo PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_win.cc)
  else()
    target_sources(xenia-ui-window-vulkan-demo PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_posix.cc)
  endif()
  target_sources(xenia-ui-window-vulkan-demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../window_demo.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_window_demo.cc
  )
  target_include_directories(xenia-ui-window-vulkan-demo PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
  )
  target_link_libraries(xenia-ui-window-vulkan-demo PRIVATE
    fmt imgui xenia-base xenia-ui xenia-ui-vulkan
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(xenia-ui-window-vulkan-demo PRIVATE X11 xcb X11-xcb)
  endif()
  xe_target_defaults(xenia-ui-window-vulkan-demo)
endif()
