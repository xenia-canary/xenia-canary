add_library(xenia-app-discord STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/discord_presence.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/discord_presence.h
)
target_include_directories(xenia-app-discord PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/discord-rpc/src
)
target_link_libraries(xenia-app-discord PUBLIC discord-rpc)
xe_target_defaults(xenia-app-discord)
