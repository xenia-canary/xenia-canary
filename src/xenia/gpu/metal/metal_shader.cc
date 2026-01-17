/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shader.h"

#include <dispatch/dispatch.h>
#include <inttypes.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/dxbc_to_dxil_converter.h"
#include "xenia/gpu/metal/metal_shader_cache.h"
#include "xenia/gpu/metal/metal_shader_converter.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

MetalShader::MetalShader(xenos::ShaderType shader_type,
                         uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
                         size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : DxbcShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                 ucode_source_endian) {}

MetalShader::MetalTranslation::~MetalTranslation() {
  if (metal_function_) {
    metal_function_->release();
    metal_function_ = nullptr;
  }
  if (metal_library_) {
    metal_library_->release();
    metal_library_ = nullptr;
  }
}

bool MetalShader::MetalTranslation::TranslateToMetal(
    MTL::Device* device, DxbcToDxilConverter& dxbc_converter,
    MetalShaderConverter& metal_converter) {
  if (!device) {
    XELOGE("MetalShader: No Metal device provided");
    return false;
  }

  // Get the translated DXBC bytecode from the base class
  const std::vector<uint8_t>& dxbc_data = translated_binary();
  if (dxbc_data.empty()) {
    XELOGE("MetalShader: No translated DXBC data available");
    return false;
  }

  const uint64_t shader_cache_key =
      MetalShaderCache::GetCacheKey(shader().ucode_data_hash(), modification(),
                                    static_cast<uint32_t>(shader().type()));

  if (cvars::metal_shader_disk_cache && g_metal_shader_cache &&
      g_metal_shader_cache->IsInitialized()) {
    MetalShaderCache::CachedMetallib cached;
    if (g_metal_shader_cache->Load(shader_cache_key, &cached)) {
      NS::Error* error = nullptr;
      dispatch_data_t cached_data = dispatch_data_create(
          cached.metallib_data.data(), cached.metallib_data.size(), nullptr,
          DISPATCH_DATA_DESTRUCTOR_NONE);
      metal_library_ = device->newLibrary(cached_data, &error);
      dispatch_release(cached_data);
      if (metal_library_) {
        function_name_ = cached.function_name;
        metallib_data_ = std::move(cached.metallib_data);
        NS::String* function_name_ns =
            NS::String::string(function_name_.c_str(), NS::UTF8StringEncoding);
        metal_function_ = metal_library_->newFunction(function_name_ns);
        if (metal_function_) {
          return true;
        }
        metal_library_->release();
        metal_library_ = nullptr;
      }
    }
  }
  auto dump_msc_failure = [&](const char* reason) {
    if (cvars::dump_shaders.empty()) {
      return;
    }
    static std::atomic<uint32_t> dump_counter{0};
    uint32_t dump_id = dump_counter.fetch_add(1);
    const char* type_str =
        (shader().type() == xenos::ShaderType::kVertex) ? "vs" : "ps";
    char base_name[128];
    snprintf(base_name, sizeof(base_name), "shader_%016" PRIx64 "_%s_%u",
             shader().ucode_data_hash(), type_str, dump_id);

    std::filesystem::path base_dir =
        cvars::dump_shaders / "metal_shaders" / "failures";
    std::filesystem::path dxbc_path =
        base_dir / (std::string(base_name) + ".dxbc");
    std::filesystem::path dxil_path =
        base_dir / (std::string(base_name) + ".dxil");
    std::filesystem::path info_path =
        base_dir / (std::string(base_name) + ".txt");

    xe::filesystem::CreateParentFolder(dxbc_path);

    FILE* info_file = xe::filesystem::OpenFile(info_path, "wb");
    if (info_file) {
      std::string info =
          fmt::format("reason={}\nshader_type={}\nucode_hash=0x{:016X}\n",
                      reason, type_str, shader().ucode_data_hash());
      fwrite(info.data(), 1, info.size(), info_file);
      fclose(info_file);
    }

    if (!dxbc_data.empty()) {
      FILE* f = xe::filesystem::OpenFile(dxbc_path, "wb");
      if (f) {
        fwrite(dxbc_data.data(), 1, dxbc_data.size(), f);
        fclose(f);
      }
    }

    if (!dxil_data_.empty()) {
      FILE* f = xe::filesystem::OpenFile(dxil_path, "wb");
      if (f) {
        fwrite(dxil_data_.data(), 1, dxil_data_.size(), f);
        fclose(f);
      }
    }

    XELOGE("MetalShader: dumped MSC failure artifacts to {} (reason={})",
           xe::path_to_utf8(info_path.parent_path()), reason);
  };

  // Step 1: Convert DXBC to DXIL in-process (dxilconv)
  std::string dxbc_error;
  if (!dxbc_converter.Convert(dxbc_data, dxil_data_, &dxbc_error)) {
    XELOGE("MetalShader: DXBC to DXIL conversion failed: {}", dxbc_error);
    dump_msc_failure("dxbc2dxil_failed");
    return false;
  }
  XELOGD("MetalShader: Converted {} bytes DXBC to {} bytes DXIL",
         dxbc_data.size(), dxil_data_.size());

  // Step 2: Convert DXIL to MetalLib using Metal Shader Converter
  MetalShaderConversionResult msc_result;
  if (!metal_converter.Convert(shader().type(), dxil_data_, msc_result)) {
    XELOGE("MetalShader: DXIL to Metal conversion failed: {}",
           msc_result.error_message);
    dump_msc_failure("msc_convert_failed");
    return false;
  }
  function_name_ = msc_result.function_name;
  metallib_data_ = std::move(msc_result.metallib_data);
  XELOGD("MetalShader: Converted {} bytes DXIL to {} bytes MetalLib",
         dxil_data_.size(), metallib_data_.size());

  // Debug: Dump shader artifacts (DXBC, DXIL, MetalLib) to files when enabled.
  static int shader_dump_counter = 0;
  if (!cvars::dump_shaders.empty()) {
    std::filesystem::path base_dir = cvars::dump_shaders / "metal_shaders";

    char filename[128];
    const char* type_str =
        (shader().type() == xenos::ShaderType::kVertex) ? "vs" : "ps";
    int counter = shader_dump_counter++;

    // Dump DXBC (translated binary from DXBC translator)
    const auto& dxbc_data = translated_binary();
    if (!dxbc_data.empty()) {
      snprintf(filename, sizeof(filename), "shader_%d_%s.dxbc", counter,
               type_str);
      std::filesystem::path dxbc_path = base_dir / filename;
      xe::filesystem::CreateParentFolder(dxbc_path);
      FILE* f = xe::filesystem::OpenFile(dxbc_path, "wb");
      if (f) {
        fwrite(dxbc_data.data(), 1, dxbc_data.size(), f);
        fclose(f);
      }
    }

    // Dump DXIL
    if (!dxil_data_.empty()) {
      snprintf(filename, sizeof(filename), "shader_%d_%s.dxil", counter,
               type_str);
      std::filesystem::path dxil_path = base_dir / filename;
      xe::filesystem::CreateParentFolder(dxil_path);
      FILE* f = xe::filesystem::OpenFile(dxil_path, "wb");
      if (f) {
        fwrite(dxil_data_.data(), 1, dxil_data_.size(), f);
        fclose(f);
      }
    }

    // Dump MetalLib
    if (!metallib_data_.empty()) {
      snprintf(filename, sizeof(filename), "shader_%d_%s.metallib", counter,
               type_str);
      std::filesystem::path metallib_path = base_dir / filename;
      xe::filesystem::CreateParentFolder(metallib_path);
      FILE* f = xe::filesystem::OpenFile(metallib_path, "wb");
      if (f) {
        fwrite(metallib_data_.data(), 1, metallib_data_.size(), f);
        fclose(f);
      }
    }
  }

  // Step 3: Create Metal library from the metallib data
  NS::Error* error = nullptr;
  dispatch_data_t data =
      dispatch_data_create(metallib_data_.data(), metallib_data_.size(),
                           nullptr, DISPATCH_DATA_DESTRUCTOR_NONE);

  metal_library_ = device->newLibrary(data, &error);
  dispatch_release(data);

  if (!metal_library_) {
    if (error) {
      XELOGE("MetalShader: Failed to create Metal library: {}",
             error->localizedDescription()->utf8String());
    } else {
      XELOGE("MetalShader: Failed to create Metal library (unknown error)");
    }
    return false;
  }

  // Step 4: Get the main function from the library
  // MSC generates functions with specific names based on shader type
  NS::String* function_name = NS::String::string(
      msc_result.function_name.c_str(), NS::UTF8StringEncoding);

  metal_function_ = metal_library_->newFunction(function_name);

  if (!metal_function_) {
    // Try alternative function names
    const char* alt_names[] = {"main0", "main", "vertexMain", "fragmentMain"};
    for (const char* alt_name : alt_names) {
      NS::String* alt_func_name =
          NS::String::string(alt_name, NS::UTF8StringEncoding);
      metal_function_ = metal_library_->newFunction(alt_func_name);
      if (metal_function_) {
        XELOGD("MetalShader: Found function with alternative name: {}",
               alt_name);
        break;
      }
    }
  }

  if (!metal_function_) {
    // List available functions for debugging
    NS::Array* function_names = metal_library_->functionNames();
    XELOGE("MetalShader: Could not find shader function. Available functions:");
    for (NS::UInteger i = 0; i < function_names->count(); i++) {
      NS::String* name = static_cast<NS::String*>(function_names->object(i));
      XELOGE("  - {}", name->utf8String());
    }
    return false;
  }

  if (cvars::metal_shader_disk_cache && g_metal_shader_cache &&
      g_metal_shader_cache->IsInitialized()) {
    g_metal_shader_cache->Store(shader_cache_key, function_name_,
                                metallib_data_.data(), metallib_data_.size());
  }

  return true;
}

Shader::Translation* MetalShader::CreateTranslationInstance(
    uint64_t modification) {
  return new MetalTranslation(*this, modification);
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
