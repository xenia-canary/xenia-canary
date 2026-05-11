add_library(xenia-ui STATIC)
xe_platform_sources(xenia-ui ${CMAKE_CURRENT_SOURCE_DIR})
# Demo files are standalone executables in XENIA_BUILD_MISC; windowed_app_main_*
# files are added explicitly per-target. wx files are excluded on Android (no
# xe-wxWidgets target there).
file(GLOB _ui_exclude
  "${CMAKE_CURRENT_SOURCE_DIR}/*_demo.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/windowed_app_main_*.cc"
)
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  file(GLOB _wx_exclude
    "${CMAKE_CURRENT_SOURCE_DIR}/*_wx.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*_wx.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/wx_locale.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/wx_locale.h"
  )
  list(APPEND _ui_exclude ${_wx_exclude})
endif()
get_target_property(_ui_srcs xenia-ui SOURCES)
if(_ui_exclude AND _ui_srcs)
  list(REMOVE_ITEM _ui_srcs ${_ui_exclude})
  set_target_properties(xenia-ui PROPERTIES SOURCES "${_ui_srcs}")
endif()
target_link_libraries(xenia-ui PUBLIC xenia-base)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
  target_link_libraries(xenia-ui PUBLIC xe-wxWidgets)
endif()
if(WIN32)
  target_link_libraries(xenia-ui PRIVATE dxgi)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # xcb / X11 / X11-xcb: Vulkan surface creation in window_wx.cc.
  # fontconfig: imgui_drawer.cc font lookup.
  # GTK3: XCB handle extraction in window_wx.cc (gtk_widget_realize, etc.).
  target_link_libraries(xenia-ui PRIVATE xcb X11 X11-xcb fontconfig ${GTK3_LIBRARIES})
elseif(APPLE)
  # xe_platform_sources globs *.cc only; .mm sources need explicit listing.
  target_sources(xenia-ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/surface_mac.mm
    ${CMAKE_CURRENT_SOURCE_DIR}/windowed_app_wx_mac.mm
  )
  target_link_libraries(xenia-ui PRIVATE
    "-framework Cocoa"
    "-framework QuartzCore"
  )
endif()
xe_target_defaults(xenia-ui)

xe_embed_binary_assets(xenia-ui
    "${PROJECT_SOURCE_DIR}/assets/font" font)
xe_embed_binary_assets(xenia-ui
    "${PROJECT_SOURCE_DIR}/assets/icon" app_icon)

# Compile assets/locale/<lang>/xenia.po -> <lang>/xenia.mo at configure
# time, then bundle the staging tree into the binary. wx_locale.cc decodes
# the bundle and feeds it to wxTranslations via a custom loader, so no
# .mo files ship alongside the exe.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(_xe_locale_stage "${CMAKE_CURRENT_BINARY_DIR}/locale_staged")
  file(GLOB _xe_po_files
       "${PROJECT_SOURCE_DIR}/assets/locale/*/xenia.po")
  foreach(_xe_po IN LISTS _xe_po_files)
    get_filename_component(_xe_po_dir "${_xe_po}" DIRECTORY)
    get_filename_component(_xe_lang "${_xe_po_dir}" NAME)
    set(_xe_mo "${_xe_locale_stage}/${_xe_lang}/xenia.mo")
    file(MAKE_DIRECTORY "${_xe_locale_stage}/${_xe_lang}")
    execute_process(
      COMMAND ${Python3_EXECUTABLE}
              "${PROJECT_SOURCE_DIR}/tools/build/compile_po.py"
              "${_xe_po}" "${_xe_mo}"
      RESULT_VARIABLE _xe_rc)
    if(NOT _xe_rc EQUAL 0)
      message(FATAL_ERROR "compile_po.py failed for ${_xe_po}")
    endif()
  endforeach()
  xe_embed_compressed_bundle(xenia-ui "${_xe_locale_stage}" locale)
endif()
