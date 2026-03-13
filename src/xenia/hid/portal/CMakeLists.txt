add_library(xenia-hid-portal STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/portal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/portal.cc
)

if(WIN32)
  target_sources(xenia-hid-portal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware_portal.h
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware_portal.cc
  )
  target_link_libraries(xenia-hid-portal PUBLIC libusb)
endif()
xe_target_defaults(xenia-hid-portal)
