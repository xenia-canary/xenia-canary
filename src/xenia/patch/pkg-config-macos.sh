#!/bin/bash
TARGET="tools/build/scripts/pkg_config.lua"

python3 -c '
import os
import sys

target = sys.argv[1]
if not os.path.exists(target):
    exit(0)

with open(target, "r") as f:
    orig_text = f.read()

# idempotency check
if "os.istarget(\"macosx\")" in orig_text and "string.startswith" in orig_text:
    print("pkg_config.lua already correctly patched.")
    exit(0)

# define our new functions by targetting headers
new_cflags = """function pkg_config.cflags(lib)
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
  if not os.istarget(\"linux\") then
    return
  end
  buildoptions({
    pkg_config_call(lib, \"cflags\"),
  })
end"""

new_lflags = """function pkg_config.lflags(lib)
  if os.istarget(\"macosx\") then
    if string.startswith(lib, \"gtk\") then
      links({ \"gtk-3\", \"gdk-3\" })
    elseif string.startswith(lib, \"glib\") then
      links({ \"glib-2.0\", \"gobject-2.0\", \"gio-2.0\" })
    elseif string.startswith(lib, \"pango\") then
      links({ \"pango-1.0\", \"pangocairo-1.0\" })
    elseif string.startswith(lib, \"cairo\") then
      links({ \"cairo\" })
    end
    return
  end
  if not os.istarget(\"linux\") then
    return
  end
  linkoptions({
    pkg_config_call(lib, \"libs-only-L\"),
    pkg_config_call(lib, \"libs-only-other\"),
  })
  local output = pkg_config_call(lib, \"libs-only-l\")
  for k, flag in next, string.explode(output, \" \") do
    -- remove \"-l\"
    if flag ~= \"\" then
        links(string.sub(flag, 3))
    end
  end
end"""

# handle potential corrupted state
lines = orig_text.splitlines()
output_lines = []
skip_until_end = False

for line in lines:
    if line.startswith("function pkg_config.cflags(lib)"):
        output_lines.append(new_cflags)
        skip_until_end = True
        continue
    if line.startswith("function pkg_config.lflags(lib)"):
        output_lines.append(new_lflags)
        skip_until_end = True
        continue
    
    if skip_until_end:
        if line.strip() == "end":
            # We found the end of the function. Stop skipping.
            skip_until_end = False
        continue
    
    output_lines.append(line)

final_text = "\n".join(output_lines) + "\n"
with open(target, "w") as f:
    f.write(final_text)
' "$TARGET"
