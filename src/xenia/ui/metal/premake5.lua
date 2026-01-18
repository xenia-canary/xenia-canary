project_root = "../../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-ui-metal")
  uuid("f5a4b3c2-d1e8-4567-9abc-def123456789")
  language("C++")
  
  filter("system:macosx")
    kind("StaticLib")
  filter("not system:macosx")
    kind("None")
  filter({})
  
  links({
    "xenia-base",
    "xenia-ui",
    "metal-cpp",
  })
  includedirs({
    project_root.."/third_party/metal-cpp",
  })
  local_platform_files()
  files({
    "../shaders/bytecode/metal/*.h",
  })

  filter("system:macosx")
    links({
      "Metal.framework",
      "MetalFX.framework",
      "MetalKit.framework",
      "QuartzCore.framework",
    })
    files({
      "metal_immediate_drawer.mm",
      "metal_presenter.mm",
    })
  filter({})

if enableMiscSubprojects then
  group("demos")
  project("xenia-ui-window-metal-demo")
    uuid("c1d2e3f4-5a6b-7c8d-9e0f-123456789abc")
    language("C++")

    filter("system:macosx")
      single_library_windowed_app_kind()
    filter("not system:macosx")
      kind("None")
    filter({})

    links({
      "fmt",
      "imgui",
      "xenia-hid",
      "xenia-base",
      "xenia-ui",
      "xenia-ui-metal",
      "metal-cpp",
    })
    includedirs({
      project_root.."/third_party/metal-cpp",
    })
    files({
      "../window_demo.cc",
      "metal_window_demo.cc",
      project_root.."/src/xenia/ui/windowed_app_main_"..platform_suffix..".cc",
    })
    resincludedirs({
      project_root,
    })

    filter("system:macosx")
    links({
      "Cocoa.framework",
      "Metal.framework",
      "MetalFX.framework",
      "MetalKit.framework",
      "QuartzCore.framework",
    })
      files({
        "Info.plist",
        project_root.."/xenia.entitlements",
      })
      buildoptions({
        "-DINFOPLIST_FILE=" .. path.getabsolute("Info.plist"),
      })
      xcodebuildsettings({
        ["INFOPLIST_FILE"] = path.getabsolute("Info.plist"),
        ["MACOSX_DEPLOYMENT_TARGET"] = "15.0",
        ["PRODUCT_BUNDLE_IDENTIFIER"] = "com.xenia.ui-metal-demo",
        ["CODE_SIGN_STYLE"] = "Automatic",
        ["CODE_SIGN_ENTITLEMENTS"] =
            path.getabsolute(project_root.."/xenia.entitlements"),
      })
    filter({})
end
