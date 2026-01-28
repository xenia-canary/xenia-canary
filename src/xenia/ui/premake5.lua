project_root = "../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-ui")
  uuid("d0407c25-b0ea-40dc-846c-82c46fbd9fa2")
  kind("StaticLib")
  language("C++")
  links({
    "xenia-base",
  })
  local_platform_files()
  removefiles({
    "*_demo.cc",
    "windowed_app_main_*.cc",
  })
  if os.istarget("android") then
    filter("platforms:Android-*")
      -- Exports JNI functions.
      wholelib("On")
  end

  filter("platforms:Windows")
    links({
      "dwmapi",
      "dxgi",
      "winmm",
    })

  filter("platforms:Linux")
    links({
      "xcb",
      "X11",
      "X11-xcb",
      "fontconfig"
    })

  filter("system:macosx")
    pkg_config.all("gtk+-3.0")
    pkg_config.all("glib-2.0")
    pkg_config.all("x11")
    pkg_config.all("xcb")
    pkg_config.all("x11-xcb")
    pkg_config.all("fontconfig")
    -- Yes this is likely where you can add some Assembly flags to deal with macOS's architecture
    linkoptions ({
       "-Wl",
       "-stack_size,0x4000000" 
    })
