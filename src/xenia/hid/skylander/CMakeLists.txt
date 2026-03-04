add_library(xenia-hid-skylander STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/skylander_portal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/skylander_portal.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/skylander_emulated.h
  ${CMAKE_CURRENT_SOURCE_DIR}/skylander_emulated.cc
)
if(WIN32)
  target_sources(xenia-hid-skylander PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/skylander_hardware.h
    ${CMAKE_CURRENT_SOURCE_DIR}/skylander_hardware.cc
  )
  target_link_libraries(xenia-hid-skylander PUBLIC libusb)
endif()
xe_target_defaults(xenia-hid-skylander)
