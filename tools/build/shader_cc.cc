// Build-time shader compiler for Xenia's built-in shaders.
//
// Usage: xenia-shader-cc [--msl | --dxbc] [--depfile <path>]
//                       <input> <output.h>
//
// Default: GLSL/XeSL -> SPIR-V, linked in-process via glslang and
// SPIRV-Tools, emitted as `const uint32_t <id>[]`. Stage parsed from
// filename stem (foo.cs.glsl -> compute); XeSL sources compiled with
// SHADING_LANGUAGE_GLSL_XE=1 and includes resolved relative to the
// input's directory.
//
// --msl (Apple only): .xesl -> .metallib via `xcrun metal -x metal -D
// SHADING_LANGUAGE_MSL_XE=1`, emitted as `const uint8_t <id>_metallib[]`.
//
// --dxbc: HLSL/XeSL -> DXBC via fxc.exe with /Fh writing the header
// directly (SHADING_LANGUAGE_HLSL_XE=1). FXC_PATH env overrides the
// auto-detected Windows Kits path; wine is used on non-Windows hosts.
//
// --depfile writes a Make-style dependency file listing every source the
// compile transitively read, so CMake/Ninja can trigger rebuilds when a
// cross-directory include (e.g. ui/shaders/xesl.xesli) changes.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "DirStackFileIncluder.h"
#include "SPIRV/GlslangToSpv.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

struct StageInfo {
  const char* key;
  EShLanguage language;
};

constexpr StageInfo kStages[] = {
    {"vs", EShLangVertex},         {"hs", EShLangTessControl},
    {"ds", EShLangTessEvaluation}, {"gs", EShLangGeometry},
    {"ps", EShLangFragment},       {"cs", EShLangCompute},
};

constexpr const char* kXeslWrapperPrefix =
    "#version 460\n"
    "#extension GL_EXT_control_flow_attributes : require\n"
    "#extension GL_EXT_samplerless_texture_functions : require\n"
    "#extension GL_GOOGLE_include_directive : require\n";

bool ParseStage(const std::string& filename, EShLanguage* out,
                std::string* stage_key_out) {
  // Strip extension, then take last 2 chars after the final dot.
  auto dot = filename.rfind('.');
  if (dot == std::string::npos || dot < 3 || filename[dot - 3] != '.') {
    return false;
  }
  std::string key = filename.substr(dot - 2, 2);
  for (const auto& stage : kStages) {
    if (key == stage.key) {
      *out = stage.language;
      *stage_key_out = key;
      return true;
    }
  }
  return false;
}

std::string IdentifierFromFilename(const std::string& filename) {
  // foo.bar.cs.glsl -> foo_bar_cs
  auto last_dot = filename.rfind('.');
  std::string stem = filename.substr(0, last_dot);
  std::replace(stem.begin(), stem.end(), '.', '_');
  return stem;
}

bool ReadFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "failed to open %s\n", path.string().c_str());
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  *out = buf.str();
  return true;
}

// Escape spaces in Make-style depfile paths (Ninja's parser requires this).
std::string EscapeDepPath(const std::string& p) {
  std::string out;
  out.reserve(p.size());
  for (char c : p) {
    if (c == ' ' || c == '\t') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// Writes a Make-style depfile: "<target>: <dep1> <dep2> ..." with each dep on
// its own line (backslash-continued) so Ninja's DEPFILE parser accepts it.
bool WriteDepfile(const std::filesystem::path& depfile_path,
                  const std::filesystem::path& target,
                  const std::vector<std::string>& deps) {
  std::error_code ec;
  std::filesystem::create_directories(depfile_path.parent_path(), ec);
  std::ofstream out(depfile_path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open depfile %s\n",
                 depfile_path.string().c_str());
    return false;
  }
  out << EscapeDepPath(target.string()) << ":";
  for (const auto& dep : deps) {
    out << " \\\n  " << EscapeDepPath(dep);
  }
  out << "\n";
  return bool(out);
}

std::string DisassembleSpirv(const std::vector<uint32_t>& spirv) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string text;
  const uint32_t options = SPV_BINARY_TO_TEXT_OPTION_INDENT |
                           SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES;
  if (!tools.Disassemble(spirv, &text, options)) {
    text.clear();
  }
  return text;
}

