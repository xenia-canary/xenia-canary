# cmake --install support

install(TARGETS "xenia-app" COMPONENT Executable)
if (UNIX AND NOT APPLE)
  option(XENIA_ICON_OUTLINED "Whether to install the version of the icon with a white outline or without" OFF)
  if (XENIA_ICON_OUTLINED)
    string(APPEND XENIA_ICON_FILE jp.xenia.xenia-canary_outline.svg)
  else()
    string(APPEND XENIA_ICON_FILE jp.xenia.xenia-canary.svg)
  endif()

  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/${XENIA_ICON_FILE}
          DESTINATION share/icons/hicolor/scalable/apps/
          RENAME jp.xenia.xenia-canary.svg
          PERMISSIONS WORLD_READ GROUP_READ GROUP_EXECUTE OWNER_READ OWNER_WRITE)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/freedesktop/jp.xenia.xenia-canary.metainfo.xml
          DESTINATION share/metainfo/
          PERMISSIONS WORLD_READ GROUP_READ GROUP_EXECUTE OWNER_READ OWNER_WRITE)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/freedesktop/jp.xenia.xenia-canary.desktop
          DESTINATION share/applications/
          PERMISSIONS WORLD_READ GROUP_READ GROUP_EXECUTE OWNER_READ OWNER_WRITE)
endif()
