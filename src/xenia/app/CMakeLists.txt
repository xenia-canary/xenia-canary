# src/xenia/app/CMakeLists.txt - Main xenia_canary executable.

if(WIN32)
  add_executable(xenia-app WIN32)
elseif(APPLE)
  add_executable(xenia-app MACOSX_BUNDLE)
else()
  add_executable(xenia-app)
endif()

set_target_properties(xenia-app PROPERTIES OUTPUT_NAME "xenia_canary")

if(APPLE)
  set_target_properties(xenia-app PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/assets/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "Xenia Canary"
    MACOSX_BUNDLE_EXECUTABLE_NAME "xenia_canary"
    MACOSX_BUNDLE_GUI_IDENTIFIER "jp.xenia.canary"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
  )
  # Build the app icon from the PNG icons when the Xcode tools are available.
  find_program(XE_ICONUTIL iconutil)
  if(XE_ICONUTIL)
    set(_iconset "${CMAKE_CURRENT_BINARY_DIR}/xenia_canary.iconset")
    set(_icns "${CMAKE_CURRENT_BINARY_DIR}/xenia_canary.icns")
    set(_icon_dir "${PROJECT_SOURCE_DIR}/assets/icon")
    add_custom_command(
      OUTPUT "${_icns}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_iconset}"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/16.png" "${_iconset}/icon_16x16.png"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/32.png" "${_iconset}/icon_16x16@2x.png"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/32.png" "${_iconset}/icon_32x32.png"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/64.png" "${_iconset}/icon_32x32@2x.png"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/128.png" "${_iconset}/icon_128x128.png"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/256.png" "${_iconset}/icon_128x128@2x.png"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/256.png" "${_iconset}/icon_256x256.png"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/512.png" "${_iconset}/icon_256x256@2x.png"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/512.png" "${_iconset}/icon_512x512.png"
      COMMAND ${CMAKE_COMMAND} -E copy "${_icon_dir}/1024.png" "${_iconset}/icon_512x512@2x.png"
      COMMAND "${XE_ICONUTIL}" -c icns -o "${_icns}" "${_iconset}"
      DEPENDS "${_icon_dir}/16.png" "${_icon_dir}/32.png" "${_icon_dir}/64.png"
              "${_icon_dir}/128.png" "${_icon_dir}/256.png" "${_icon_dir}/512.png"
              "${_icon_dir}/1024.png"
      COMMENT "Generating the macOS app icon"
      VERBATIM
    )
    target_sources(xenia-app PRIVATE "${_icns}")
    set_source_files_properties("${_icns}" PROPERTIES
      MACOSX_PACKAGE_LOCATION "Resources")
    set_target_properties(xenia-app PROPERTIES
      MACOSX_BUNDLE_ICON_FILE "xenia_canary.icns")
  endif()
endif()

# Main source
target_sources(xenia-app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/xenia_main.cc
)

# Other app sources (emulator_window, profile_dialogs, etc.)
xe_platform_sources(xenia-app ${CMAKE_CURRENT_SOURCE_DIR})

# Platform-specific entry points
if(WIN32)
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_win.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/main_resources.rc
    ${PROJECT_SOURCE_DIR}/src/xenia/base/app_win32.manifest
  )
  if(XE_TARGET_X86_64)
    # main_init_win.cc needs SSE2 only (not AVX)
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
      PROPERTIES COMPILE_OPTIONS "/arch:SSE2"
    )
  endif()
elseif(APPLE)
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_posix.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_mac.mm
  )
else()
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_posix.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_posix.cc
  )
  if(NOT MSVC AND XE_TARGET_X86_64)
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_posix.cc
      PROPERTIES COMPILE_OPTIONS "-msse2;-mno-avx"
    )
  endif()
endif()

target_compile_definitions(xenia-app PRIVATE
  XBYAK_NO_OP_NAMES
  XBYAK_ENABLE_OMITTED_OPERAND
)

# Core libraries (all platforms)
target_link_libraries(xenia-app PRIVATE
  xenia-apu
  xenia-apu-nop
  xenia-base
  xenia-core
  xenia-cpu
  xenia-gpu
  xenia-gpu-null
  xenia-hid
  xenia-hid-nop
  xenia-kernel
  xenia-patcher
  xenia-ui
  xenia-vfs
  # Third-party
  aes_128
  capstone
  fmt
  dxbc
  discord-rpc
  imgui
  libavcodec
  libavutil
  mspack
  snappy
  xxhash
)

# Vulkan/SPIR-V backends (MoltenVK on macOS)
target_link_libraries(xenia-app PRIVATE
  xenia-gpu-vulkan
  xenia-ui-vulkan
  glslang-spirv
)

# Non-Android specific (always true for Windows/Linux)
target_link_libraries(xenia-app PRIVATE
  xenia-app-discord
  xenia-apu-sdl
  xenia-debug-ui
  xenia-helper-sdl
  xenia-hid-sdl
  xenia-hid-keyboard
)

# Architecture-specific backend
if(XE_TARGET_X86_64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-a64)
endif()

# Platform-specific libraries
if(WIN32)
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-xaudio2
    xenia-gpu-d3d12
    xenia-hid-xinput
    xenia-ui-d3d12
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-alsa
    X11
    xcb
    X11-xcb
    SDL2
  )
endif()

xe_target_defaults(xenia-app)
