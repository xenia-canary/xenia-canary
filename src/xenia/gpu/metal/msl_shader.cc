/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/msl_shader.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "spirv_msl.hpp"
#include "third_party/fmt/include/fmt/format.h"
#include "third_party/xxhash/xxhash.h"

#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/msl_bindings.h"

DEFINE_bool(
    metal_spirvcross_strip_nocontract_optnone, true,
    "When using the Metal SPIRV-Cross path, strip [[clang::optnone]] from "
    "NoContraction float helper wrappers (spvFAdd/spvFSub/spvFMul and matrix "
    "variants). This avoids severe strict-math performance regressions on some "
    "Apple GPUs while keeping NoContraction semantics enabled.",
    "Metal");

DEFINE_bool(metal_shader_disk_cache, true,
            "Cache compiled Metal shader libraries (metallib) to disk when "
            "store_shaders is enabled.",
            "Metal");

namespace xe {
namespace gpu {
namespace metal {

namespace {

constexpr uint32_t kMslSourceCacheMagic = 0x5843534D;  // 'MSCX'
constexpr uint32_t kMslSourceCacheVersion = 1;
constexpr uint32_t kMslSourceCacheMaxBytes = 16 * 1024 * 1024;
// Bump when SPIRV-Cross -> MSL resource remapping or codegen-affecting options
// change so stale on-disk MSL source can't be reused with incompatible runtime
// bindings.
constexpr uint32_t kMslSourceCacheSchemaVersion = 5;

std::mutex g_msl_source_cache_mutex;
std::filesystem::path g_msl_source_cache_directory;

std::filesystem::path GetMslSourceCachePathLocked(uint64_t cache_key) {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llX.mslsrcache",
                static_cast<unsigned long long>(cache_key));
  return g_msl_source_cache_directory / name;
}

bool LoadCachedMslSource(uint64_t cache_key, std::string* source_out) {
  if (!source_out) {
    return false;
  }
  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(g_msl_source_cache_mutex);
    if (g_msl_source_cache_directory.empty()) {
      return false;
    }
    path = GetMslSourceCachePathLocked(cache_key);
  }

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }

  uint32_t magic = 0;
  uint32_t version = 0;
  uint64_t key = 0;
  uint32_t source_size = 0;
  file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  file.read(reinterpret_cast<char*>(&version), sizeof(version));
  file.read(reinterpret_cast<char*>(&key), sizeof(key));
  file.read(reinterpret_cast<char*>(&source_size), sizeof(source_size));
  if (!file || magic != kMslSourceCacheMagic ||
      version != kMslSourceCacheVersion || key != cache_key ||
      source_size > kMslSourceCacheMaxBytes) {
    return false;
  }

  source_out->resize(source_size);
  file.read(source_out->data(), source_size);
  return bool(file);
}

bool StoreCachedMslSource(uint64_t cache_key, std::string_view source) {
  if (source.empty() || source.size() > kMslSourceCacheMaxBytes) {
    return false;
  }

  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(g_msl_source_cache_mutex);
    if (g_msl_source_cache_directory.empty()) {
      return false;
    }
    path = GetMslSourceCachePathLocked(cache_key);
  }
  std::filesystem::path tmp_path = path;
  tmp_path += ".tmp";

  std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    return false;
  }
  uint32_t magic = kMslSourceCacheMagic;
  uint32_t version = kMslSourceCacheVersion;
  uint64_t key = cache_key;
  uint32_t source_size = static_cast<uint32_t>(source.size());
  file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
  file.write(reinterpret_cast<const char*>(&version), sizeof(version));
  file.write(reinterpret_cast<const char*>(&key), sizeof(key));
  file.write(reinterpret_cast<const char*>(&source_size), sizeof(source_size));
  file.write(source.data(), source.size());
  file.close();

  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path, ec);
    return false;
  }
  return true;
}

size_t ReplaceAllInPlace(std::string& text, std::string_view from,
                         std::string_view to) {
  if (from.empty()) {
    return 0;
  }
  size_t replacement_count = 0;
  size_t position = 0;
  while ((position = text.find(from, position)) != std::string::npos) {
    text.replace(position, from.size(), to);
    position += to.size();
    ++replacement_count;
  }
  return replacement_count;
}

size_t StripNoContractionOptnoneWrappers(std::string& msl_source) {
  // Keep this narrowly scoped to the NoContraction helper wrappers that are
  // known to cause significant slowdowns with strict math.
  static constexpr std::pair<std::string_view, std::string_view> kPatterns[] = {
      {"[[clang::optnone]] T spvFAdd(T l, T r)", "T spvFAdd(T l, T r)"},
      {"[[clang::optnone]] T spvFSub(T l, T r)", "T spvFSub(T l, T r)"},
      {"[[clang::optnone]] T spvFMul(T l, T r)", "T spvFMul(T l, T r)"},
      {"[[clang::optnone]] vec<T, Cols> spvFMulVectorMatrix(vec<T, Rows> v, "
       "matrix<T, Cols, Rows> m)",
       "vec<T, Cols> spvFMulVectorMatrix(vec<T, Rows> v, matrix<T, Cols, Rows> "
       "m)"},
      {"[[clang::optnone]] vec<T, Rows> spvFMulMatrixVector(matrix<T, Cols, "
       "Rows> m, vec<T, Cols> v)",
       "vec<T, Rows> spvFMulMatrixVector(matrix<T, Cols, Rows> m, vec<T, Cols> "
       "v)"},
      {"[[clang::optnone]] matrix<T, RCols, LRows> spvFMulMatrixMatrix("
       "matrix<T, LCols, LRows> l, matrix<T, RCols, RRows> r)",
       "matrix<T, RCols, LRows> spvFMulMatrixMatrix(matrix<T, LCols, LRows> l, "
       "matrix<T, RCols, RRows> r)"},
  };
  size_t replacements = 0;
  for (const auto& pattern : kPatterns) {
    replacements +=
        ReplaceAllInPlace(msl_source, pattern.first, pattern.second);
  }
  return replacements;
}

uint64_t GetMslSourceCacheKey(const MslShader::MslTranslation& translation,
                              bool is_ios, uint32_t msl_major,
                              uint32_t msl_minor,
                              bool ios_support_base_vertex_instance,
                              bool emulate_cube_array,
                              bool ios_use_simdgroup_functions,
                              bool use_argument_buffers,
                              uint8_t argument_buffers_tier) {
  uint64_t translated_spirv_hash = 0;
  const auto& translated_spirv = translation.translated_binary();
  if (!translated_spirv.empty()) {
    translated_spirv_hash =
        XXH3_64bits(translated_spirv.data(), translated_spirv.size());
  }

  struct KeyData {
    uint64_t shader_hash;
    uint64_t modification;
    uint64_t translated_spirv_hash;
    uint32_t stage;
    uint8_t is_ios;
    uint8_t msl_major;
    uint8_t msl_minor;
    uint8_t ios_support_base_vertex_instance;
    uint8_t emulate_cube_array;
    uint8_t ios_use_simdgroup_functions;
    uint8_t use_argument_buffers;
    uint8_t argument_buffers_tier;
    uint16_t reserved0;
    uint32_t cache_schema_version;
  } key_data = {};
  key_data.shader_hash = translation.shader().ucode_data_hash();
  key_data.modification = translation.modification();
  key_data.translated_spirv_hash = translated_spirv_hash;
  key_data.stage = static_cast<uint32_t>(translation.shader().type());
  key_data.is_ios = is_ios ? 1 : 0;
  key_data.msl_major = static_cast<uint8_t>(msl_major);
  key_data.msl_minor = static_cast<uint8_t>(msl_minor);
  key_data.ios_support_base_vertex_instance =
      ios_support_base_vertex_instance ? 1 : 0;
  key_data.emulate_cube_array = emulate_cube_array ? 1 : 0;
  key_data.ios_use_simdgroup_functions = ios_use_simdgroup_functions ? 1 : 0;
  key_data.use_argument_buffers = use_argument_buffers ? 1 : 0;
  key_data.argument_buffers_tier = argument_buffers_tier;
  key_data.cache_schema_version = kMslSourceCacheSchemaVersion;
  return XXH3_64bits(&key_data, sizeof(key_data));
}

}  // namespace

