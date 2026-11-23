/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SHADER_STORAGE_H_
#define XENIA_GPU_SHADER_STORAGE_H_

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// Shader storage file format (.xsh) - shared between D3D12 and Vulkan backends.
// Stores guest shader microcode (Xbox 360 bytecode) for persistent caching.

// File magic: 'XESH'
constexpr fourcc_t kShaderStorageMagic = make_fourcc_le("XESH");

// Header for each stored shader entry.
XEPACKEDSTRUCT(ShaderStoredHeader, {
  uint64_t ucode_data_hash;

  uint32_t ucode_dword_count : 31;
  xenos::ShaderType type : 1;

  // Increment when the format changes in an incompatible way.
  static constexpr uint32_t kVersion = 0x20201219;
});

// Pipeline storage file format (.xpso) - API-specific but with shared header.
// File magic: 'XEPS'
constexpr fourcc_t kPipelineStorageMagic = make_fourcc_le("XEPS");

// Storage directory structure:
//   <cache_root>/shaders/
//     shareable/           - Files shared between different machines/drivers
//       {TITLE_ID}.xsh     - Shader microcode (shared between D3D12/Vulkan)
//       {TITLE_ID}.*.xpso  - Pipeline descriptions (API-specific)
//     local/               - Files specific to this machine/driver
//       {TITLE_ID}.*.bin   - Driver-specific pipeline cache

inline std::filesystem::path GetShaderStorageRoot(
    const std::filesystem::path& cache_root) {
  return cache_root / "shaders";
}

inline std::filesystem::path GetShaderStorageShareableRoot(
    const std::filesystem::path& cache_root) {
  return GetShaderStorageRoot(cache_root) / "shareable";
}

inline std::filesystem::path GetShaderStorageLocalRoot(
    const std::filesystem::path& cache_root) {
  return GetShaderStorageRoot(cache_root) / "local";
}

inline std::filesystem::path GetShaderStorageFilePath(
    const std::filesystem::path& cache_root, uint32_t title_id) {
  return GetShaderStorageShareableRoot(cache_root) /
         fmt::format("{:08X}.xsh", title_id);
}

// Ensures shader storage directories exist. Returns true on success.
inline bool EnsureShaderStorageDirectoriesExist(
    const std::filesystem::path& cache_root) {
  std::error_code error_code;
  auto shareable_root = GetShaderStorageShareableRoot(cache_root);
  if (!std::filesystem::exists(shareable_root)) {
    if (!std::filesystem::create_directories(shareable_root, error_code)) {
      XELOGE(
          "Failed to create shader storage directory, persistent shader "
          "storage will be disabled: {}",
          xe::path_to_utf8(shareable_root));
      return false;
    }
  }
  return true;
}

// File header for shader storage (.xsh).
XEPACKEDSTRUCT(ShaderStorageFileHeader, {
  uint32_t magic;
  uint32_t version_swapped;
});

// Validates the shader storage file header. Returns true if valid.
inline bool ValidateShaderStorageHeader(FILE* file,
                                        ShaderStorageFileHeader& header_out) {
  if (!fread(&header_out, sizeof(header_out), 1, file)) {
    return false;
  }
  return header_out.magic == kShaderStorageMagic &&
         xe::byte_swap(header_out.version_swapped) ==
             ShaderStoredHeader::kVersion;
}

// Writes a new shader storage file header.
inline bool WriteShaderStorageHeader(FILE* file) {
  ShaderStorageFileHeader header;
  header.magic = kShaderStorageMagic;
  header.version_swapped = xe::byte_swap(ShaderStoredHeader::kVersion);
  return fwrite(&header, sizeof(header), 1, file) == 1;
}

