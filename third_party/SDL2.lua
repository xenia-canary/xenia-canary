--
-- On Linux we build against the system version (libsdl2-dev for building),
-- since SDL2 is our robust API there like DirectX is on Windows.
--

local sdl2_sys_includedirs = {}
local sdl2_sys_libdirs = {}
local third_party_path = os.getcwd()
local sdl2_config = os.getenv("SDL2_CONFIG") or "sdl2-config"

if os.istarget("windows") then
  -- build ourselves
  include("SDL2-static.lua")
else
  -- use system libraries
  if os.istarget("macosx") then
    local target_arch = os.targetarch() or ""
    local option_arch = _OPTIONS and _OPTIONS["arch"] or ""
    local want_x86 =
        _OPTIONS and _OPTIONS["mac-x86_64"] or
        target_arch == "x86_64" or target_arch == "x64" or
        option_arch == "x86_64" or option_arch == "x64"
    local want_arm = target_arch == "arm64" or option_arch == "arm64"
    if want_x86 and os.isfile("/usr/local/bin/sdl2-config") then
      sdl2_config = "/usr/local/bin/sdl2-config"
    elseif want_arm and os.isfile("/opt/homebrew/bin/sdl2-config") then
      sdl2_config = "/opt/homebrew/bin/sdl2-config"
    end
  end
  local result, code, what = os.outputof(sdl2_config .. " --cflags")
  if result then
    for inc in string.gmatch(result, "-I([%S]+)") do
      table.insert(sdl2_sys_includedirs, inc)
    end
  else
    error("Failed to run 'sdl2-config'. Are libsdl2 development files installed?")
  end
  local libs, libs_code, libs_what = os.outputof(sdl2_config .. " --libs")
  if libs then
    for libdir in string.gmatch(libs, "-L([%S]+)") do
      table.insert(sdl2_sys_libdirs, libdir)
    end
  else
    error("Failed to run 'sdl2-config --libs'. Are libsdl2 development files installed?")
  end
end

if os.istarget("macosx") then
  local target_arch = os.targetarch() or ""
  local option_arch = _OPTIONS and _OPTIONS["arch"] or ""
  local want_x86 =
      target_arch == "x86_64" or target_arch == "x64" or
      option_arch == "x86_64" or option_arch == "x64"
  local want_arm = target_arch == "arm64" or option_arch == "arm64"
  if want_x86 and os.isdir("/usr/local/opt/sdl2/lib") then
    table.insert(sdl2_sys_libdirs, "/usr/local/opt/sdl2/lib")
  elseif want_arm and os.isdir("/opt/homebrew/opt/sdl2/lib") then
    table.insert(sdl2_sys_libdirs, "/opt/homebrew/opt/sdl2/lib")
  end
end

--
-- Call this function in project scope to include the SDL2 headers.
--
function sdl2_include()
  filter("platforms:Windows")
    includedirs({
      path.getrelative(".", third_party_path) .. "/SDL2/include",
    })
  filter("platforms:Linux or platforms:Mac-*")
    includedirs(sdl2_sys_includedirs)
    libdirs(sdl2_sys_libdirs)
  filter({})
end