void SetMslShaderSourceCacheDirectory(const std::filesystem::path& cache_dir) {
  std::lock_guard<std::mutex> lock(g_msl_source_cache_mutex);
  g_msl_source_cache_directory = cache_dir;
  if (g_msl_source_cache_directory.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(g_msl_source_cache_directory, ec);
  if (ec) {
    XELOGW("MslShader: Failed to create source cache directory {}: {}",
           g_msl_source_cache_directory.string(), ec.message());
    g_msl_source_cache_directory.clear();
  }
}

void ClearMslShaderSourceCacheDirectory() {
  std::lock_guard<std::mutex> lock(g_msl_source_cache_mutex);
  g_msl_source_cache_directory.clear();
}

// SPIR-V descriptor set indices matching SpirvShaderTranslator's layout.
// These values must stay in sync with the enum in spirv_shader_translator.h.
namespace SpirvSets {
constexpr uint32_t kSharedMemoryAndEdram = 0;
constexpr uint32_t kConstants = 1;
constexpr uint32_t kTexturesVertex = 2;
constexpr uint32_t kTexturesPixel = 3;
}  // namespace SpirvSets

// SPIR-V constant buffer binding indices within descriptor set 1.
// Must match SpirvShaderTranslator::ConstantBuffer enum.
namespace SpirvCbv {
constexpr uint32_t kSystem = 0;
constexpr uint32_t kFloatVertex = 1;
constexpr uint32_t kFloatPixel = 2;
constexpr uint32_t kBoolLoop = 3;
constexpr uint32_t kFetch = 4;
constexpr uint32_t kClipPlanes = 5;
constexpr uint32_t kTessellation = 6;
}  // namespace SpirvCbv

MslShader::MslShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                     const uint32_t* ucode_dwords, size_t ucode_dword_count,
                     std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian) {}

Shader::Translation* MslShader::CreateTranslationInstance(
    uint64_t modification) {
  return new MslTranslation(*this, modification);
}

MslShader::MslTranslation::~MslTranslation() {
  if (argument_encoder_) {
    argument_encoder_->release();
    argument_encoder_ = nullptr;
  }
  if (metal_function_) {
    metal_function_->release();
    metal_function_ = nullptr;
  }
  if (metal_library_) {
    metal_library_->release();
    metal_library_ = nullptr;
  }
}

// Metal buffer binding indices for resources.
// These must match the SPIRV-Cross remapping done below.
namespace MslBindings {
// Descriptor Set 0: Shared memory and EDRAM.
constexpr uint32_t kSharedMemory = 0;
// Descriptor Set 1: Constant buffers.
constexpr uint32_t kSystemConstants = 1;
constexpr uint32_t kFloatConstantsVertex = 2;
constexpr uint32_t kFloatConstantsPixel = 3;
constexpr uint32_t kBoolLoopConstants = 4;
constexpr uint32_t kFetchConstants = 5;
constexpr uint32_t kClipPlaneConstants = 6;
constexpr uint32_t kTessellationConstants = 7;
// Descriptor Set 2/3: Textures start at this index.
constexpr uint32_t kTextureBase = 0;
// Samplers start at this index.
constexpr uint32_t kSamplerBase = 0;
}  // namespace MslBindings

static const char* GetExecutionModelName(spv::ExecutionModel stage) {
  switch (stage) {
    case spv::ExecutionModelVertex:
      return "vertex";
    case spv::ExecutionModelFragment:
      return "fragment";
    case spv::ExecutionModelTessellationControl:
      return "tess_control";
    case spv::ExecutionModelTessellationEvaluation:
      return "tess_evaluation";
    case spv::ExecutionModelGLCompute:
      return "compute";
    default:
      return "other";
  }
}

static std::string JoinStringList(const std::vector<std::string>& values) {
  std::string joined;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) {
      joined += ", ";
    }
    joined += values[i];
  }
  return joined;
}

static void AddEntryPointCandidatesFromMslSource(
    const std::string& msl_source, xenos::ShaderType shader_type,
    std::vector<std::string>* candidates) {
  if (!candidates || msl_source.empty()) {
    return;
  }
  const char* expected_stage =
      shader_type == xenos::ShaderType::kVertex ? "vertex" : "fragment";
  // Match typical MSL stage entry declarations generated by SPIRV-Cross:
  //   vertex <ret> <name>(...)
  //   fragment <ret> <name>(...)
  static const std::regex kStageEntryRegex(
      R"((?:^|\n)\s*(vertex|fragment)\s+[^\n\(\{;]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\()",
      std::regex::optimize);
  for (std::sregex_iterator
           it(msl_source.begin(), msl_source.end(), kStageEntryRegex),
       end;
       it != end; ++it) {
    if (it->size() < 3) {
      continue;
    }
    const std::string stage = (*it)[1].str();
    if (stage != expected_stage) {
      continue;
    }
    const std::string name = (*it)[2].str();
    if (name.empty()) {
      continue;
    }
    if (std::find(candidates->begin(), candidates->end(), name) ==
        candidates->end()) {
      candidates->push_back(name);
    }
  }
  // Match alternate style:
  //   <ret> <name>(...) [[vertex]]
  //   <ret> <name>(...) [[fragment]]
  static const std::regex kAttributedEntryRegex(
      R"((?:^|\n)\s*[^\n\(\{;]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;\{\)]*\)\s*\[\[\s*(vertex|fragment)\s*\]\])",
      std::regex::optimize);
  for (std::sregex_iterator
           it(msl_source.begin(), msl_source.end(), kAttributedEntryRegex),
       end;
       it != end; ++it) {
    if (it->size() < 3) {
      continue;
    }
    const std::string name = (*it)[1].str();
    const std::string stage = (*it)[2].str();
    if (name.empty() || stage != expected_stage) {
      continue;
    }
    if (std::find(candidates->begin(), candidates->end(), name) ==
        candidates->end()) {
      candidates->push_back(name);
    }
  }
}

