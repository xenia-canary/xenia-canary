# src/xenia/CMakeLists.txt - Master file for all xenia modules.

# xenia-core (this directory's own sources)
add_library(xenia-core STATIC)
file(GLOB _core_sources   "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)
target_sources(xenia-core PRIVATE ${_core_sources})
target_link_libraries(xenia-core PUBLIC fmt xenia-base)
xe_target_defaults(xenia-core)

# All subdirectories
add_subdirectory(base)
add_subdirectory(cpu)
add_subdirectory(cpu/backend/x64)
add_subdirectory(apu)
add_subdirectory(apu/nop)
add_subdirectory(gpu)
add_subdirectory(gpu/null)
add_subdirectory(gpu/vulkan)
add_subdirectory(hid)
add_subdirectory(hid/nop)
add_subdirectory(hid/skylander)
add_subdirectory(kernel)
add_subdirectory(patcher)
add_subdirectory(ui)
add_subdirectory(ui/vulkan)
add_subdirectory(vfs)
add_subdirectory(debug/ui)
add_subdirectory(app/discord)
add_subdirectory(app)

# SDL-based modules (not Android)
add_subdirectory(apu/sdl)
add_subdirectory(helper/sdl)
add_subdirectory(hid/sdl)

# Platform-conditional modules
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(apu/alsa)
endif()

if(WIN32)
  add_subdirectory(apu/xaudio2)
  add_subdirectory(gpu/d3d12)
  add_subdirectory(hid/winkey)
  add_subdirectory(hid/xinput)
  add_subdirectory(ui/d3d12)
endif()
