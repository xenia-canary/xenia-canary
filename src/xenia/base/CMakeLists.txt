add_library(xenia-base STATIC)
xe_platform_sources(xenia-base ${CMAKE_CURRENT_SOURCE_DIR})
# Exclude console_app_main and main_init files
file(GLOB _base_exclude   "${CMAKE_CURRENT_SOURCE_DIR}/console_app_main_*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/main_init_*.cc"
)
get_target_property(_base_srcs xenia-base SOURCES)
if(_base_exclude AND _base_srcs)
  list(REMOVE_ITEM _base_srcs ${_base_exclude})
  set_target_properties(xenia-base PROPERTIES SOURCES "${_base_srcs}")
endif()
target_link_libraries(xenia-base PUBLIC fmt)
xe_target_defaults(xenia-base)

if(XENIA_BUILD_TESTS)
  add_subdirectory(testing)
endif()