// Reads shader entries from storage file and calls the callback for each.
// Returns the number of valid bytes read (for truncation on corruption).
// The callback signature is: bool(xenos::ShaderType type,
//                                  const uint32_t* ucode_dwords,
//                                  uint32_t ucode_dword_count,
//                                  uint64_t ucode_data_hash)
// Callback should return true to continue reading, false to stop.
template <typename Callback>
inline uint64_t ReadShaderEntries(FILE* file, Callback&& callback) {
  uint64_t valid_bytes = sizeof(ShaderStorageFileHeader);

  ShaderStoredHeader shader_header;
  std::vector<uint32_t> ucode_dwords;
  ucode_dwords.reserve(0xFFFF);

  while (true) {
    if (!fread(&shader_header, sizeof(shader_header), 1, file)) {
      break;
    }
    size_t ucode_byte_count =
        shader_header.ucode_dword_count * sizeof(uint32_t);
    ucode_dwords.resize(shader_header.ucode_dword_count);
    if (shader_header.ucode_dword_count &&
        !fread(ucode_dwords.data(), ucode_byte_count, 1, file)) {
      break;
    }
    uint64_t ucode_data_hash =
        XXH3_64bits(ucode_dwords.data(), ucode_byte_count);
    if (shader_header.ucode_data_hash != ucode_data_hash) {
      // Validation failed - corrupted entry.
      break;
    }
    valid_bytes += sizeof(shader_header) + ucode_byte_count;

    if (!callback(shader_header.type, ucode_dwords.data(),
                  shader_header.ucode_dword_count, ucode_data_hash)) {
      break;
    }
  }

  return valid_bytes;
}

// File header for pipeline storage (.xpso).
XEPACKEDSTRUCT(PipelineStorageFileHeader, {
  uint32_t magic;
  uint32_t magic_api;
  uint32_t version_swapped;
});

// Template class for shader and pipeline storage management.
// TPipelineStoredDescription is the API-specific pipeline description type.
template <typename TPipelineStoredDescription>
class ShaderStorageWriter {
 public:
  // Configuration for pipeline storage files.
  struct PipelineStorageConfig {
    std::string file_suffix;  // e.g., ".fsi.vk.xpso" or ".rov.d3d12.xpso"
    fourcc_t api_magic;       // e.g., 'VKPS' or 'DXRO'
    uint32_t version;         // Pipeline description version
  };

  // Callback for loading a shader from ucode data.
  // Return true to continue reading, false to stop.
  using ShaderLoadCallback =
      std::function<bool(xenos::ShaderType type, const uint32_t* ucode_dwords,
                         uint32_t ucode_dword_count, uint64_t ucode_data_hash)>;

  // Callback for translating shaders. Called with the set of
  // (ucode_hash, modification) pairs that need translation.
  // Implementation should handle parallel translation internally.
  using TranslateCallback = std::function<void(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed)>;

  ShaderStorageWriter() = default;
  ~ShaderStorageWriter() { ShutdownShaderStorage(); }

  // Non-copyable.
  ShaderStorageWriter(const ShaderStorageWriter&) = delete;
  ShaderStorageWriter& operator=(const ShaderStorageWriter&) = delete;

