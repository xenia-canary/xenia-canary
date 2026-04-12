add_library(xenia-ui STATIC)
xe_platform_sources(xenia-ui ${CMAKE_CURRENT_SOURCE_DIR})
# Exclude demo, windowed_app_main, and wx files on Android (no xe-wxWidgets)
file(GLOB _ui_exclude   "${CMAKE_CURRENT_SOURCE_DIR}/*_demo.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/windowed_app_main_*.cc"
)
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  file(GLOB _wx_exclude "${CMAKE_CURRENT_SOURCE_DIR}/*_wx.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*_wx.h"
  )
  list(APPEND _ui_exclude ${_wx_exclude})
endif()
get_target_property(_ui_srcs xenia-ui SOURCES)
if(_ui_exclude AND _ui_srcs)
  list(REMOVE_ITEM _ui_srcs ${_ui_exclude})
  set_target_properties(xenia-ui PROPERTIES SOURCES "${_ui_srcs}")
endif()
target_link_libraries(xenia-ui PUBLIC xenia-base)
target_link_libraries(xenia-ui PUBLIC xe-wxWidgets)
if(WIN32)
  target_link_libraries(xenia-ui PRIVATE dxgi)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # xcb, X11, X11-xcb needed for Vulkan surface creation in window_wx.cc
  # fontconfig needed by imgui_drawer.cc
  # GTK3 needed for XCB handle extraction in window_wx.cc (gtk_widget_realize, etc.)
  target_link_libraries(xenia-ui PRIVATE xcb X11 X11-xcb fontconfig ${GTK3_LIBRARIES})
endif()
xe_target_defaults(xenia-ui)
