-- Shader custom build rules for premake5.
-- Defines shader_rules_spirv() and shader_rules_dxbc() which auto-discover
-- shader sources and generate per-file custom build rules.

local function shader_rules(shader_dir, backend, source_globs, include_exts, script_name, bytecode_subdir)
  local abs_script = path.getabsolute(project_root .. "/tools/build/" .. script_name)

  -- Discover source files (compilable shaders).
  local sources = {}
  for _, glob in ipairs(source_globs) do
    local matches = os.matchfiles(shader_dir .. "/" .. glob)
    for _, f in ipairs(matches) do
      table.insert(sources, f)
    end
  end

  -- Discover include/header files for buildinputs.
  local includes = {}
  for _, ext in ipairs(include_exts) do
    local matches = os.matchfiles(shader_dir .. "/*." .. ext)
    for _, f in ipairs(matches) do
      table.insert(includes, f)
    end
  end

  -- Absolute paths for includes (used as buildinputs).
  local abs_includes = {}
  for _, inc in ipairs(includes) do
    table.insert(abs_includes, path.getabsolute(inc))
  end

  -- Add all sources and includes to the project for IDE visibility.
  if #sources > 0 then
    files(sources)
  end
  if #includes > 0 then
    files(includes)
  end

  -- Generate per-file custom build rules for each source.
  local valid_stages = { vs=true, hs=true, ds=true, gs=true, ps=true, cs=true }
  for _, src in ipairs(sources) do
    local src_name = path.getname(src)
    -- Identifier: strip extension, replace dots with underscores.
    -- e.g. "apply_gamma_pwl.cs.xesl" -> "apply_gamma_pwl_cs"
    local identifier = path.getbasename(src_name):gsub("%.", "_")

    -- Only process files that have a valid 2-char stage suffix.
    local stage = identifier:sub(-2)
    if valid_stages[stage] then
      local abs_output = path.getabsolute(shader_dir .. "/bytecode/" .. bytecode_subdir .. "/" .. identifier .. ".h")

      filter("files:" .. src)
        buildmessage("Compiling " .. backend .. " shader: " .. src_name)
        buildcommands({
          "python \"" .. abs_script .. "\" \"%{file.abspath}\" \"" .. abs_output .. "\""
        })
        buildoutputs({ abs_output })
        buildinputs(abs_includes)
    end
  end

  filter({})
end

function shader_rules_spirv(shader_dir)
  shader_rules(
    shader_dir,
    "SPIR-V",
    { "*.xesl", "*.glsl" },     -- source globs
    { "xesli", "glsli" },       -- include extensions
    "compile_shader_spirv.py",
    "vulkan_spirv"
  )
end

function shader_rules_dxbc(shader_dir)
  shader_rules(
    shader_dir,
    "DXBC",
    { "*.xesl", "*.hlsl" },     -- source globs
    { "xesli", "hlsli" },       -- include extensions
    "compile_shader_dxbc.py",
    "d3d12_5_1"
  )
end
