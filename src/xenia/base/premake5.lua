project_root = "../../.."
include(project_root.."/tools/build")

project("xenia-base")
  uuid("aeadaf22-2b20-4941-b05f-a802d5679c11")
  kind("StaticLib")
  language("C++")
  links({
    "fmt",
  })
  local_platform_files()
  removefiles({
    "console_app_main_*.cc",
    "main_init_*.cc",
  })
  filter("architecture:ARM64")
    removefiles({
      "clock_x64.cc",
      "platform_amd64.cc",
      "platform_amd64.h",
    })
  filter({})
  files({
    "debug_visualizers.natvis",
  })

if enableTests then
  include("testing")
end