  // Opens files, loads shaders, triggers translation, starts write thread.
  // Caller must call ShutdownShaderStorage() first if re-initializing.
  bool InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id,
      const PipelineStorageConfig& pipeline_config,
      ShaderLoadCallback load_shader, TranslateCallback translate_shaders,
      std::vector<TPipelineStoredDescription>& pipeline_descriptions_out) {
    cache_root_ = cache_root;
    title_id_ = title_id;

    if (!EnsureShaderStorageDirectoriesExist(cache_root)) {
      return false;
    }

    auto shader_storage_shareable_root =
        GetShaderStorageShareableRoot(cache_root);
    ++storage_index_;

    // Open pipeline storage file.
    auto pipeline_storage_file_path =
        shader_storage_shareable_root /
        fmt::format("{:08X}{}", title_id, pipeline_config.file_suffix);
    pipeline_storage_file_ =
        xe::filesystem::OpenFile(pipeline_storage_file_path, "a+b");
    if (!pipeline_storage_file_) {
      XELOGE(
          "Failed to open the pipeline storage file for writing, persistent "
          "shader storage will be disabled: {}",
          xe::path_to_utf8(pipeline_storage_file_path));
      return false;
    }

    // Read pipeline descriptions.
    const uint32_t pipeline_storage_version_swapped =
        xe::byte_swap(pipeline_config.version);
    PipelineStorageFileHeader pipeline_header;
    if (fread(&pipeline_header, sizeof(pipeline_header), 1,
              pipeline_storage_file_) &&
        pipeline_header.magic == kPipelineStorageMagic &&
        pipeline_header.magic_api == pipeline_config.api_magic &&
        pipeline_header.version_swapped == pipeline_storage_version_swapped) {
      // Valid header, read pipeline descriptions.
      xe::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END);
      int64_t pipeline_storage_told_end =
          xe::filesystem::Tell(pipeline_storage_file_);
      size_t pipeline_storage_told_count =
          size_t(pipeline_storage_told_end >= int64_t(sizeof(pipeline_header))
                     ? (uint64_t(pipeline_storage_told_end) -
                        sizeof(pipeline_header)) /
                           sizeof(TPipelineStoredDescription)
                     : 0);
      if (pipeline_storage_told_count) {
        xe::filesystem::Seek(pipeline_storage_file_,
                             int64_t(sizeof(pipeline_header)), SEEK_SET);
        pipeline_descriptions_out.resize(pipeline_storage_told_count);
        pipeline_descriptions_out.resize(
            fread(pipeline_descriptions_out.data(),
                  sizeof(TPipelineStoredDescription),
                  pipeline_storage_told_count, pipeline_storage_file_));
        // Validate each description's hash.
        size_t valid_count = 0;
        for (size_t i = 0; i < pipeline_descriptions_out.size(); ++i) {
          const TPipelineStoredDescription& desc = pipeline_descriptions_out[i];
          if (XXH3_64bits(&desc.description, sizeof(desc.description)) !=
              desc.description_hash) {
            break;
          }
          ++valid_count;
        }
        pipeline_descriptions_out.resize(valid_count);
      }
      // Truncate to last valid description.
      xe::filesystem::TruncateStdioFile(
          pipeline_storage_file_,
          uint64_t(sizeof(pipeline_header) +
                   sizeof(TPipelineStoredDescription) *
                       pipeline_descriptions_out.size()));
    } else {
      // Write new header.
      xe::filesystem::TruncateStdioFile(pipeline_storage_file_, 0);
      pipeline_header.magic = kPipelineStorageMagic;
      pipeline_header.magic_api = pipeline_config.api_magic;
      pipeline_header.version_swapped = pipeline_storage_version_swapped;
      if (fwrite(&pipeline_header, sizeof(pipeline_header), 1,
                 pipeline_storage_file_) != 1) {
        XELOGE("Failed to write pipeline storage header");
        fclose(pipeline_storage_file_);
        pipeline_storage_file_ = nullptr;
        return false;
      }
    }

    // Open shader storage file.
    auto shader_storage_file_path =
        GetShaderStorageFilePath(cache_root, title_id);
    shader_storage_file_ =
        xe::filesystem::OpenFile(shader_storage_file_path, "a+b");
    if (!shader_storage_file_) {
      XELOGE(
          "Failed to open the guest shader storage file for writing, "
          "persistent shader storage will be disabled: {}",
          xe::path_to_utf8(shader_storage_file_path));
      fclose(pipeline_storage_file_);
      pipeline_storage_file_ = nullptr;
      return false;
    }

    // Load shaders from storage.
    size_t shaders_loaded = 0;
    ShaderStorageFileHeader shader_header;
    if (ValidateShaderStorageHeader(shader_storage_file_, shader_header)) {
      uint64_t shader_storage_valid_bytes = ReadShaderEntries(
          shader_storage_file_,
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
              uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            if (load_shader(type, ucode_dwords, ucode_dword_count,
                            ucode_data_hash)) {
              ++shaders_loaded;
              return true;
            }
            return false;
          });
      xe::filesystem::TruncateStdioFile(shader_storage_file_,
                                        shader_storage_valid_bytes);
    } else {
      // Write new header.
      xe::filesystem::TruncateStdioFile(shader_storage_file_, 0);
      if (!WriteShaderStorageHeader(shader_storage_file_)) {
        XELOGE("Failed to write shader storage header");
        fclose(shader_storage_file_);
        shader_storage_file_ = nullptr;
        fclose(pipeline_storage_file_);
        pipeline_storage_file_ = nullptr;
        return false;
      }
    }
    XELOGI("Loaded {} shaders from storage", shaders_loaded);

    // Collect shader translations needed from pipeline descriptions.
    std::set<std::pair<uint64_t, uint64_t>> translations_needed;
    for (const TPipelineStoredDescription& desc : pipeline_descriptions_out) {
      translations_needed.emplace(desc.description.vertex_shader_hash,
                                  desc.description.vertex_shader_modification);
      if (desc.description.pixel_shader_hash) {
        translations_needed.emplace(desc.description.pixel_shader_hash,
                                    desc.description.pixel_shader_modification);
      }
    }
    XELOGI("Loaded {} pipeline descriptions, {} shader translations needed",
           pipeline_descriptions_out.size(), translations_needed.size());

    // Translate shaders (callback handles parallel translation).
    if (!translations_needed.empty() && translate_shaders) {
      translate_shaders(translations_needed);
    }

    // Start the write thread.
    storage_write_thread_shutdown_ = false;
    storage_write_thread_ =
        xe::threading::Thread::Create({}, [this]() { WriteThread(); });
    storage_write_thread_->set_name("Shader Storage Writer");

    return true;
  }

  // Shutdown storage: stops write thread, closes files.
  void ShutdownShaderStorage() {
    if (storage_write_thread_) {
      {
        std::lock_guard<std::mutex> lock(storage_write_request_lock_);
        storage_write_thread_shutdown_ = true;
      }
      storage_write_request_cond_.notify_one();
      xe::threading::Wait(storage_write_thread_.get(), false);
      storage_write_thread_.reset();
    }

    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_shader_queue_.clear();
      storage_write_pipeline_queue_.clear();
      storage_write_flush_shaders_ = false;
      storage_write_flush_pipelines_ = false;
    }

    if (pipeline_storage_file_) {
      fclose(pipeline_storage_file_);
      pipeline_storage_file_ = nullptr;
    }
    if (shader_storage_file_) {
      fclose(shader_storage_file_);
      shader_storage_file_ = nullptr;
    }

    cache_root_.clear();
    title_id_ = 0;
  }

  uint32_t storage_index() const { return storage_index_; }
  FILE* shader_storage_file() const { return shader_storage_file_; }
  FILE* pipeline_storage_file() const { return pipeline_storage_file_; }
  bool is_active() const { return shader_storage_file_ != nullptr; }
  const std::filesystem::path& cache_root() const { return cache_root_; }
  uint32_t title_id() const { return title_id_; }

  void QueueShaderWrite(const Shader* shader) {
    if (!shader_storage_file_) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_shader_queue_.push_back(shader);
    }
    storage_write_request_cond_.notify_one();
  }

  void QueuePipelineWrite(const TPipelineStoredDescription& description) {
    if (!pipeline_storage_file_) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_pipeline_queue_.push_back(description);
    }
    storage_write_request_cond_.notify_one();
  }

  void RequestFlush(bool flush_shaders, bool flush_pipelines) {
    bool need_notify = false;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      if (flush_shaders) {
        storage_write_flush_shaders_ = true;
        need_notify = true;
      }
      if (flush_pipelines) {
        storage_write_flush_pipelines_ = true;
        need_notify = true;
      }
    }
    if (need_notify) {
      storage_write_request_cond_.notify_one();
    }
  }

 private:
  void WriteThread() {
    ShaderStoredHeader shader_header;
    std::memset(&shader_header, 0, sizeof(shader_header));

    std::vector<uint32_t> ucode_guest_endian;
    ucode_guest_endian.reserve(0xFFFF);

    bool flush_shaders = false;
    bool flush_pipelines = false;

    while (true) {
      if (flush_shaders) {
        flush_shaders = false;
        assert_not_null(shader_storage_file_);
        fflush(shader_storage_file_);
      }
      if (flush_pipelines) {
        flush_pipelines = false;
        assert_not_null(pipeline_storage_file_);
        fflush(pipeline_storage_file_);
      }

      const Shader* shader = nullptr;
      TPipelineStoredDescription pipeline_description;
      bool write_pipeline = false;
      {
        std::unique_lock<std::mutex> lock(storage_write_request_lock_);
        if (storage_write_thread_shutdown_) {
          return;
        }
        if (!storage_write_shader_queue_.empty()) {
          shader = storage_write_shader_queue_.front();
          storage_write_shader_queue_.pop_front();
        } else if (storage_write_flush_shaders_) {
          storage_write_flush_shaders_ = false;
          flush_shaders = true;
        }
        if (!storage_write_pipeline_queue_.empty()) {
          std::memcpy(&pipeline_description,
                      &storage_write_pipeline_queue_.front(),
                      sizeof(pipeline_description));
          storage_write_pipeline_queue_.pop_front();
          write_pipeline = true;
        } else if (storage_write_flush_pipelines_) {
          storage_write_flush_pipelines_ = false;
          flush_pipelines = true;
        }
        if (!shader && !write_pipeline && !flush_shaders && !flush_pipelines) {
          storage_write_request_cond_.wait(lock);
          continue;
        }
      }

      if (shader) {
        shader_header.ucode_data_hash = shader->ucode_data_hash();
        shader_header.ucode_dword_count = shader->ucode_dword_count();
        shader_header.type = shader->type();
        assert_not_null(shader_storage_file_);
        if (fwrite(&shader_header, sizeof(shader_header), 1,
                   shader_storage_file_) != 1) {
          XELOGE("Failed to write shader header to storage");
        } else if (shader_header.ucode_dword_count) {
          ucode_guest_endian.resize(shader_header.ucode_dword_count);
          // Need to swap because the hash is calculated for the shader with
          // guest endianness.
          xe::copy_and_swap(ucode_guest_endian.data(), shader->ucode_dwords(),
                            shader_header.ucode_dword_count);
          if (fwrite(ucode_guest_endian.data(),
                     shader_header.ucode_dword_count * sizeof(uint32_t), 1,
                     shader_storage_file_) != 1) {
            XELOGE("Failed to write shader ucode to storage");
          }
        }
      }

      if (write_pipeline) {
        assert_not_null(pipeline_storage_file_);
        if (fwrite(&pipeline_description, sizeof(pipeline_description), 1,
                   pipeline_storage_file_) != 1) {
          XELOGE("Failed to write pipeline description to storage");
        }
      }
    }
  }

  std::filesystem::path cache_root_;
  uint32_t title_id_ = 0;
  uint32_t storage_index_ = 0;

  FILE* shader_storage_file_ = nullptr;
  FILE* pipeline_storage_file_ = nullptr;

  std::unique_ptr<xe::threading::Thread> storage_write_thread_;
  std::mutex storage_write_request_lock_;
  std::condition_variable storage_write_request_cond_;
  std::deque<const Shader*> storage_write_shader_queue_;
  std::deque<TPipelineStoredDescription> storage_write_pipeline_queue_;
  bool storage_write_flush_shaders_ = false;
  bool storage_write_flush_pipelines_ = false;
  bool storage_write_thread_shutdown_ = false;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SHADER_STORAGE_H_
