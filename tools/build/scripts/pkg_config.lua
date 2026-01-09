-- Helper methods to use the system pkg-config utility

pkg_config = {}

local function pkg_config_call(lib, what)
  local result, code = os.outputof("pkg-config --"..what.." "..lib)
  if result then
    return result
  else
    error("Failed to run 'pkg-config' for library '"..lib.."'. Are the development files installed?")
  end
end

function pkg_config.cflags(lib)
  if os.istarget("macosx") and (string.startswith(lib, "gtk") or string.startswith(lib, "gdk") or string.startswith(lib, "glib") or string.startswith(lib, "pango") or string.startswith(lib, "cairo") or string.startswith(lib, "atk")) then
    os.execute("sh src/xenia/patch/setup_xcode_gtk.sh")
    includedirs({
      "/tmp/xenia_gtk_patched_permanent/gtk-3.0",
      "/tmp/xenia_gtk_patched_permanent/glib-2.0",
      "/tmp/xenia_gtk_patched_permanent/glib-2.0/include",
      "/tmp/xenia_gtk_patched_permanent/pango-1.0",
      "/tmp/xenia_gtk_patched_permanent/cairo",
      "/tmp/xenia_gtk_patched_permanent/atk-1.0",
      "/tmp/xenia_gtk_patched_permanent/gdk-pixbuf-2.0",
      "/tmp/xenia_gtk_patched_permanent/harfbuzz",
    })
    return
  end
  if not os.istarget("linux") then
    return
  end
  buildoptions({
    pkg_config_call(lib, "cflags"),
  })
end

function pkg_config.lflags(lib)
  if os.istarget("macosx") then
    if string.startswith(lib, "gtk") then
      links({ "gtk-3", "gdk-3" })
    elseif string.startswith(lib, "glib") then
      links({ "glib-2.0", "gobject-2.0", "gio-2.0" })
    elseif string.startswith(lib, "pango") then
      links({ "pango-1.0", "pangocairo-1.0" })
    elseif string.startswith(lib, "cairo") then
      links({ "cairo" })
    end
    return
  end
  if not os.istarget("linux") then
    return
  end
  linkoptions({
    pkg_config_call(lib, "libs-only-L"),
    pkg_config_call(lib, "libs-only-other"),
  })
  -- We can't just drop the stdout of the `--libs` command in
  -- linkoptions because library order matters
  local output = pkg_config_call(lib, "libs-only-l")
  for k, flag in next, string.explode(output, " ") do
    -- remove "-l"
    if flag ~= "" then
        links(string.sub(flag, 3))
    end
  end
end

function pkg_config.all(lib)
    pkg_config.cflags(lib)
    pkg_config.lflags(lib)
end