static void DumpMslResolveFailureSource(const std::string& msl_source,
                                        uint64_t shader_hash,
                                        uint64_t modification,
                                        xenos::ShaderType shader_type) {
  if (msl_source.empty()) {
    return;
  }
  const char* stage_tag =
      shader_type == xenos::ShaderType::kVertex ? "vs" : "ps";
  std::filesystem::path dump_dir;
  if (!cvars::dump_shaders.empty()) {
    dump_dir = std::filesystem::absolute(cvars::dump_shaders) / "msl_shaders" /
               "failures";
  } else {
    dump_dir = std::filesystem::temp_directory_path() / "xenia_msl_failures";
  }
  std::error_code ec;
  std::filesystem::create_directories(dump_dir, ec);
  if (ec) {
    XELOGW("MslShader: Failed to create unresolved-entry dump directory {}: {}",
           dump_dir.string(), ec.message());
    return;
  }
  std::filesystem::path dump_path =
      dump_dir / fmt::format("{}_{:016X}_mod{:016X}_entry_resolve_fail.metal",
                             stage_tag, shader_hash, modification);
  FILE* f = xe::filesystem::OpenFile(dump_path, "w");
  if (!f) {
    XELOGW("MslShader: Failed to open unresolved-entry dump file {}",
           dump_path.string());
    return;
  }
  fwrite(msl_source.data(), 1, msl_source.size(), f);
  fclose(f);
  XELOGW("MslShader: Dumped unresolved-entry MSL source to {}",
         dump_path.string());
}

static void AddResourceBindings(
    spirv_cross::CompilerMSL& compiler, spv::ExecutionModel stage,
    uint32_t texture_descriptor_set, uint64_t shader_hash,
    bool use_argument_buffers,
    std::vector<uint32_t>* sampler_spv_bindings_msl_order) {
  using MSLBinding = spirv_cross::MSLResourceBinding;

  // Set 0: Shared memory (storage buffer at binding 0).
  {
    MSLBinding binding;
    binding.stage = stage;
    binding.desc_set = SpirvSets::kSharedMemoryAndEdram;
    binding.binding = 0;
    binding.msl_buffer = MslBindings::kSharedMemory;
    binding.msl_texture = 0;
    binding.msl_sampler = 0;
    compiler.add_msl_resource_binding(binding);
  }

  // Set 0 binding 1: EDRAM (only used in FSI mode — skip for host RT path).
  // We still add a dummy mapping so SPIRV-Cross doesn't complain if the
  // shader happens to reference it (it shouldn't for kHostRenderTargets).
  {
    MSLBinding binding;
    binding.stage = stage;
    binding.desc_set = SpirvSets::kSharedMemoryAndEdram;
    binding.binding = 1;
    binding.msl_buffer = 30;  // High index, unused.
    binding.msl_texture = 0;
    binding.msl_sampler = 0;
    compiler.add_msl_resource_binding(binding);
  }

  // Set 1: Constant buffers.
  struct CbvMapping {
    uint32_t spirv_binding;
    uint32_t msl_buffer;
  };
  static const CbvMapping cbv_mappings[] = {
      {SpirvCbv::kSystem, MslBindings::kSystemConstants},
      {SpirvCbv::kFloatVertex, MslBindings::kFloatConstantsVertex},
      {SpirvCbv::kFloatPixel, MslBindings::kFloatConstantsPixel},
      {SpirvCbv::kBoolLoop, MslBindings::kBoolLoopConstants},
      {SpirvCbv::kFetch, MslBindings::kFetchConstants},
      {SpirvCbv::kClipPlanes, MslBindings::kClipPlaneConstants},
      {SpirvCbv::kTessellation, MslBindings::kTessellationConstants},
  };
  for (const auto& cbv : cbv_mappings) {
    MSLBinding binding;
    binding.stage = stage;
    binding.desc_set = SpirvSets::kConstants;
    binding.binding = cbv.spirv_binding;
    binding.msl_buffer = cbv.msl_buffer;
    binding.msl_texture = 0;
    binding.msl_sampler = 0;
    compiler.add_msl_resource_binding(binding);
  }

  // Set 2 (vertex textures) and Set 3 (pixel textures): texture bindings.
  //
  // The SPIR-V translator places sampler bindings AFTER texture bindings in
  // the same descriptor set (sampler i gets SPIR-V binding texture_count + i).
  // Metal only supports 16 samplers per stage, so we must remap sampler
  // bindings to compact indices 0..M-1 regardless of their SPIR-V binding.
  //
  // Use a single MSLResourceBinding entry per SPIR-V binding to avoid
  // overwrite issues (SPIRV-Cross keys entries by {stage, set, binding}).
  //
  // Step 1: Map all 32 possible texture bindings with identity mapping.
  //         Set msl_sampler = 0 as a safe default (overridden for real
  //         samplers in step 2).
  for (uint32_t set = SpirvSets::kTexturesVertex;
       set <= SpirvSets::kTexturesPixel; ++set) {
    if (use_argument_buffers) {
      // Map the argument buffer pointer itself to a Metal buffer slot.
      MSLBinding argbuf_binding;
      argbuf_binding.stage = stage;
      argbuf_binding.desc_set = set;
      argbuf_binding.binding = spirv_cross::kArgumentBufferBinding;
      argbuf_binding.msl_buffer =
          MslBufferIndex::kArgumentBufferTexturesSamplers;
      argbuf_binding.msl_texture = 0;
      argbuf_binding.msl_sampler = 0;
      compiler.add_msl_resource_binding(argbuf_binding);
    }
    for (uint32_t i = 0; i < MslTextureIndex::kMaxPerStage; ++i) {
      MSLBinding binding;
      binding.stage = stage;
      binding.desc_set = set;
      binding.binding = i;
      binding.msl_buffer = 0;
      binding.msl_texture = MslBindings::kTextureBase + i;
      binding.msl_sampler = 0;
      compiler.add_msl_resource_binding(binding);
    }
  }

  // Step 2: Query the actual SPIR-V resources to find sampler bindings
  //         and remap them to compact Metal sampler indices 0..M-1.
  //         This handles the case where SPIR-V sampler bindings exceed 15
  //         (which would violate Metal's 16-sampler-per-stage limit).
  auto resources = compiler.get_shader_resources();
  struct SamplerSpvBinding {
    uint32_t set;
    uint32_t binding;
  };
  std::vector<SamplerSpvBinding> sampler_spv_bindings;
  sampler_spv_bindings.reserve(resources.separate_samplers.size());
  for (const auto& samp : resources.separate_samplers) {
    uint32_t set =
        compiler.get_decoration(samp.id, spv::DecorationDescriptorSet);
    if (set != texture_descriptor_set) {
      continue;
    }
    uint32_t spv_binding =
        compiler.get_decoration(samp.id, spv::DecorationBinding);
    sampler_spv_bindings.push_back({set, spv_binding});
  }
  bool sampler_order_was_unsorted = false;
  for (size_t i = 1; i < sampler_spv_bindings.size(); ++i) {
    const auto& prev = sampler_spv_bindings[i - 1];
    const auto& curr = sampler_spv_bindings[i];
    if (curr.set < prev.set ||
        (curr.set == prev.set && curr.binding < prev.binding)) {
      sampler_order_was_unsorted = true;
      break;
    }
  }
  std::sort(sampler_spv_bindings.begin(), sampler_spv_bindings.end(),
            [](const SamplerSpvBinding& a, const SamplerSpvBinding& b) {
              if (a.set != b.set) {
                return a.set < b.set;
              }
              return a.binding < b.binding;
            });
  if (sampler_order_was_unsorted) {
    XELOGD(
        "MslShader: Reordered sampler SPIR-V bindings by set/binding for "
        "stable Metal remap shader={:016X} stage={}",
        shader_hash, GetExecutionModelName(stage));
  }
  uint32_t sampler_msl_index = 0;
  std::ostringstream sampler_remap_log;
  uint32_t sampler_remap_count = 0;
  for (size_t i = 0; i < sampler_spv_bindings.size(); ++i) {
    uint32_t set = sampler_spv_bindings[i].set;
    uint32_t spv_binding = sampler_spv_bindings[i].binding;
    if (i > 0 && set == sampler_spv_bindings[i - 1].set &&
        spv_binding == sampler_spv_bindings[i - 1].binding) {
      XELOGW(
          "MslShader: Duplicate sampler SPIR-V binding at set={} binding={} "
          "shader={:016X}",
          set, spv_binding, shader_hash);
      continue;
    }
    if (sampler_msl_index >= 16) {
      XELOGW(
          "MslShader: Too many samplers ({} >= 16), sampler at set={} "
          "binding={} will not be bound",
          sampler_msl_index, set, spv_binding);
      break;
    }
    // Overwrite the entry for this binding with the compact sampler index.
    // For standalone samplers, don't propagate the SPIR-V binding to
    // msl_texture; sampler SPIR-V bindings are after image bindings and may
    // exceed the per-stage Metal texture slot limit.
    MSLBinding binding;
    binding.stage = stage;
    binding.desc_set = set;
    binding.binding = spv_binding;
    binding.msl_buffer = 0;
    binding.msl_texture = MslTextureIndex::kBase;
    binding.msl_sampler =
        (use_argument_buffers ? MslArgumentBufferId::kSamplerBase
                              : MslBindings::kSamplerBase) +
        sampler_msl_index;
    compiler.add_msl_resource_binding(binding);
    if (sampler_spv_bindings_msl_order) {
      sampler_spv_bindings_msl_order->push_back(spv_binding);
    }
    sampler_remap_log << "[set=" << set << ",binding=" << spv_binding
                      << "->msl_sampler=" << sampler_msl_index << "] ";
    sampler_msl_index++;
    sampler_remap_count++;
  }
  if (sampler_remap_count) {
    XELOGD("MslShader: Sampler remap table shader={:016X} stage={} count={} {}",
           shader_hash, GetExecutionModelName(stage), sampler_remap_count,
           sampler_remap_log.str());
  } else {
    XELOGD("MslShader: Sampler remap table shader={:016X} stage={} count=0",
           shader_hash, GetExecutionModelName(stage));
  }
}

