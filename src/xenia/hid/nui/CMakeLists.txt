# Kinectix NUI library — INuiBackend, NuiManager, recorded playback backend.
#
# Optional backends (libfreenect, libfreenect2, MediaPipe) are gated by
# CMake options and not yet implemented; the option declarations exist
# so that future enable-and-build works without surgery.

add_library(xenia-hid-nui STATIC)

xe_platform_sources(xenia-hid-nui ${CMAKE_CURRENT_SOURCE_DIR})

# Pull in the recorded backend sources from the recorded/ subdirectory.
# xe_platform_sources only globs the immediate dir, so we add this manually.
target_sources(xenia-hid-nui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/recorded/recorded_backend.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/recorded/recorded_backend.h
  ${CMAKE_CURRENT_SOURCE_DIR}/recorded/xnuirec_reader.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/recorded/xnuirec_reader.h
)

target_link_libraries(xenia-hid-nui PUBLIC xenia-base xenia-hid)

# -----------------------------------------------------------------------------
# Optional backends. Each adds external dependencies and must be opt-in.
# Toggle at configure time:  cmake -DKINECTIX_NUI_FREENECT=ON ...
# -----------------------------------------------------------------------------

option(KINECTIX_NUI_FREENECT  "Build the libfreenect (Kinect v1) backend"  OFF)
option(KINECTIX_NUI_FREENECT2 "Build the libfreenect2 (Kinect v2) backend" OFF)
option(KINECTIX_NUI_MEDIAPIPE "Build the MediaPipe (webcam pose) backend"  OFF)

if(KINECTIX_NUI_FREENECT)
  target_compile_definitions(xenia-hid-nui PUBLIC XE_KINECTIX_NUI_FREENECT=1)
  # TODO: target_sources(... freenect/freenect_backend.cc) once written.
  # TODO: find_package(Freenect REQUIRED) or vendored equivalent.
  message(STATUS "KINECTIX_NUI_FREENECT enabled, but backend sources are not "
                 "yet implemented. Stage 2 in ROADMAP.md.")
endif()

if(KINECTIX_NUI_FREENECT2)
  target_compile_definitions(xenia-hid-nui PUBLIC XE_KINECTIX_NUI_FREENECT2=1)
  message(STATUS "KINECTIX_NUI_FREENECT2 enabled, but backend sources are not "
                 "yet implemented. Stage 2b in ROADMAP.md.")
endif()

if(KINECTIX_NUI_MEDIAPIPE)
  target_compile_definitions(xenia-hid-nui PUBLIC XE_KINECTIX_NUI_MEDIAPIPE=1)
  message(STATUS "KINECTIX_NUI_MEDIAPIPE enabled, but backend sources are not "
                 "yet implemented. Stage 3 in ROADMAP.md.")
endif()

xe_target_defaults(xenia-hid-nui)