bool WriteSpirvHeader(const std::filesystem::path& path,
                      const std::string& identifier,
                      const std::vector<uint32_t>& spirv) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open output %s\n", path.string().c_str());
    return false;
  }
  out << "// Generated by xenia-shader-cc. Do not edit.\n#if 0\n";
  std::string disasm = DisassembleSpirv(spirv);
  if (!disasm.empty()) {
    out << disasm;
    if (disasm.back() != '\n') {
      out << '\n';
    }
  }
  out << "#endif\n\n";
  out << "const uint32_t " << identifier << "[] = {";
  for (size_t i = 0; i < spirv.size(); ++i) {
    out << (i % 6 == 0 ? "\n    " : " ");
    char word[16];
    std::snprintf(word, sizeof(word), "0x%08X,", spirv[i]);
    out << word;
  }
  out << "\n};\n";
  return bool(out);
}

#if defined(_WIN32)
// Quote an argument for the MSVCRT command-line parser, which is what fxc.exe
// (and most MSVC-built tools) use to re-split GetCommandLineW() back into
// argv. _spawnvp joins argv with literal spaces and no quoting, so any arg
// containing whitespace -- e.g. "C:\Program Files (x86)\..." -- must be
// wrapped here or it gets re-split into multiple tokens on the receiving end.
std::string QuoteForSpawn(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
    return arg;
  }
  std::string out;
  out.push_back('"');
  for (size_t i = 0; i < arg.size(); ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++backslashes;
      ++i;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\');
      break;
    } else if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back('"');
  return out;
}
#endif

int RunCommand(const std::vector<std::string>& args,
               bool silent_stdout = false) {
  if (args.empty()) return -1;
#if defined(_WIN32)
  std::vector<std::string> quoted;
  quoted.reserve(args.size());
  for (const auto& a : args) quoted.push_back(QuoteForSpawn(a));
  std::vector<char*> argv;
  argv.reserve(quoted.size() + 1);
  for (auto& a : quoted) argv.push_back(a.data());
  argv.push_back(nullptr);
  int saved_stdout = -1;
  if (silent_stdout) {
    std::fflush(stdout);
    saved_stdout = _dup(_fileno(stdout));
    int devnull = _open("nul", _O_WRONLY);
    if (devnull >= 0) {
      _dup2(devnull, _fileno(stdout));
      _close(devnull);
    }
  }
  intptr_t rc = _spawnvp(_P_WAIT, args[0].c_str(), argv.data());
  if (saved_stdout >= 0) {
    std::fflush(stdout);
    _dup2(saved_stdout, _fileno(stdout));
    _close(saved_stdout);
  }
  if (rc < 0) {
    std::fprintf(stderr, "_spawnvp(%s) failed: %s\n", args[0].c_str(),
                 std::strerror(errno));
    return -1;
  }
  return static_cast<int>(rc);
#else
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
  if (rc != 0) {
    std::fprintf(stderr, "posix_spawnp(%s) failed: %s\n", argv[0],
                 std::strerror(rc));
    return -1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    std::fprintf(stderr, "waitpid failed for %s\n", argv[0]);
    return -1;
  }
  if (!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
#endif
}

#ifdef XE_SHADER_CC_METAL

bool ReadBinaryFile(const std::filesystem::path& path,
                    std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  auto size = in.tellg();
  if (size < 0) return false;
  in.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(size));
  in.read(reinterpret_cast<char*>(out->data()), size);
  return bool(in);
}

