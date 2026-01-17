/**
 *******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 *******************************************************************************
 */

#include "xenia/gpu/metal/metal_shader_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

#include "third_party/xxhash/xxhash.h"
#include "xenia/base/logging.h"

namespace xe {
namespace gpu {
namespace metal {

std::unique_ptr<MetalShaderCache> g_metal_shader_cache =
    std::make_unique<MetalShaderCache>();

namespace {

constexpr uint32_t kCacheFileMagic = 0x4D4C4358;  // 'XCLM'
constexpr uint32_t kCacheFileVersion = 1;

struct CacheFileHeader {
  uint32_t magic;
  uint32_t version;
  uint64_t cache_key;
  uint32_t function_name_length;
  uint32_t metallib_size;
};

static_assert(sizeof(CacheFileHeader) == 24, "Unexpected header packing.");

}  // namespace

void MetalShaderCache::Initialize(const std::filesystem::path& cache_dir) {
  std::lock_guard<std::mutex> lock(mutex_);
  Shutdown();
  cache_dir_ = cache_dir;
  std::error_code ec;
  std::filesystem::create_directories(cache_dir_, ec);
  if (ec) {
    XELOGW("MetalShaderCache: Failed to create cache directory {}: {}",
           cache_dir_.string(), ec.message());
    initialized_ = false;
    return;
  }
  initialized_ = true;
}

void MetalShaderCache::Shutdown() {
  cache_.clear();
  cache_dir_.clear();
  initialized_ = false;
}

uint64_t MetalShaderCache::GetCacheKey(uint64_t ucode_hash,
                                       uint64_t modification, uint32_t stage) {
  struct KeyData {
    uint64_t ucode_hash;
    uint64_t modification;
    uint32_t stage;
    uint32_t reserved;
  } key_data = {ucode_hash, modification, stage, 0};
  return XXH3_64bits(&key_data, sizeof(key_data));
}

MetalShaderCache::CacheStats MetalShaderCache::GetStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  CacheStats stats;
  stats.entry_count = cache_.size();
  stats.memory_entry_count = cache_.size();
  for (const auto& it : cache_) {
    stats.memory_total_bytes += it.second.metallib_data.size();
  }
  stats.total_bytes = stats.memory_total_bytes;
  return stats;
}

bool MetalShaderCache::Load(uint64_t cache_key, CachedMetallib* out) {
  if (!out) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return false;
    }
    auto it = cache_.find(cache_key);
    if (it != cache_.end()) {
      out->function_name = it->second.function_name;
      out->metallib_data = it->second.metallib_data;
      return true;
    }
  }

  CachedMetallib disk_entry;
  if (!LoadFromDisk(cache_key, &disk_entry)) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return false;
    }
    MemoryEntry mem;
    mem.function_name = disk_entry.function_name;
    mem.metallib_data = disk_entry.metallib_data;
    cache_.emplace(cache_key, std::move(mem));
  }

  *out = std::move(disk_entry);
  return true;
}

void MetalShaderCache::Store(uint64_t cache_key, std::string_view function_name,
                             const uint8_t* metallib_data,
                             size_t metallib_size) {
  if (!metallib_data || metallib_size == 0) {
    return;
  }

  CachedMetallib entry;
  entry.function_name.assign(function_name.data(), function_name.size());
  entry.metallib_data.resize(metallib_size);
  std::memcpy(entry.metallib_data.data(), metallib_data, metallib_size);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return;
    }
    MemoryEntry mem;
    mem.function_name = entry.function_name;
    mem.metallib_data = entry.metallib_data;
    cache_[cache_key] = std::move(mem);
  }

  StoreToDisk(cache_key, entry);
}

std::filesystem::path MetalShaderCache::GetDiskPath(uint64_t cache_key) const {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llX.metalshcache",
                static_cast<unsigned long long>(cache_key));
  return cache_dir_ / name;
}

bool MetalShaderCache::LoadFromDisk(uint64_t cache_key, CachedMetallib* out) {
  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return false;
    }
    path = GetDiskPath(cache_key);
  }

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }

  CacheFileHeader hdr = {};
  file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  if (!file || hdr.magic != kCacheFileMagic ||
      hdr.version != kCacheFileVersion || hdr.cache_key != cache_key) {
    return false;
  }

  if (hdr.function_name_length > 4096 ||
      hdr.metallib_size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }

  std::string fn;
  fn.resize(hdr.function_name_length);
  file.read(fn.data(), hdr.function_name_length);
  if (!file) {
    return false;
  }

  std::vector<uint8_t> data;
  data.resize(hdr.metallib_size);
  file.read(reinterpret_cast<char*>(data.data()), hdr.metallib_size);
  if (!file) {
    return false;
  }

  out->function_name = std::move(fn);
  out->metallib_data = std::move(data);
  return true;
}

bool MetalShaderCache::StoreToDisk(uint64_t cache_key,
                                   const CachedMetallib& in) {
  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return false;
    }
    path = GetDiskPath(cache_key);
  }

  std::filesystem::path tmp_path = path;
  tmp_path += ".tmp";

  std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    return false;
  }

  CacheFileHeader hdr = {};
  hdr.magic = kCacheFileMagic;
  hdr.version = kCacheFileVersion;
  hdr.cache_key = cache_key;
  hdr.function_name_length = static_cast<uint32_t>(in.function_name.size());
  hdr.metallib_size = static_cast<uint32_t>(in.metallib_data.size());
  file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
  file.write(in.function_name.data(), in.function_name.size());
  file.write(reinterpret_cast<const char*>(in.metallib_data.data()),
             in.metallib_data.size());
  file.close();

  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path, ec);
    return false;
  }
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
