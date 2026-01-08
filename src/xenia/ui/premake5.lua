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
    includedirs({
      "/usr/local/include",
      "/opt/homebrew/include",
      "/opt/homebrew/include/gtk-3.0",
      "/opt/homebrew/include/glib-2.0",
      "/opt/homebrew/lib/glib-2.0/include",
      "/opt/homebrew/include/pango-1.0",
      "/opt/homebrew/include/cairo",
      "/opt/homebrew/include/gdk-pixbuf-2.0",
      "/opt/homebrew/include/atk-1.0",
      "/opt/homebrew/include/harfbuzz",
    })
    links({
      "fontconfig",
      "gtk"
    })