bool MslShader::MslTranslation::CompileToMsl(MTL::Device* device, bool is_ios) {
  if (!device) {
    XELOGE("MslShader: No Metal device provided");
    return false;
  }
  if (argument_encoder_) {
    argument_encoder_->release();
    argument_encoder_ = nullptr;
  }
  uses_argument_buffers_ = false;
  argument_encoder_alignment_ = 0;
  argument_encoder_encoded_length_ = 0;

  const std::vector<uint8_t>& spirv_data = translated_binary();
  if (spirv_data.empty()) {
    XELOGE("MslShader: No translated SPIR-V data available");
    return false;
  }
  const size_t spirv_size_bytes = spirv_data.size();
  if (spirv_size_bytes >= 64 * 1024) {
    XELOGW(
        "MslShader: Large SPIR-V module detected ({:016X}, {} bytes, stage={})",
        shader().ucode_data_hash(), spirv_size_bytes,
        shader().type() == xenos::ShaderType::kVertex ? "vertex" : "fragment");
  }
  const auto compile_begin = std::chrono::steady_clock::now();
  auto to_ms =
      [](const std::chrono::steady_clock::duration& duration) -> double {
    return std::chrono::duration<double, std::milli>(duration).count();
  };
  double spirv_cross_ms = 0.0;
  double metal_library_ms = 0.0;
  double function_resolve_ms = 0.0;

  // SPIR-V data is a vector of bytes, but SPIRV-Cross expects uint32_t words.
  if (spirv_data.size() % sizeof(uint32_t) != 0) {
    XELOGE("MslShader: SPIR-V data size is not aligned to 4 bytes");
    return false;
  }
  const uint32_t* spirv_words =
      reinterpret_cast<const uint32_t*>(spirv_data.data());
  size_t spirv_word_count = spirv_data.size() / sizeof(uint32_t);

  // SPIRV-Cross throws exceptions on translation errors.  We catch them
  // below and return false so the draw can be skipped gracefully instead
  // of crashing the process.
  spirv_cross::CompilerMSL compiler(spirv_words, spirv_word_count);

  // Configure MSL options.
  auto opts = compiler.get_msl_options();
  if (is_ios) {
    opts.platform = spirv_cross::CompilerMSL::Options::iOS;
  } else {
    opts.platform = spirv_cross::CompilerMSL::Options::macOS;
  }
  bool ios_supports_base_vertex_instance = false;
  bool ios_supports_cube_array = true;
  bool ios_supports_simdgroup_scope_ops = false;
  bool ios_supports_metal3 = false;
  bool ios_supports_apple9 = false;
  bool ios_supports_apple10 = false;
  if (is_ios) {
    // Apple docs: base vertex/instance drawing is Apple3+, cube array textures
    // are Apple4+, SIMD-scoped permute is Apple6+.
    ios_supports_base_vertex_instance =
        device->supportsFamily(MTL::GPUFamilyApple3);
    ios_supports_cube_array = device->supportsFamily(MTL::GPUFamilyApple4);
    ios_supports_simdgroup_scope_ops =
        device->supportsFamily(MTL::GPUFamilyApple6);
    // Apple Metal Feature Set Tables (2025-10-20): Metal 3+ starts at Apple7,
    // Apple9/Apple10 map to newer iPhone GPU families (A17/A18 and A19).
    ios_supports_metal3 = device->supportsFamily(MTL::GPUFamilyApple7);
    ios_supports_apple9 = device->supportsFamily(MTL::GPUFamilyApple9);
    ios_supports_apple10 = device->supportsFamily(MTL::GPUFamilyApple10);
  }
  // iOS: prefer Metal 3 language levels on Apple7+ devices.
  // Keep legacy fallback for older iOS GPUs.
  uint32_t msl_major = 2;
  uint32_t msl_minor = is_ios ? 3u : 4u;
  MTL::LanguageVersion language_version =
      is_ios ? MTL::LanguageVersion2_3 : MTL::LanguageVersion2_4;
  if (is_ios && ios_supports_metal3) {
    msl_major = 3;
    if (ios_supports_apple10) {
      msl_minor = 2;
      language_version = MTL::LanguageVersion3_2;
    } else if (ios_supports_apple9) {
      msl_minor = 1;
      language_version = MTL::LanguageVersion3_1;
    } else {
      msl_minor = 0;
      language_version = MTL::LanguageVersion3_0;
    }
  }
  opts.msl_version =
      spirv_cross::CompilerMSL::Options::make_msl_version(msl_major, msl_minor);
  // Use argument buffers for SPIRV-Cross texture/sampler bindings to reduce
  // per-draw CPU binding overhead.
  uses_argument_buffers_ = true;
  opts.argument_buffers = uses_argument_buffers_;
  uint8_t argument_buffers_tier_key = 0;
  if (uses_argument_buffers_) {
    if (device->argumentBuffersSupport() >= MTL::ArgumentBuffersTier2) {
      opts.argument_buffers_tier =
          spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
      argument_buffers_tier_key = 1;
    } else {
      opts.argument_buffers_tier =
          spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier1;
      argument_buffers_tier_key = 0;
    }
    // Keep argument-buffer layouts stable even if SPIRV-Cross marks specific
    // resources inactive in some variants.
    opts.force_active_argument_buffer_resources = true;
  }
  // Match iOS feature toggles to documented GPU-family availability.
  opts.ios_support_base_vertex_instance = ios_supports_base_vertex_instance;
  const bool emulate_cube_array = is_ios && !ios_supports_cube_array;
  opts.emulate_cube_array = emulate_cube_array;
  // Keep iOS aligned with the conservative cross-platform path by default.
  // Enabling simdgroup translation on some iOS shader/compiler combinations can
  // yield libraries with no exported stage entry points (`functionNames=[]`).
  const bool ios_use_simdgroup_functions =
      is_ios && ios_supports_simdgroup_scope_ops;
  opts.ios_use_simdgroup_functions = ios_use_simdgroup_functions;
  // Force sample rate shading to be available if needed.
  opts.force_sample_rate_shading = false;
  // Keep native component counts for fragment outputs to preserve shader-
  // defined alpha semantics and avoid over-constraining RT writes.
  opts.pad_fragment_output_components = false;
  const uint64_t msl_source_cache_key = GetMslSourceCacheKey(
      *this, is_ios, msl_major, msl_minor, ios_supports_base_vertex_instance,
      emulate_cube_array, ios_use_simdgroup_functions, uses_argument_buffers_,
      argument_buffers_tier_key);
  compiler.set_msl_options(opts);
  if (uses_argument_buffers_) {
    // Keep shared memory/storage buffers and constant buffers as discrete
    // bindings; put textures + samplers in argument buffers.
    compiler.add_discrete_descriptor_set(SpirvSets::kSharedMemoryAndEdram);
    compiler.add_discrete_descriptor_set(SpirvSets::kConstants);
  }

  // Remap SPIR-V descriptor sets/bindings to Metal buffer/texture/sampler
  // indices.
  // Query the actual execution model from the SPIR-V binary rather than
  // assuming kVertex always maps to spv::ExecutionModelVertex.  The
  // SpirvShaderTranslator already sets the correct model — domain shaders
  // (tessellation evaluation) are emitted as TessellationEvaluation, not
  // Vertex — so we must honour that for SPIRV-Cross resource remapping and
  // entry-point lookup.
  std::string spirv_entry_point_name = "main";
  std::vector<std::string> spirv_entry_point_descriptions;
  spv::ExecutionModel execution_model;
  {
    auto entry_points = compiler.get_entry_points_and_stages();
    if (!entry_points.empty()) {
      spirv_entry_point_descriptions.reserve(entry_points.size());
      for (const auto& entry_point : entry_points) {
        spirv_entry_point_descriptions.push_back(
            fmt::format("{}:{}", entry_point.name,
                        GetExecutionModelName(entry_point.execution_model)));
      }
      spv::ExecutionModel preferred_model =
          shader().type() == xenos::ShaderType::kVertex
              ? spv::ExecutionModelVertex
              : spv::ExecutionModelFragment;
      const spirv_cross::EntryPoint* selected_entry = &entry_points.front();
      for (const auto& entry_point : entry_points) {
        if (entry_point.execution_model == preferred_model) {
          selected_entry = &entry_point;
          break;
        }
      }
      execution_model = selected_entry->execution_model;
      if (!selected_entry->name.empty()) {
        spirv_entry_point_name = selected_entry->name;
      }
    } else {
      // Fallback: infer from shader type (pre-tessellation behavior).
      execution_model = shader().type() == xenos::ShaderType::kVertex
                            ? spv::ExecutionModelVertex
                            : spv::ExecutionModelFragment;
      XELOGW(
          "MslShader: SPIR-V module has no declared entry points "
          "(shader={:016X}, expected_stage={})",
          shader().ucode_data_hash(), GetExecutionModelName(execution_model));
    }
  }
  try {
    compiler.set_entry_point(spirv_entry_point_name, execution_model);
  } catch (const std::exception& e) {
    XELOGW(
        "MslShader: Failed to set SPIR-V entry point '{}' (stage={}) "
        "shader={:016X}: {}",
        spirv_entry_point_name, GetExecutionModelName(execution_model),
        shader().ucode_data_hash(), e.what());
  }
  sampler_binding_indices_for_msl_slots_.clear();
  texture_binding_indices_for_msl_slots_.clear();
  // Domain shaders use tessellation-evaluation execution model, but still use
  // the vertex texture descriptor set in SpirvShaderTranslator.
  uint32_t texture_descriptor_set =
      shader().type() == xenos::ShaderType::kVertex ? SpirvSets::kTexturesVertex
                                                    : SpirvSets::kTexturesPixel;
  std::vector<uint32_t> sampler_spv_bindings_msl_order;
  AddResourceBindings(compiler, execution_model, texture_descriptor_set,
                      shader().ucode_data_hash(), uses_argument_buffers_,
                      &sampler_spv_bindings_msl_order);
  auto resources = compiler.get_shader_resources();

  // Build the runtime texture binding lookup for Metal slots.
  // Slots are direct (msl_texture = SPIR-V binding). Seed with translator
  // order, then refine with reflection where possible.
  const MslShader& msl_shader = static_cast<const MslShader&>(shader());
  const auto& shader_texture_bindings =
      msl_shader.GetTextureBindingsAfterTranslation();
  auto find_runtime_texture_binding_index =
      [&](uint32_t fetch_constant, xenos::FetchOpDimension dimension,
          bool is_signed) -> int32_t {
    for (size_t i = 0; i < shader_texture_bindings.size(); ++i) {
      const auto& binding = shader_texture_bindings[i];
      if (binding.fetch_constant == fetch_constant &&
          binding.dimension == dimension && binding.is_signed == is_signed) {
        return int32_t(i);
      }
    }
    return -1;
  };
  auto parse_texture_resource_name =
      [](const std::string& name, uint32_t* fetch_constant_out,
         xenos::FetchOpDimension* dimension_out, bool* is_signed_out) -> bool {
    static constexpr const char kPrefix[] = "xe_texture";
    if (!fetch_constant_out || !dimension_out || !is_signed_out ||
        name.size() <= sizeof(kPrefix) - 1 ||
        name.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) {
      return false;
    }
    size_t offset = sizeof(kPrefix) - 1;
    uint32_t fetch_constant = 0;
    bool has_digit = false;
    while (offset < name.size() &&
           std::isdigit(static_cast<unsigned char>(name[offset]))) {
      has_digit = true;
      fetch_constant = fetch_constant * 10 + uint32_t(name[offset] - '0');
      ++offset;
    }
    if (!has_digit || offset >= name.size() || name[offset] != '_') {
      return false;
    }
    ++offset;

    size_t dim_end = name.find('_', offset);
    if (dim_end == std::string::npos || dim_end <= offset) {
      return false;
    }
    std::string dim_token = name.substr(offset, dim_end - offset);
    xenos::FetchOpDimension dimension = xenos::FetchOpDimension::k2D;
    if (dim_token == "2d") {
      dimension = xenos::FetchOpDimension::k2D;
    } else if (dim_token == "3d") {
      dimension = xenos::FetchOpDimension::k3DOrStacked;
    } else if (dim_token == "cube") {
      dimension = xenos::FetchOpDimension::kCube;
    } else {
      return false;
    }

    offset = dim_end + 1;
    if (offset >= name.size()) {
      return false;
    }
    char sign_char = name[offset];
    if (sign_char != 's' && sign_char != 'u') {
      return false;
    }

    *fetch_constant_out = fetch_constant;
    *dimension_out = dimension;
    *is_signed_out = sign_char == 's';
    return true;
  };
  texture_binding_indices_for_msl_slots_.reserve(std::min<size_t>(
      shader_texture_bindings.size(), MslTextureIndex::kMaxPerStage));
  for (uint32_t i = 0;
       i < std::min<uint32_t>(uint32_t(shader_texture_bindings.size()),
                              MslTextureIndex::kMaxPerStage);
       ++i) {
    texture_binding_indices_for_msl_slots_.push_back(int32_t(i));
  }
  bool has_reflected_texture_bindings = false;
  bool texture_reflection_incomplete = false;
  auto register_texture_binding =
      [&](const spirv_cross::Resource& resource) -> void {
    uint32_t set =
        compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
    if (set != texture_descriptor_set) {
      return;
    }
    has_reflected_texture_bindings = true;
    uint32_t spv_binding =
        compiler.get_decoration(resource.id, spv::DecorationBinding);
    if (spv_binding >= MslTextureIndex::kMaxPerStage) {
      XELOGW(
          "MslShader: Reflected texture binding {} exceeds Metal slot limit {} "
          "shader={:016X}",
          spv_binding, MslTextureIndex::kMaxPerStage,
          shader().ucode_data_hash());
      return;
    }
    int32_t runtime_binding_index = -1;
    uint32_t parsed_fetch_constant = 0;
    xenos::FetchOpDimension parsed_dimension = xenos::FetchOpDimension::k2D;
    bool parsed_is_signed = false;
    if (parse_texture_resource_name(resource.name, &parsed_fetch_constant,
                                    &parsed_dimension, &parsed_is_signed)) {
      runtime_binding_index = find_runtime_texture_binding_index(
          parsed_fetch_constant, parsed_dimension, parsed_is_signed);
      if (runtime_binding_index < 0) {
        XELOGW(
            "MslShader: Unable to resolve texture resource '{}' in runtime "
            "bindings shader={:016X}",
            resource.name, shader().ucode_data_hash());
      }
    }
    if (runtime_binding_index < 0) {
      // Fall back to SPIR-V decoration binding index when reflection names are
      // unavailable or don't match translator naming.
      if (spv_binding >= shader_texture_bindings.size()) {
        XELOGW(
            "MslShader: Reflected texture binding {} is out of runtime range "
            "{} "
            "shader={:016X}",
            spv_binding, shader_texture_bindings.size(),
            shader().ucode_data_hash());
        texture_reflection_incomplete = true;
        return;
      }
      runtime_binding_index = int32_t(spv_binding);
    }

    if (texture_binding_indices_for_msl_slots_.size() <= spv_binding) {
      texture_binding_indices_for_msl_slots_.resize(spv_binding + 1, -1);
    }
    int32_t& slot_binding = texture_binding_indices_for_msl_slots_[spv_binding];
    if (slot_binding == -1) {
      slot_binding = runtime_binding_index;
    } else if (slot_binding != runtime_binding_index) {
      XELOGW(
          "MslShader: Conflicting texture remap at slot {} ({} vs {}) "
          "shader={:016X}",
          spv_binding, slot_binding, runtime_binding_index,
          shader().ucode_data_hash());
      texture_reflection_incomplete = true;
    }
  };
  for (const auto& image : resources.separate_images) {
    register_texture_binding(image);
  }
  for (const auto& image : resources.sampled_images) {
    register_texture_binding(image);
  }
  if (!has_reflected_texture_bindings && !shader_texture_bindings.empty()) {
    XELOGW(
        "MslShader: No reflected texture bindings for shader={:016X} stage={} "
        "(runtime={}), using translator-order texture remap",
        shader().ucode_data_hash(), GetExecutionModelName(execution_model),
        shader_texture_bindings.size());
  } else if (texture_reflection_incomplete) {
    XELOGW(
        "MslShader: Incomplete texture reflection remap for shader={:016X} "
        "stage={}, keeping translator-order mappings for unresolved slots",
        shader().ucode_data_hash(), GetExecutionModelName(execution_model));
  }

  // Build the runtime sampler binding lookup for Metal slots.
  // This is needed because SPIRV-Cross may drop / reorder separate samplers,
  // while runtime sampler parameters are stored in translator binding order.
  uint32_t shader_texture_binding_count = static_cast<uint32_t>(
      msl_shader.GetTextureBindingsAfterTranslation().size());
  const auto& shader_sampler_bindings =
      msl_shader.GetSamplerBindingsAfterTranslation();
  std::vector<bool> sampler_binding_mapped(shader_sampler_bindings.size(),
                                           false);
  sampler_binding_indices_for_msl_slots_.reserve(
      sampler_spv_bindings_msl_order.size());
  for (uint32_t spv_binding : sampler_spv_bindings_msl_order) {
    if (spv_binding < shader_texture_binding_count) {
      XELOGW(
          "MslShader: Reflected sampler binding {} is below texture binding "
          "base {} shader={:016X}",
          spv_binding, shader_texture_binding_count,
          shader().ucode_data_hash());
      continue;
    }
    uint32_t sampler_binding_index = spv_binding - shader_texture_binding_count;
    if (sampler_binding_index >= shader_sampler_bindings.size()) {
      XELOGW(
          "MslShader: Reflected sampler binding {} maps to out-of-range "
          "sampler index {} (available {}) shader={:016X}",
          spv_binding, sampler_binding_index, shader_sampler_bindings.size(),
          shader().ucode_data_hash());
      continue;
    }
    if (sampler_binding_index < sampler_binding_mapped.size() &&
        sampler_binding_mapped[sampler_binding_index]) {
      continue;
    }
    sampler_binding_indices_for_msl_slots_.push_back(sampler_binding_index);
    if (sampler_binding_index < sampler_binding_mapped.size()) {
      sampler_binding_mapped[sampler_binding_index] = true;
    }
  }
  size_t sampler_mapped_count = sampler_binding_indices_for_msl_slots_.size();
  if (!shader_sampler_bindings.empty() &&
      sampler_mapped_count < shader_sampler_bindings.size()) {
    for (uint32_t i = 0; i < shader_sampler_bindings.size(); ++i) {
      if (!sampler_binding_mapped[i]) {
        sampler_binding_indices_for_msl_slots_.push_back(i);
      }
    }
  }
  bool sampler_reflection_incomplete =
      sampler_spv_bindings_msl_order.size() > sampler_mapped_count;
  if (!shader_sampler_bindings.empty() && sampler_reflection_incomplete) {
    XELOGW(
        "MslShader: Sampler reflection remap incomplete; preserving mapped "
        "samplers and appending unmapped samplers in translator order "
        "shader={:016X} stage={} (reflected={} mapped={} runtime={})",
        shader().ucode_data_hash(), GetExecutionModelName(execution_model),
        sampler_spv_bindings_msl_order.size(), sampler_mapped_count,
        shader_sampler_bindings.size());
  } else if (sampler_spv_bindings_msl_order.empty() &&
             !shader_sampler_bindings.empty()) {
    XELOGW(
        "MslShader: No reflected sampler bindings for shader={:016X} stage={}, "
        "using translator-order sampler remap",
        shader().ucode_data_hash(), GetExecutionModelName(execution_model));
  }
  if (sampler_binding_indices_for_msl_slots_.size() >
      MslSamplerIndex::kMaxPerStage) {
    sampler_binding_indices_for_msl_slots_.resize(
        MslSamplerIndex::kMaxPerStage);
  }

  // Validate resource counts against Metal limits before compilation.
  bool has_argument_buffer_resources = false;
  {
    bool expects_tessellation_constants = false;
    for (const auto& uniform_buffer : resources.uniform_buffers) {
      uint32_t set = compiler.get_decoration(uniform_buffer.id,
                                             spv::DecorationDescriptorSet);
      uint32_t binding =
          compiler.get_decoration(uniform_buffer.id, spv::DecorationBinding);
      if (set == SpirvSets::kConstants && binding == SpirvCbv::kTessellation) {
        expects_tessellation_constants = true;
        break;
      }
    }
    size_t texture_count =
        resources.sampled_images.size() + resources.separate_images.size();
    size_t sampler_count = resources.separate_samplers.size();
    has_argument_buffer_resources = (texture_count + sampler_count) != 0;
    XELOGD(
        "MslShader: Resource summary shader={:016X} stage={} textures={} "
        "samplers={} uniform_buffers={} tessellation_cbv_expected={}",
        shader().ucode_data_hash(), GetExecutionModelName(execution_model),
        texture_count, sampler_count, resources.uniform_buffers.size(),
        expects_tessellation_constants ? 1 : 0);
    if (expects_tessellation_constants) {
      XELOGW(
          "MslShader: Shader {:016X} stage={} declares set={},binding={} "
          "tessellation constants (msl_buffer={})",
          shader().ucode_data_hash(), GetExecutionModelName(execution_model),
          SpirvSets::kConstants, SpirvCbv::kTessellation,
          MslBindings::kTessellationConstants);
    }
    if (texture_count > MslTextureIndex::kMaxPerStage) {
      XELOGE(
          "MslShader: Shader uses {} textures, exceeding Metal's {}-per-stage "
          "limit in this backend",
          texture_count, MslTextureIndex::kMaxPerStage);
      return false;
    }
    if (sampler_count > 16) {
      XELOGE(
          "MslShader: Shader uses {} samplers, exceeding Metal's 16-per-stage "
          "limit — translation should be considered failed",
          sampler_count);
      return false;
    }
  }

  // Compile to MSL.  SPIRV-Cross throws on translation errors.
  const auto spirv_cross_begin = std::chrono::steady_clock::now();
  bool loaded_source_from_cache = false;
  if (cvars::metal_shader_disk_cache) {
    loaded_source_from_cache =
        LoadCachedMslSource(msl_source_cache_key, &msl_source_);
    if (loaded_source_from_cache) {
      XELOGD("MslShader: Loaded cached MSL source ({:016X})",
             shader().ucode_data_hash());
    }
  }
  if (!loaded_source_from_cache) {
    try {
      msl_source_ = compiler.compile();
    } catch (const std::exception& e) {
      XELOGE("MslShader: SPIRV-Cross compilation failed: {}", e.what());
      return false;
    }
    if (cvars::metal_shader_disk_cache) {
      StoreCachedMslSource(msl_source_cache_key, msl_source_);
    }
  }
  spirv_cross_ms = to_ms(std::chrono::steady_clock::now() - spirv_cross_begin);
  if (msl_source_.empty()) {
    XELOGE("MslShader: SPIRV-Cross compilation produced empty output");
    return false;
  }
  if (cvars::metal_spirvcross_strip_nocontract_optnone) {
    size_t stripped = StripNoContractionOptnoneWrappers(msl_source_);
    if (stripped != 0) {
      XELOGD(
          "MslShader: Stripped {} NoContraction optnone wrapper signature(s) "
          "for shader {:016X}",
          stripped, shader().ucode_data_hash());
    }
  }

  // Get the entry point name that SPIRV-Cross chose.
  std::vector<std::string> entry_point_candidates;
  auto add_entry_point_candidate = [&](const std::string& name) {
    if (name.empty()) {
      return;
    }
    if (std::find(entry_point_candidates.begin(), entry_point_candidates.end(),
                  name) == entry_point_candidates.end()) {
      entry_point_candidates.push_back(name);
    }
  };
  add_entry_point_candidate(spirv_entry_point_name);
  try {
    add_entry_point_candidate(compiler.get_cleansed_entry_point_name(
        spirv_entry_point_name, execution_model));
  } catch (const std::exception& e) {
    XELOGW(
        "MslShader: SPIRV-Cross entry point lookup failed for '{}' "
        "(stage={}): {}",
        spirv_entry_point_name, GetExecutionModelName(execution_model),
        e.what());
  }
  if (spirv_entry_point_name != "main") {
    try {
      add_entry_point_candidate(
          compiler.get_cleansed_entry_point_name("main", execution_model));
    } catch (const std::exception&) {
    }
  }
  add_entry_point_candidate("main0");
  add_entry_point_candidate("main");
  add_entry_point_candidate("vertexMain");
  add_entry_point_candidate("fragmentMain");
  if (entry_point_candidates.empty()) {
    entry_point_candidates.push_back("main0");
  }
  AddEntryPointCandidatesFromMslSource(msl_source_, shader().type(),
                                       &entry_point_candidates);
  entry_point_name_ = entry_point_candidates.front();
  XELOGD("MslShader: Compiled SPIR-V to MSL ({} bytes, entries: {})",
         msl_source_.size(), JoinStringList(entry_point_candidates));

  // Dump MSL source when --dump_shaders is set (matching Vulkan dump pattern).
  if (!cvars::dump_shaders.empty()) {
    auto base_path = std::filesystem::absolute(cvars::dump_shaders);
    auto msl_dir = base_path / "msl_shaders";
    std::filesystem::create_directories(msl_dir);
    const char* type_str =
        (shader().type() == xenos::ShaderType::kVertex) ? "vs" : "ps";
    auto msl_path =
        msl_dir / fmt::format("{}_{:016X}_mod{:016X}.metal", type_str,
                              shader().ucode_data_hash(), modification());
    FILE* f = xe::filesystem::OpenFile(msl_path, "w");
    if (f) {
      fwrite(msl_source_.data(), 1, msl_source_.size(), f);
      fclose(f);
      XELOGD("MslShader: Dumped MSL to {}", msl_path.string());
    }
  }

  // Log SPIRV-Cross resolved SystemConstants member offsets for diagnostic
  // comparison with the C++ struct layout.
  try {
    auto resources = compiler.get_shader_resources();
    for (const auto& ubo : resources.uniform_buffers) {
      uint32_t set =
          compiler.get_decoration(ubo.id, spv::DecorationDescriptorSet);
      uint32_t binding =
          compiler.get_decoration(ubo.id, spv::DecorationBinding);
      if (set == SpirvSets::kConstants && binding == SpirvCbv::kSystem) {
        auto& base_type = compiler.get_type(ubo.base_type_id);
        uint32_t member_count =
            static_cast<uint32_t>(base_type.member_types.size());
        XELOGD(
            "MslShader: SystemConstants reflection shader={:016X} "
            "members={}",
            shader().ucode_data_hash(), member_count);
        for (uint32_t m = 0; m < member_count; ++m) {
          uint32_t offset = compiler.get_member_decoration(
              base_type.self, m, spv::DecorationOffset);
          const std::string& name = compiler.get_member_name(base_type.self, m);
          XELOGD("  [{}] '{}' offset={}", m, name, offset);
        }
        break;
      }
    }
  } catch (const std::exception& e) {
    XELOGW("MslShader: SystemConstants reflection failed: {}", e.what());
  }

  // Compile MSL source to a Metal library.
  // Use a local autorelease pool to ensure NS::String factory objects and any
  // NS::Error autoreleased by the Metal driver are cleaned up promptly.
  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

  NS::Error* error = nullptr;
  auto* source_str =
      NS::String::string(msl_source_.c_str(), NS::UTF8StringEncoding);
  auto* compile_options = MTL::CompileOptions::alloc()->init();
  // Use fast math for better performance (matches Xbox 360 behavior better
  // than strict IEEE — the 360's ALU doesn't fully conform to IEEE anyway).
  compile_options->setFastMathEnabled(true);
  // Match the Metal compiler language version to SPIRV-Cross output.
  compile_options->setLanguageVersion(language_version);

  const auto metal_library_begin = std::chrono::steady_clock::now();
  metal_library_ = device->newLibrary(source_str, compile_options, &error);
  metal_library_ms =
      to_ms(std::chrono::steady_clock::now() - metal_library_begin);
  compile_options->release();

  if (error && error->localizedDescription() &&
      error->localizedDescription()->utf8String()) {
    // Metal may return warnings alongside a non-null library.
    XELOGW("MslShader: Metal compile diagnostics: {}",
           error->localizedDescription()->utf8String());
  }

  if (!metal_library_) {
    if (error) {
      XELOGE("MslShader: Metal library compilation failed: {}",
             error->localizedDescription()->utf8String());
    } else {
      XELOGE("MslShader: Metal library compilation failed (unknown error)");
    }
    pool->release();
    return false;
  }

  // Get the entry point function.
  std::vector<std::string> attempted_function_names;
  std::vector<std::string> available_function_names;
  const auto function_resolve_begin = std::chrono::steady_clock::now();
  auto try_bind_function = [&](const std::string& function_name) -> bool {
    if (function_name.empty()) {
      return false;
    }
    if (std::find(attempted_function_names.begin(),
                  attempted_function_names.end(),
                  function_name) != attempted_function_names.end()) {
      return false;
    }
    attempted_function_names.push_back(function_name);
    auto* fn_name =
        NS::String::string(function_name.c_str(), NS::UTF8StringEncoding);
    MTL::Function* function = metal_library_->newFunction(fn_name);
    if (!function) {
      return false;
    }
    metal_function_ = function;
    entry_point_name_ = function_name;
    return true;
  };
  for (const auto& candidate : entry_point_candidates) {
    if (try_bind_function(candidate)) {
      break;
    }
  }
  if (!metal_function_) {
    NS::Array* function_names = metal_library_->functionNames();
    if (function_names) {
      for (NS::UInteger i = 0; i < function_names->count(); ++i) {
        auto* name = static_cast<NS::String*>(function_names->object(i));
        if (!name || !name->utf8String()) {
          continue;
        }
        available_function_names.emplace_back(name->utf8String());
      }
    }
    if (available_function_names.size() == 1) {
      try_bind_function(available_function_names.front());
    }
    if (!metal_function_) {
      for (const auto& available_name : available_function_names) {
        if (available_name.find("main") != std::string::npos &&
            try_bind_function(available_name)) {
          break;
        }
      }
    }
    if (!metal_function_) {
      const char* stage_hint =
          shader().type() == xenos::ShaderType::kVertex ? "vertex" : "fragment";
      for (const auto& available_name : available_function_names) {
        if (available_name.find(stage_hint) != std::string::npos &&
            try_bind_function(available_name)) {
          break;
        }
      }
    }
  }
  if (!metal_function_) {
    DumpMslResolveFailureSource(msl_source_, shader().ucode_data_hash(),
                                modification(), shader().type());
    XELOGE(
        "MslShader: Could not resolve function in compiled library "
        "(shader={:016X}, stage={}, spirv_entry='{}', candidates=[{}], "
        "attempted=[{}], available=[{}], spirv_entries=[{}])",
        shader().ucode_data_hash(), GetExecutionModelName(execution_model),
        spirv_entry_point_name, JoinStringList(entry_point_candidates),
        JoinStringList(attempted_function_names),
        JoinStringList(available_function_names),
        JoinStringList(spirv_entry_point_descriptions));
    metal_library_->release();
    metal_library_ = nullptr;
    pool->release();
    return false;
  }
  function_resolve_ms =
      to_ms(std::chrono::steady_clock::now() - function_resolve_begin);

  if (uses_argument_buffers_ && has_argument_buffer_resources) {
    argument_encoder_ = metal_function_->newArgumentEncoder(
        MslBufferIndex::kArgumentBufferTexturesSamplers);
    if (argument_encoder_) {
      argument_encoder_alignment_ =
          static_cast<uint32_t>(argument_encoder_->alignment());
      argument_encoder_encoded_length_ =
          static_cast<uint32_t>(argument_encoder_->encodedLength());
    } else {
      XELOGE("MslShader: Failed to create argument encoder for shader {:016X}",
             shader().ucode_data_hash());
      metal_function_->release();
      metal_function_ = nullptr;
      metal_library_->release();
      metal_library_ = nullptr;
      pool->release();
      return false;
    }
  } else {
    argument_encoder_alignment_ = 0;
    argument_encoder_encoded_length_ = 0;
  }

  pool->release();

  const double total_compile_ms =
      to_ms(std::chrono::steady_clock::now() - compile_begin);
  if (total_compile_ms >= 100.0 || spirv_size_bytes >= 64 * 1024) {
    XELOGW(
        "MslShader: Compile timings shader={:016X} stage={} spirv={}B "
        "msl={}B total={:.2f}ms (spirv-cross={:.2f}ms, metal-library={:.2f}ms, "
        "entry-resolve={:.2f}ms)",
        shader().ucode_data_hash(),
        shader().type() == xenos::ShaderType::kVertex ? "vertex" : "fragment",
        spirv_size_bytes, msl_source_.size(), total_compile_ms, spirv_cross_ms,
        metal_library_ms, function_resolve_ms);
  } else {
    XELOGD(
        "MslShader: Compile timings shader={:016X} stage={} total={:.2f}ms "
        "(spirv-cross={:.2f}ms, metal-library={:.2f}ms, "
        "entry-resolve={:.2f}ms)",
        shader().ucode_data_hash(),
        shader().type() == xenos::ShaderType::kVertex ? "vertex" : "fragment",
        total_compile_ms, spirv_cross_ms, metal_library_ms,
        function_resolve_ms);
  }

  XELOGD("MslShader: Successfully created Metal function '{}'",
         entry_point_name_);
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