bool WriteMetallibHeader(const std::filesystem::path& path,
                         const std::string& identifier,
                         const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open output %s\n", path.string().c_str());
    return false;
  }
  out << "// Generated by xenia-shader-cc. Do not edit.\n";
  out << "const uint8_t " << identifier << "_metallib[] = {";
  for (size_t i = 0; i < bytes.size(); ++i) {
    out << (i % 16 == 0 ? "\n    " : " ");
    char byte[8];
    std::snprintf(byte, sizeof(byte), "0x%02X,", bytes[i]);
    out << byte;
  }
  out << "\n};\n";
  return bool(out);
}

#endif  // XE_SHADER_CC_METAL

// Locates fxc.exe (or dxc.exe) for --dxbc mode: FXC_PATH env var takes
// priority; otherwise on Windows we walk Windows Kits\10\bin\*\x64\fxc.exe
// and pick the highest-versioned one. Returns empty on miss.
std::string FindFxc() {
  const char* env = std::getenv("FXC_PATH");
  if (env && *env) return env;
#ifdef _WIN32
  const char* pf86 = std::getenv("ProgramFiles(x86)");
  if (!pf86) return {};
  std::filesystem::path kits =
      std::filesystem::path(pf86) / "Windows Kits" / "10" / "bin";
  std::error_code ec;
  std::string best;
  for (auto& entry : std::filesystem::directory_iterator(kits, ec)) {
    if (!entry.is_directory(ec)) continue;
    std::filesystem::path candidate = entry.path() / "x64" / "fxc.exe";
    if (std::filesystem::exists(candidate, ec) && candidate.string() > best) {
      best = candidate.string();
    }
  }
  return best;
#else
  return {};
#endif
}

}  // namespace

