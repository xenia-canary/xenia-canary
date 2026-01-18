project_root = path.getabsolute("../../..")
include(project_root.."/tools/build")
local metal_converter_libdir =
    path.join(project_root, "third_party/metal-shader-converter/lib")
local dxilconv_libdir_arm64 =
    path.join(project_root, "third_party/DirectXShaderCompiler/build_dxilconv_macos/lib")
local dxilconv_libdir_x86_64 =
    path.join(project_root,
              "third_party/DirectXShaderCompiler/build_dxilconv_macos_x86_64/lib")
local dxilconv_libdir = dxilconv_libdir_arm64
local lz4_libdir = "/opt/homebrew/opt/lz4/lib"
local sdl2_libdir = "/opt/homebrew/opt/sdl2/lib"
local info_plist_path =
    path.getabsolute(path.join(project_root, "src/xenia/app/Info.plist"))
local entitlements_path =
    path.getabsolute(path.join(project_root, "xenia.entitlements"))
local icon_path =
    path.getabsolute(path.join(project_root, "assets/icon/xenia.icns"))

group("src")
project("xenia-app")
  uuid("d7e98620-d007-4ad8-9dbd-b47c8853a17f")
  language("C++")
  links({
    "xenia-apu",
    "xenia-apu-nop",
    "xenia-base",
    "xenia-core",
    "xenia-cpu",
    "xenia-gpu",
    "xenia-gpu-null",
    "xenia-gpu-vulkan",
    "xenia-hid",
    "xenia-hid-nop",
    "xenia-kernel",
    "xenia-patcher",
    "xenia-ui",
    "xenia-ui-vulkan",
    "xenia-vfs",
  })
  links({
    "aes_128",
    "capstone",
    "fmt",
    "dxbc",
    "discord-rpc",
    "glslang-spirv",
    "imgui",
    "libavcodec",
    "libavutil",
    "mspack",
    "snappy",
    "xxhash",
  })
  defines({
    "XBYAK_NO_OP_NAMES",
    "XBYAK_ENABLE_OMITTED_OPERAND",
  })
  local_platform_files()
  sdl2_include()
  files({
    "../base/main_init_"..platform_suffix..".cc",
    "../ui/windowed_app_main_"..platform_suffix..".cc",
  })

  resincludedirs({
    project_root,
  })

  filter(SINGLE_LIBRARY_FILTER)
    -- Unified library containing all apps as StaticLibs, not just the main
    -- emulator windowed app.
    kind("SharedLib")
  if enableMiscSubprojects then
      links({
        "xenia-gpu-vulkan-trace-viewer",
        "xenia-hid-demo",
        "xenia-ui-window-vulkan-demo",
      })
  end
  filter(NOT_SINGLE_LIBRARY_FILTER)
    kind("WindowedApp")

  -- `targetname` is broken if building from Gradle, works only for toggling the
  -- `lib` prefix, as Gradle uses LOCAL_MODULE_FILENAME, not a derivative of
  -- LOCAL_MODULE, to specify the targets to build when executing ndk-build.
  filter("platforms:Mac-*")
    targetname("xenia")
  filter("platforms:not Mac-*")
    targetname("xenia_canary")

  filter("architecture:x86_64")
    links({
      "xenia-cpu-backend-x64",
    })
  filter("architecture:arm64")
    links({
      "xenia-cpu-backend-a64",
    })

  -- TODO(Triang3l): The emulator itself on Android.
  filter("platforms:not Android-*")
    files({
      "xenia_main.cc",
    })

  filter("platforms:Windows")
    files({
      "main_resources.rc",
    })

  filter({"architecture:x86_64", "files:../base/main_init_"..platform_suffix..".cc"})
    vectorextensions("SSE2")  -- Disable AVX for main_init_win.cc so our AVX check doesn't use AVX instructions.

  filter("platforms:not Android-*")
    links({
      "xenia-app-discord",
      "xenia-apu-sdl",
      -- TODO(Triang3l): CPU debugger on Android.
      "xenia-debug-ui",
      "xenia-helper-sdl",
      "xenia-hid-sdl",
    })
  filter("architecture:arm64")
    removelinks({
      "xenia-debug-ui",
      "xenia-app-discord",
      "discord-rpc",
    })
    -- TODO(wmarti): Port debug UI to ARM64 host contexts (rip/eflags/xmm).

  filter("platforms:Linux")
    links({
      "X11",
      "xcb",
      "X11-xcb",
      "SDL2",
    })

  filter("platforms:Windows")
    links({
      "xenia-apu-xaudio2",
      "xenia-gpu-d3d12",
      "xenia-hid-winkey",
      "xenia-hid-xinput",
      "xenia-ui-d3d12",
    })

  filter("platforms:Mac-*")
    removelinks({
      "xenia-app-discord",
      "discord-rpc",
      "xenia-gpu-vulkan",
      "xenia-ui-vulkan",
      "xenia-gpu-vulkan-trace-viewer",
      "xenia-ui-window-vulkan-demo",
    })
  filter("platforms:Mac-*")
    local function select_libdir(candidates, filename)
      for _, dir in ipairs(candidates) do
        if os.isfile(path.join(dir, filename)) then
          return dir
        end
      end
      return nil
    end
    local target_arch = os.targetarch() or ""
    local option_arch = _OPTIONS and _OPTIONS["arch"] or ""
    local want_x86 =
        _OPTIONS and _OPTIONS["mac-x86_64"] or
        target_arch == "x86_64" or target_arch == "x64" or
        option_arch == "x86_64" or option_arch == "x64"
    local want_arm = target_arch == "arm64" or option_arch == "arm64"
    -- Link Metal UI/GPU on macOS so the Metal backend can be selected.
    local app_bundle = "${TARGET_BUILD_DIR}/${FULL_PRODUCT_NAME}"
    local app_contents = "${TARGET_BUILD_DIR}/${FULL_PRODUCT_NAME}/Contents"
    local app_frameworks = app_contents .. "/Frameworks"
    local app_executable = app_contents .. "/MacOS/xenia"
    if os.istarget("macosx") then
      if want_x86 then
        dxilconv_libdir = dxilconv_libdir_x86_64
      elseif want_arm then
        dxilconv_libdir = dxilconv_libdir_arm64
      end
      if want_x86 and not os.isdir(dxilconv_libdir_x86_64) then
        error("dxilconv x86_64 libdir not found; build dxilconv for x86_64.")
      end
      if want_arm and not os.isdir(dxilconv_libdir_arm64) then
        error("dxilconv arm64 libdir not found; build dxilconv for arm64.")
      end
      local lz4_candidates = {
        "/opt/homebrew/opt/lz4/lib",
        "/opt/homebrew/lib",
        "/usr/local/opt/lz4/lib",
        "/usr/local/lib",
      }
      local sdl2_candidates = {
        "/opt/homebrew/opt/sdl2/lib",
        "/opt/homebrew/lib",
        "/usr/local/opt/sdl2/lib",
        "/usr/local/lib",
      }
      if want_x86 then
        lz4_candidates = {
          "/usr/local/opt/lz4/lib",
          "/usr/local/lib",
          "/opt/homebrew/opt/lz4/lib",
          "/opt/homebrew/lib",
        }
        sdl2_candidates = {
          "/usr/local/opt/sdl2/lib",
          "/usr/local/lib",
          "/opt/homebrew/opt/sdl2/lib",
          "/opt/homebrew/lib",
        }
      end
      lz4_libdir = select_libdir(lz4_candidates, "liblz4.1.dylib") or lz4_libdir
      sdl2_libdir = select_libdir(sdl2_candidates, "libSDL2-2.0.0.dylib")
          or sdl2_libdir
      if not os.isfile(path.join(lz4_libdir, "liblz4.1.dylib")) then
        error("LZ4 dylib not found. Install with `brew install lz4`.")
      end
      if not os.isfile(path.join(sdl2_libdir, "libSDL2-2.0.0.dylib")) then
        error("SDL2 dylib not found. Install with `brew install sdl2`.")
      end
    end
    links({
      "xenia-gpu-metal",
      "xenia-ui-metal",
      "metal-cpp",
      "metalirconverter",
      "dxilconv",
      "LLVMDxcSupport",
      "SDL2",
      "Cocoa.framework",
      "CoreFoundation.framework",
      "Metal.framework",
      "MetalFX.framework",
      "MetalKit.framework",
      "QuartzCore.framework",
    })
    libdirs({
      metal_converter_libdir,
      dxilconv_libdir,
      "/usr/local/lib",
    })
    linkoptions({
      path.join(dxilconv_libdir, "libdxilconv.dylib"),
    })
    runpathdirs({
      "@executable_path/../Frameworks",
      metal_converter_libdir,
      dxilconv_libdir,
      "/usr/local/lib",
    })
    linkoptions({
      "-Wl,-rpath,@executable_path/../Frameworks",
      "-Wl,-rpath,@loader_path/../Frameworks",
    })
    postbuildcommands({
      'mkdir -p "' .. app_frameworks .. '"',
      'cp -f "' ..
          path.join(metal_converter_libdir, "libmetalirconverter.dylib") ..
          '" "' .. app_frameworks .. '/"',
      'cp -f "' ..
          path.join(dxilconv_libdir, "libdxilconv.dylib") ..
          '" "' .. app_frameworks .. '/"',
      'cp -f "' .. path.join(lz4_libdir, "liblz4.1.dylib") .. '" "' ..
          app_frameworks .. '/"',
      'cp -f "' .. path.join(sdl2_libdir, "libSDL2-2.0.0.dylib") .. '" "' ..
          app_frameworks .. '/"',
      'install_name_tool -id @rpath/liblz4.1.dylib "' .. app_frameworks ..
          '/liblz4.1.dylib"',
      'install_name_tool -id @rpath/libSDL2-2.0.0.dylib "' .. app_frameworks ..
          '/libSDL2-2.0.0.dylib"',
      'if otool -L "' .. app_executable .. '" | grep -q ' ..
          '"/opt/homebrew/opt/lz4/lib/liblz4.1.dylib"; then ' ..
          'install_name_tool -change ' ..
          '"/opt/homebrew/opt/lz4/lib/liblz4.1.dylib" ' ..
          '"@rpath/liblz4.1.dylib" "' .. app_executable .. '"; fi',
      'if otool -L "' .. app_executable .. '" | grep -q ' ..
          '"/usr/local/opt/lz4/lib/liblz4.1.dylib"; then ' ..
          'install_name_tool -change ' ..
          '"/usr/local/opt/lz4/lib/liblz4.1.dylib" ' ..
          '"@rpath/liblz4.1.dylib" "' .. app_executable .. '"; fi',
      'if otool -L "' .. app_executable .. '" | grep -q ' ..
          '"/opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib"; then ' ..
          'install_name_tool -change ' ..
          '"/opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib" ' ..
          '"@rpath/libSDL2-2.0.0.dylib" "' .. app_executable .. '"; fi',
      'if otool -L "' .. app_executable .. '" | grep -q ' ..
          '"/usr/local/opt/sdl2/lib/libSDL2-2.0.0.dylib"; then ' ..
          'install_name_tool -change ' ..
          '"/usr/local/opt/sdl2/lib/libSDL2-2.0.0.dylib" ' ..
          '"@rpath/libSDL2-2.0.0.dylib" "' .. app_executable .. '"; fi',
      'codesign --force --sign - "' .. app_frameworks ..
          '/libmetalirconverter.dylib"',
      'codesign --force --sign - "' .. app_frameworks ..
          '/libdxilconv.dylib"',
      'codesign --force --sign - "' .. app_frameworks ..
          '/liblz4.1.dylib"',
      'codesign --force --sign - "' .. app_frameworks ..
          '/libSDL2-2.0.0.dylib"',
      'codesign --force --deep --sign - --entitlements "' ..
          entitlements_path .. '" "' .. app_bundle .. '"',
    })
  filter({"platforms:Mac-*", "architecture:x86_64"})
    removelinks({
      "LLVMDxcSupport",
    })
    linkoptions({
      "-Wl,-pagezero_size,0x1000",
      path.getabsolute(path.join(dxilconv_libdir_x86_64,
                                 "libLLVMDxcSupport.a")),
    })
  filter("platforms:Mac-*")
    files({
      info_plist_path,
      entitlements_path,
      icon_path,
    })
    filter({"platforms:Mac-*", "files:**.icns"})
      buildaction("Resources")
    filter("platforms:Mac-*")
    buildoptions({
      "-DINFOPLIST_FILE=" .. info_plist_path,
    })
    xcodebuildsettings({
      ["INFOPLIST_FILE"] = info_plist_path,
      ["MACOSX_DEPLOYMENT_TARGET"] = "15.0",
      ["PRODUCT_NAME"] = "Xenia-Canary",
      ["EXECUTABLE_NAME"] = "xenia",
      ["PRODUCT_BUNDLE_IDENTIFIER"] = "com.xenia.xenia",
      ["CODE_SIGN_STYLE"] = "Automatic",
      ["CODE_SIGN_ENTITLEMENTS"] = entitlements_path,
    })

  if enableMiscSubprojects then
    filter({"platforms:Windows", SINGLE_LIBRARY_FILTER})
      links({
        "xenia-gpu-d3d12-trace-viewer",
        "xenia-ui-window-d3d12-demo",
      })
  end

  filter("platforms:Windows")
    -- Only create the .user file if it doesn't already exist.
    local user_file = project_root.."/build/xenia-app.vcxproj.user"
    if not os.isfile(user_file) then
      debugdir(project_root)
    end
