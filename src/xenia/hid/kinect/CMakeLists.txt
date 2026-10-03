add_library(xenia-hid-kinect STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/usb_camera.h
)

if(WIN32)
  target_sources(xenia-hid-kinect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_camera_win.cc
  )
  target_link_libraries(xenia-hid-kinect PUBLIC libusb)
else()
  target_sources(xenia-hid-kinect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_camera_posix.cc
  )
endif()
target_link_libraries(xenia-hid-kinect PUBLIC xenia-base)
xe_target_defaults(xenia-hid-kinect)