int main(int argc, char** argv) {
  bool msl_mode = false;
  bool dxbc_mode = false;
  std::string depfile_path;
  int arg_idx = 1;
  while (arg_idx < argc && argv[arg_idx][0] == '-') {
    if (std::strcmp(argv[arg_idx], "--msl") == 0) {
#ifndef XE_SHADER_CC_METAL
      std::fprintf(stderr,
                   "--msl not available (built without XE_SHADER_CC_METAL)\n");
      return 1;
#endif
      msl_mode = true;
      ++arg_idx;
    } else if (std::strcmp(argv[arg_idx], "--dxbc") == 0) {
      dxbc_mode = true;
      ++arg_idx;
    } else if (std::strcmp(argv[arg_idx], "--depfile") == 0) {
      if (arg_idx + 1 >= argc) {
        std::fprintf(stderr, "--depfile requires a path\n");
        return 1;
      }
      depfile_path = argv[arg_idx + 1];
      arg_idx += 2;
    } else {
      std::fprintf(stderr, "unknown flag: %s\n", argv[arg_idx]);
      return 1;
    }
  }
  if (argc - arg_idx != 2) {
    std::fprintf(
        stderr,
        "Usage: %s [--msl | --dxbc] [--depfile <path>] <input> <output>\n",
        argv[0]);
    return 1;
  }
  if (msl_mode && dxbc_mode) {
    std::fprintf(stderr, "--msl and --dxbc are mutually exclusive\n");
    return 1;
  }

  std::filesystem::path input_path = argv[arg_idx];
  std::filesystem::path output_path = argv[arg_idx + 1];
  std::string input_filename = input_path.filename().string();
  std::string identifier = IdentifierFromFilename(input_filename);

  if (msl_mode) {
#ifdef XE_SHADER_CC_METAL
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto tag = identifier + "_" + std::to_string(::getpid());
    auto air_path = tmp_dir / (tag + ".air");
    auto lib_path = tmp_dir / (tag + ".metallib");

    auto cleanup = [&] {
      std::error_code ec;
      std::filesystem::remove(air_path, ec);
      std::filesystem::remove(lib_path, ec);
    };

    std::vector<std::string> metal_cmd = {
        "xcrun",
        "-sdk",
        "macosx",
        "metal",
        "-x",
        "metal",
        "-std=macos-metal2.3",
        "-mmacosx-version-min=" XE_METAL_MIN_OS,
        "-D",
        "SHADING_LANGUAGE_MSL_XE=1",
        "-w",
    };
    std::string input_dir = input_path.parent_path().string();
    if (!input_dir.empty()) {
      metal_cmd.push_back("-I");
      metal_cmd.push_back(input_dir);
    }
    if (!depfile_path.empty()) {
      // xcrun metal is clang-based and supports standard depfile flags.
      // -MT retargets the depfile at our final .h (not the intermediate .air)
      // so CMake/Ninja link the dependency graph to the right output.
      metal_cmd.push_back("-MD");
      metal_cmd.push_back("-MT");
      metal_cmd.push_back(output_path.string());
      metal_cmd.push_back("-MF");
      metal_cmd.push_back(depfile_path);
    }
    metal_cmd.push_back("-c");
    metal_cmd.push_back(input_path.string());
    metal_cmd.push_back("-o");
    metal_cmd.push_back(air_path.string());
    if (RunCommand(metal_cmd) != 0) {
      std::fprintf(stderr, "metal failed for %s\n",
                   input_path.string().c_str());
      cleanup();
      return 1;
    }
    if (RunCommand({"xcrun", "-sdk", "macosx", "metallib", air_path.string(),
                    "-o", lib_path.string()}) != 0) {
      std::fprintf(stderr, "metallib failed for %s\n",
                   input_path.string().c_str());
      cleanup();
      return 1;
    }

    std::vector<uint8_t> bytes;
    if (!ReadBinaryFile(lib_path, &bytes)) {
      std::fprintf(stderr, "failed to read %s\n", lib_path.string().c_str());
      cleanup();
      return 1;
    }
    cleanup();

    if (!WriteMetallibHeader(output_path, identifier, bytes)) {
      return 1;
    }
    return 0;
#else
    std::fprintf(stderr,
                 "--msl not available (built without XE_SHADER_CC_METAL)\n");
    return 1;
#endif
  }

  EShLanguage stage;
  std::string stage_key;
  if (!ParseStage(input_filename, &stage, &stage_key)) {
    std::fprintf(stderr,
                 "cannot determine shader stage from filename: %s\n",
                 input_filename.c_str());
    return 1;
  }

  if (dxbc_mode) {
    std::string fxc = FindFxc();
    if (fxc.empty()) {
      std::fprintf(stderr,
                   "ERROR: could not find fxc! Set FXC_PATH or install "
                   "Windows SDK.\n");
      return 1;
    }
    std::string fxc_name =
        std::filesystem::path(fxc).filename().string();
    std::transform(fxc_name.begin(), fxc_name.end(), fxc_name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const bool is_dxc = fxc_name.find("dxc") != std::string::npos;

    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);

    std::vector<std::string> cmd;
#ifndef _WIN32
    cmd.push_back("wine");
#endif
    cmd.push_back(fxc);

    std::string src_dir = input_path.parent_path().string();
    std::string target = stage_key + (is_dxc ? "_6_0" : "_5_1");
    if (is_dxc) {
      // DXC supports SM 6.0+ only.
      cmd.insert(cmd.end(), {
                                "-T", target,
                                "-HV", "2017",
                                "-D", "SHADING_LANGUAGE_HLSL_XE=1",
                                "-I", src_dir,
                                "-Fh", output_path.string(),
                                "-Vn", identifier,
                                "-nologo",
                                input_path.string(),
                            });
    } else {
      cmd.insert(cmd.end(), {
                                "/D", "SHADING_LANGUAGE_HLSL_XE=1",
                                "/I", src_dir,
                                "/Fh", output_path.string(),
                                "/T", target,
                                "/Vn", identifier,
                                "/O3",
                                "/Qstrip_reflect",
                                "/Qstrip_debug",
                                "/Qstrip_priv",
                                "/Gfp",
                                "/nologo",
                                input_path.string(),
                            });
    }
    if (RunCommand(cmd, /*silent_stdout=*/true) != 0) {
      std::fprintf(stderr, "fxc failed for %s\n",
                   input_path.string().c_str());
      return 1;
    }

    // Depfile: FXC has no native depfile emission, so re-invoke with /P
    // (preprocess-only) to a temp file, then scrape `#line N "path"` markers.
    // DXC supports /P the same way.
    if (!depfile_path.empty()) {
      auto tmp_dir = std::filesystem::temp_directory_path();
      auto tag = identifier + "_" +
                 std::to_string(
#ifdef _WIN32
                     static_cast<int>(_getpid())
#else
                     static_cast<int>(::getpid())
#endif
                 );
      auto pp_path = tmp_dir / (tag + ".hlsl");

      std::vector<std::string> pp_cmd;
#ifndef _WIN32
      pp_cmd.push_back("wine");
#endif
      pp_cmd.push_back(fxc);
      if (is_dxc) {
        pp_cmd.insert(pp_cmd.end(), {
                                        "-P",
                                        pp_path.string(),
                                        "-D",
                                        "SHADING_LANGUAGE_HLSL_XE=1",
                                        "-I",
                                        src_dir,
                                        "-nologo",
                                        input_path.string(),
                                    });
      } else {
        pp_cmd.insert(pp_cmd.end(), {
                                        "/P",
                                        pp_path.string(),
                                        "/D",
                                        "SHADING_LANGUAGE_HLSL_XE=1",
                                        "/I",
                                        src_dir,
                                        "/nologo",
                                        input_path.string(),
                                    });
      }
      if (RunCommand(pp_cmd, /*silent_stdout=*/true) != 0) {
        std::fprintf(stderr,
                     "fxc /P failed for %s (depfile won't be written)\n",
                     input_path.string().c_str());
        std::filesystem::remove(pp_path, ec);
        return 1;
      }
      std::string pp;
      if (!ReadFile(pp_path, &pp)) {
        std::filesystem::remove(pp_path, ec);
        return 1;
      }
      std::filesystem::remove(pp_path, ec);
      // Extract unique paths from `#line N "path"` markers.
      std::vector<std::string> deps;
      std::set<std::string> seen;
      size_t pos = 0;
      while ((pos = pp.find("#line ", pos)) != std::string::npos) {
        size_t quote = pp.find('"', pos);
        if (quote == std::string::npos) break;
        size_t end = pp.find('"', quote + 1);
        if (end == std::string::npos) break;
        std::string path = pp.substr(quote + 1, end - quote - 1);
        // FXC emits paths with escaped backslashes on Windows — unescape.
        std::string unescaped;
        unescaped.reserve(path.size());
        for (size_t i = 0; i < path.size(); ++i) {
          if (path[i] == '\\' && i + 1 < path.size() && path[i + 1] == '\\') {
            unescaped.push_back('\\');
            ++i;
          } else {
            unescaped.push_back(path[i]);
          }
        }
        if (seen.insert(unescaped).second) deps.push_back(unescaped);
        pos = end + 1;
      }
      if (!WriteDepfile(depfile_path, output_path, deps)) return 1;
    }
    return 0;
  }

  std::string source;
  if (!ReadFile(input_path, &source)) {
    return 1;
  }

  // .xesl files rely on a wrapper prepending #version / extensions and then
  // #include'ing the file itself. glslang needs the include directive to come
  // from an actual source string, not from disk, so we synthesize the wrapper
  // and let the includer fetch the real file.
  const bool is_xesl = input_filename.size() > 5 &&
                       input_filename.compare(input_filename.size() - 5, 5,
                                              ".xesl") == 0;
  std::string wrapper;
  std::string wrapper_name;
  if (is_xesl) {
    wrapper = kXeslWrapperPrefix;
    wrapper += "#include \"";
    wrapper += input_filename;
    wrapper += "\"\n";
  }

  glslang::InitializeProcess();

  glslang::TShader shader(stage);
  const char* source_strings[1];
  const char* source_names[1];
  int source_lengths[1];
  if (is_xesl) {
    source_strings[0] = wrapper.c_str();
    source_lengths[0] = static_cast<int>(wrapper.size());
    wrapper_name = "xesl_wrapper";
    source_names[0] = wrapper_name.c_str();
  } else {
    source_strings[0] = source.c_str();
    source_lengths[0] = static_cast<int>(source.size());
    source_names[0] = input_filename.c_str();
  }
  shader.setStringsWithLengthsAndNames(source_strings, source_lengths,
                                       source_names, 1);
  shader.setPreamble("#define SHADING_LANGUAGE_GLSL_XE 1\n");
  // Match the old `glslangValidator -V` default: Vulkan 1.0 / SPV 1.0, which
  // stays compatible with the Vulkan 1.0 devices the runtime still supports.
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan,
                     100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  DirStackFileIncluder includer;
  std::string input_dir = input_path.parent_path().string();
  if (!input_dir.empty()) {
    includer.pushExternalLocalDirectory(input_dir);
  }

  const int default_version = 460;
  const EShMessages messages = static_cast<EShMessages>(
      EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);

  if (!shader.parse(GetDefaultResources(), default_version, false, messages,
                    includer)) {
    std::fprintf(stderr, "glslang parse failed for %s:\n%s%s",
                 input_path.string().c_str(), shader.getInfoLog(),
                 shader.getInfoDebugLog());
    glslang::FinalizeProcess();
    return 1;
  }

  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    std::fprintf(stderr, "glslang link failed for %s:\n%s%s",
                 input_path.string().c_str(), program.getInfoLog(),
                 program.getInfoDebugLog());
    glslang::FinalizeProcess();
    return 1;
  }

  // Emit raw SPIR-V from glslang; optimization is done explicitly below to
  // match the previous `spirv-opt -O -O --canonicalize-ids` behavior.
  glslang::SpvOptions spv_options;
  spv_options.generateDebugInfo = false;
  spv_options.stripDebugInfo = true;
  spv_options.disableOptimizer = true;
  spv_options.validate = false;

  std::vector<uint32_t> spirv;
  glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &spv_options);

  // Emit the depfile from the includer's set of resolved paths. Add the
  // primary source too (it's a dep of the output .h but not something the
  // includer tracked since we synthesize the XeSL wrapper).
  if (!depfile_path.empty()) {
    std::vector<std::string> deps;
    deps.push_back(input_path.string());
    for (const auto& p : includer.getIncludedFiles()) {
      deps.push_back(p);
    }
    if (!WriteDepfile(depfile_path, output_path, deps)) {
      glslang::FinalizeProcess();
      return 1;
    }
  }

  glslang::FinalizeProcess();

  if (spirv.empty()) {
    std::fprintf(stderr, "GlslangToSpv produced empty output for %s\n",
                 input_path.string().c_str());
    return 1;
  }

  {
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_2);
    optimizer.SetMessageConsumer(
        [](spv_message_level_t, const char*, const spv_position_t&,
           const char* message) { std::fprintf(stderr, "%s\n", message); });
    optimizer.RegisterPerformancePasses();
    optimizer.RegisterPerformancePasses();
    optimizer.RegisterPass(spvtools::CreateCanonicalizeIdsPass());
    std::vector<uint32_t> optimized;
    if (!optimizer.Run(spirv.data(), spirv.size(), &optimized)) {
      std::fprintf(stderr, "spirv-opt failed for %s\n",
                   input_path.string().c_str());
      return 1;
    }
    spirv = std::move(optimized);
  }

  if (!WriteSpirvHeader(output_path, identifier, spirv)) {
    return 1;
  }
  return 0;
}
