/**
 *******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 *******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHADER_CACHE_H_
#define XENIA_GPU_METAL_METAL_SHADER_CACHE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xe {
namespace gpu {
namespace metal {

class MetalShaderCache {
 public:
  struct CachedMetallib {
    std::string function_name;
    std::vector<uint8_t> metallib_data;
  };

  struct CacheStats {
    size_t entry_count = 0;
    size_t total_bytes = 0;
    size_t memory_entry_count = 0;
    size_t memory_total_bytes = 0;
  };

  MetalShaderCache() = default;
  ~MetalShaderCache() = default;

  void Initialize(const std::filesystem::path& cache_dir);
  void Shutdown();

  bool IsInitialized() const { return initialized_; }
  std::filesystem::path cache_dir() const { return cache_dir_; }

  static uint64_t GetCacheKey(uint64_t ucode_hash, uint64_t modification,
                              uint32_t stage);

  bool Load(uint64_t cache_key, CachedMetallib* out);
  void Store(uint64_t cache_key, std::string_view function_name,
             const uint8_t* metallib_data, size_t metallib_size);

  CacheStats GetStats() const;

 private:
  struct MemoryEntry {
    std::string function_name;
    std::vector<uint8_t> metallib_data;
  };

  bool LoadFromDisk(uint64_t cache_key, CachedMetallib* out);
  bool StoreToDisk(uint64_t cache_key, const CachedMetallib& in);

  std::filesystem::path GetDiskPath(uint64_t cache_key) const;

  mutable std::mutex mutex_;
  bool initialized_ = false;
  std::filesystem::path cache_dir_;
  std::unordered_map<uint64_t, MemoryEntry> cache_;
};

extern std::unique_ptr<MetalShaderCache> g_metal_shader_cache;

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_SHADER_CACHE_H_
