/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_heap_pool.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "third_party/stb/stb_image_write.h"
#include "xenia/base/assert.h"
#include "xenia/base/autorelease_pool_mac.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_shared_memory.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_128bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_128bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_16bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_16bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_32bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_32bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_64bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_64bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_8bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_8bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_bgrg8_rgb8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_bgrg8_rgbg8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_ctx1_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_depth_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_depth_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_depth_unorm_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_depth_unorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxn_rg8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt1_rgba8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt3_rgba8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt3a_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt3aas1111_argb4_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt3aas1111_bgra4_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt5_rgba8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt5a_r8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_gbgr8_grgb8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_gbgr8_rgb8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r10g11b11_rgba16_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r10g11b11_rgba16_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r10g11b11_rgba16_snorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r11g11b10_rgba16_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r11g11b10_rgba16_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r11g11b10_rgba16_snorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r16_snorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r16_snorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r16_unorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r16_unorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r4g4b4a4_a4r4g4b4_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r4g4b4a4_a4r4g4b4_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r4g4b4a4_b4g4r4a4_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r4g4b4a4_b4g4r4a4_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g5b5a1_b5g5r5a1_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g6b5_b5g6r5_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g6b5_b5g6r5_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rg16_snorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rg16_snorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rg16_unorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rg16_unorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rgba16_snorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rgba16_snorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rgba16_unorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rgba16_unorm_float_scaled_cs.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(metal_force_bc_decompress, false,
            "Force BC1/2/3/5/DXN decompression to RGBA8/RG8 (debug).", "Metal");
DEFINE_bool(metal_texture_cache_use_private, true,
            "Use MTLStorageModePrivate for Metal texture cache textures when "
            "GPU upload paths support it.",
            "Metal");
DEFINE_bool(metal_texture_upload_via_blit, true,
            "Upload textures via staging buffers and GPU blit copies instead "
            "of CPU replaceRegion.",
            "Metal");

DECLARE_bool(metal_use_heaps);
DECLARE_int32(metal_heap_min_bytes);

namespace xe {
namespace gpu {
namespace metal {
namespace {

#if XE_PLATFORM_IOS
constexpr uint64_t kUploadBufferPoolMaxBytes = 128ull * 1024ull * 1024ull;
constexpr uint64_t kScaledResolveRetiredMaxBytes = 64ull * 1024ull * 1024ull;
#else
constexpr uint64_t kUploadBufferPoolMaxBytes = 512ull * 1024ull * 1024ull;
constexpr uint64_t kScaledResolveRetiredMaxBytes = 256ull * 1024ull * 1024ull;
#endif

struct MetalLoadConstants {
  uint32_t is_tiled_3d_endian_scale;
  uint32_t guest_offset;
  uint32_t guest_pitch_aligned;
  uint32_t guest_z_stride_block_rows_aligned;
  uint32_t size_blocks[3];
  uint32_t padding0;  // Pad to 16-byte boundary for uint3 in MSL.
  uint32_t host_offset;
  uint32_t host_pitch;
  uint32_t height_texels;
  uint32_t padding1[5];  // Pad to 64 bytes to match HLSL CB size.
};
static_assert(sizeof(MetalLoadConstants) == 64);

class ScopedAutoreleasePool {
 public:
  ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}
  ~ScopedAutoreleasePool() {
    if (pool_) {
      pool_->release();
    }
  }

  ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
  ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

 private:
  NS::AutoreleasePool* pool_;
};

bool SupportsPixelFormat(MTL::Device* device, MTL::PixelFormat format) {
  if (!device || format == MTL::PixelFormatInvalid) {
    return false;
  }
  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  descriptor->setTextureType(MTL::TextureType2D);
  descriptor->setPixelFormat(format);
  descriptor->setWidth(1);
  descriptor->setHeight(1);
  descriptor->setDepth(1);
  descriptor->setArrayLength(1);
  descriptor->setMipmapLevelCount(1);
  descriptor->setSampleCount(1);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(MTL::StorageModeShared);
  MTL::Texture* texture = device->newTexture(descriptor);
  descriptor->release();
  if (texture) {
    texture->release();
  }
  return texture != nullptr;
}

uint32_t GetEstimatedBytesPerPixel(MTL::PixelFormat format) {
  switch (format) {
    case MTL::PixelFormatRGBA8Unorm:
    case MTL::PixelFormatRGBA8Unorm_sRGB:
    case MTL::PixelFormatBGRA8Unorm:
    case MTL::PixelFormatBGRA8Unorm_sRGB:
    case MTL::PixelFormatR32Float:
    case MTL::PixelFormatR32Uint:
    case MTL::PixelFormatR32Sint:
    case MTL::PixelFormatDepth32Float:
    case MTL::PixelFormatDepth24Unorm_Stencil8:
    case MTL::PixelFormatX32_Stencil8:
      return 4;
    case MTL::PixelFormatRG16Float:
    case MTL::PixelFormatRG16Uint:
    case MTL::PixelFormatRG16Sint:
      return 4;
    case MTL::PixelFormatRGBA16Float:
    case MTL::PixelFormatRGBA16Uint:
    case MTL::PixelFormatRGBA16Sint:
    case MTL::PixelFormatRG32Float:
    case MTL::PixelFormatRG32Uint:
    case MTL::PixelFormatRG32Sint:
    case MTL::PixelFormatDepth32Float_Stencil8:
      return 8;
    case MTL::PixelFormatR16Float:
    case MTL::PixelFormatR16Uint:
    case MTL::PixelFormatR16Sint:
    case MTL::PixelFormatDepth16Unorm:
      return 2;
    default:
      return 4;
  }
}

uint64_t EstimateTextureBytes(MTL::Texture* texture) {
  if (!texture) {
    return 0;
  }

  const uint32_t bytes_per_pixel =
      GetEstimatedBytesPerPixel(texture->pixelFormat());
  const uint32_t sample_count =
      std::max<uint32_t>(1, static_cast<uint32_t>(texture->sampleCount()));
  const uint32_t mip_count =
      std::max<uint32_t>(1, static_cast<uint32_t>(texture->mipmapLevelCount()));
  const uint32_t array_length =
      std::max<uint32_t>(1, static_cast<uint32_t>(texture->arrayLength()));

  uint64_t total = 0;
  for (uint32_t level = 0; level < mip_count; ++level) {
    uint32_t width =
        std::max<uint32_t>(1, static_cast<uint32_t>(texture->width() >> level));
    uint32_t height = std::max<uint32_t>(
        1, static_cast<uint32_t>(texture->height() >> level));
    uint32_t depth =
        std::max<uint32_t>(1, static_cast<uint32_t>(texture->depth() >> level));
    uint64_t level_bytes = uint64_t(width) * uint64_t(height) *
                           uint64_t(depth) * bytes_per_pixel * sample_count;
    total += level_bytes;
  }

  return total * array_length;
}

bool AreDimensionsCompatible(xenos::FetchOpDimension shader_dimension,
                             xenos::DataDimension texture_dimension) {
  switch (shader_dimension) {
    case xenos::FetchOpDimension::k1D:
    case xenos::FetchOpDimension::k2D:
      return texture_dimension == xenos::DataDimension::k1D ||
             texture_dimension == xenos::DataDimension::k2DOrStacked ||
             texture_dimension == xenos::DataDimension::k3D;
    case xenos::FetchOpDimension::k3DOrStacked:
      return texture_dimension == xenos::DataDimension::k3D;
    case xenos::FetchOpDimension::kCube:
      return texture_dimension == xenos::DataDimension::kCube;
    default:
      return false;
  }
}

MTL::TextureSwizzleChannels ToMetalTextureSwizzle(uint32_t xenos_swizzle) {
  MTL::TextureSwizzleChannels swizzle;
  // Xenos: R=0, G=1, B=2, A=3, 0=4, 1=5
  // Metal: Zero=0, One=1, Red=2, Green=3, Blue=4, Alpha=5
  static const MTL::TextureSwizzle kMap[] = {
      MTL::TextureSwizzleRed,    // 0
      MTL::TextureSwizzleGreen,  // 1
      MTL::TextureSwizzleBlue,   // 2
      MTL::TextureSwizzleAlpha,  // 3
      MTL::TextureSwizzleZero,   // 4
      MTL::TextureSwizzleOne,    // 5
      MTL::TextureSwizzleZero,   // 6 (Unused)
      MTL::TextureSwizzleZero,   // 7 (Unused)
  };
  swizzle.red = kMap[(xenos_swizzle >> 0) & 0x7];
  swizzle.green = kMap[(xenos_swizzle >> 3) & 0x7];
  swizzle.blue = kMap[(xenos_swizzle >> 6) & 0x7];
  swizzle.alpha = kMap[(xenos_swizzle >> 9) & 0x7];
  return swizzle;
}

}  // namespace

class MetalTextureCache::UploadBufferPool
    : public std::enable_shared_from_this<UploadBufferPool> {
 public:
  explicit UploadBufferPool(MTL::Device* device, uint64_t max_pooled_bytes)
      : device_(device), max_pooled_bytes_(max_pooled_bytes) {}

  MTL::Buffer* Acquire(size_t size) {
    if (!device_) {
      return nullptr;
    }
    size = xe::round_up(size, size_t(256));
    bool can_pool = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++usage_tick_;
      size_t best_index = entries_.size();
      size_t best_size = std::numeric_limits<size_t>::max();
      for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& entry = entries_[i];
        if (entry.in_use || entry.size < size) {
          continue;
        }
        if (entry.size < best_size) {
          best_index = i;
          best_size = entry.size;
        }
      }
      if (best_index < entries_.size()) {
        entries_[best_index].in_use = true;
        entries_[best_index].last_used_tick = usage_tick_;
        return entries_[best_index].buffer;
      }
      can_pool = pooled_bytes_ + size <= max_pooled_bytes_;
    }
    MTL::Buffer* buffer =
        device_->newBuffer(size, MTL::ResourceStorageModeShared);
    if (!buffer) {
      return nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++usage_tick_;
      if (can_pool && pooled_bytes_ + size <= max_pooled_bytes_) {
        pooled_bytes_ += size;
        entries_.push_back({buffer, size, true, true, usage_tick_});
        return buffer;
      }
      ++transient_allocations_;
    }
    return buffer;
  }

  void ReleaseImmediate(MTL::Buffer* buffer) {
    if (!buffer) {
      return;
    }
    bool release_transient = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++usage_tick_;
      for (Entry& entry : entries_) {
        if (entry.buffer == buffer) {
          entry.in_use = false;
          entry.last_used_tick = usage_tick_;
          release_transient = false;
          break;
        }
      }
    }
    if (release_transient) {
      buffer->release();
    }
  }

  void ReleaseAfter(MTL::CommandBuffer* cmd, MTL::Buffer* buffer) {
    if (!buffer) {
      return;
    }
    if (!cmd) {
      ReleaseImmediate(buffer);
      return;
    }
    bool add_handler = false;
    {
      std::lock_guard<std::mutex> lock(PendingReleasesMutex());
      auto& pending = PendingReleasesMap()[cmd];
      add_handler = pending.empty();
      pending.push_back({shared_from_this(), buffer});
    }
    if (add_handler) {
      cmd->addCompletedHandler(^(MTL::CommandBuffer* completed_cmd) {
        UploadBufferPool::HandleCommandBufferCompleted(completed_cmd);
      });
    }
  }

  void Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& entry : entries_) {
      if (entry.buffer) {
        entry.buffer->release();
        entry.buffer = nullptr;
      }
    }
    pooled_bytes_ = 0;
    entries_.clear();
  }

  size_t GetEntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

  uint64_t GetTotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pooled_bytes_;
  }

 private:
  struct Entry {
    MTL::Buffer* buffer = nullptr;
    size_t size = 0;
    bool pooled = false;
    bool in_use = false;
    uint64_t last_used_tick = 0;
  };

  struct PendingRelease {
    std::shared_ptr<UploadBufferPool> pool;
    MTL::Buffer* buffer = nullptr;
  };

  using PendingReleasesByCommandBuffer =
      std::unordered_map<MTL::CommandBuffer*, std::vector<PendingRelease>>;

  static std::mutex& PendingReleasesMutex() {
    // Heap-allocated and intentionally leaked to avoid static destruction order
    // issues on macOS/POSIX where threads may outlive static destructors.
    static std::mutex* pending_releases_mutex = new std::mutex();
    return *pending_releases_mutex;
  }

  static PendingReleasesByCommandBuffer& PendingReleasesMap() {
    // Heap-allocated and intentionally leaked for the same reason as the mutex.
    static PendingReleasesByCommandBuffer* pending_releases =
        new PendingReleasesByCommandBuffer();
    return *pending_releases;
  }

  static void HandleCommandBufferCompleted(MTL::CommandBuffer* cmd);

  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  MTL::Device* device_ = nullptr;
  uint64_t max_pooled_bytes_ = 0;
  uint64_t pooled_bytes_ = 0;
  uint64_t usage_tick_ = 0;
  uint64_t transient_allocations_ = 0;
};

void MetalTextureCache::UploadBufferPool::HandleCommandBufferCompleted(
    MTL::CommandBuffer* cmd) {
  std::vector<PendingRelease> releases;
  {
    std::lock_guard<std::mutex> lock(PendingReleasesMutex());
    auto& pending_releases = PendingReleasesMap();
    auto it = pending_releases.find(cmd);
    if (it == pending_releases.end()) {
      return;
    }
    releases = std::move(it->second);
    pending_releases.erase(it);
  }
  for (auto& release : releases) {
    release.pool->ReleaseImmediate(release.buffer);
  }
}

MetalTextureCache::MetalTextureCache(MetalCommandProcessor* command_processor,
                                     const RegisterFile& register_file,
                                     MetalSharedMemory& shared_memory,
                                     uint32_t draw_resolution_scale_x,
                                     uint32_t draw_resolution_scale_y)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x,
                   draw_resolution_scale_y),
      command_processor_(command_processor) {}

MetalTextureCache::~MetalTextureCache() { Shutdown(); }

MTL::StorageMode MetalTextureCache::GetCacheTextureStorageMode() const {
  if (!::cvars::metal_texture_upload_via_blit ||
      !::cvars::metal_texture_cache_use_private) {
    return MTL::StorageModeShared;
  }
  return MTL::StorageModePrivate;
}

bool MetalTextureCache::ShouldUploadViaBlit() const {
  return ::cvars::metal_texture_upload_via_blit;
}

bool MetalTextureCache::CanUseCurrentCommandBufferForTextureUploads() const {
  if (!ShouldUploadViaBlit() || !command_processor_) {
    return false;
  }
  if (!command_processor_->GetCurrentCommandBuffer()) {
    return false;
  }
  return !command_processor_->HasActiveRenderEncoder();
}

void MetalTextureCache::BeginUploadCommandBufferBatch() {
  ++upload_batch_depth_;
  if (upload_batch_depth_ != 1) {
    return;
  }
  if (!ShouldUploadViaBlit() || !command_processor_) {
    return;
  }
  // Avoid cross-command-buffer upload batching while a draw/copy command
  // buffer is already active in the command processor. Keeping upload work on
  // a separate command buffer in that state can reorder with in-flight render
  // setup and lead to startup rendering regressions.
  if (command_processor_->GetCurrentCommandBuffer()) {
    return;
  }
  MTL::CommandQueue* queue = command_processor_->GetMetalCommandQueue();
  if (!queue) {
    return;
  }
  MTL::CommandBuffer* cmd = queue->commandBuffer();
  if (!cmd) {
    return;
  }
  cmd->retain();
  cmd->setLabel(
      NS::String::string("XeniaTextureUploadBatch", NS::UTF8StringEncoding));
  upload_batch_command_buffer_ = cmd;
  upload_batch_command_buffer_has_work_ = false;
}

void MetalTextureCache::EndUploadCommandBufferBatch() {
  if (!upload_batch_depth_) {
    return;
  }
  --upload_batch_depth_;
  if (upload_batch_depth_ != 0) {
    return;
  }
  MTL::CommandBuffer* cmd = upload_batch_command_buffer_;
  upload_batch_command_buffer_ = nullptr;
  bool has_work = upload_batch_command_buffer_has_work_;
  upload_batch_command_buffer_has_work_ = false;
  if (!cmd) {
    return;
  }
  if (!has_work) {
    cmd->release();
    return;
  }
  cmd->addCompletedHandler(^(MTL::CommandBuffer* completed_cmd) {
    completed_cmd->release();
  });
  cmd->commit();
}

void MetalTextureCache::AbortUploadCommandBufferBatch(bool commit_if_has_work) {
  MTL::CommandBuffer* cmd = upload_batch_command_buffer_;
  upload_batch_command_buffer_ = nullptr;
  bool has_work = upload_batch_command_buffer_has_work_;
  upload_batch_command_buffer_has_work_ = false;
  if (!cmd) {
    return;
  }
  if (!has_work || !commit_if_has_work) {
    cmd->release();
    return;
  }
  cmd->addCompletedHandler(^(MTL::CommandBuffer* completed_cmd) {
    completed_cmd->release();
  });
  cmd->commit();
}

bool MetalTextureCache::IsDecompressionNeededForKey(TextureKey key) const {
  switch (key.format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXN: {
      if (::cvars::metal_force_bc_decompress) {
        return true;
      }
      // BC support is GPU-family dependent on iOS. Creating BC textures on a
      // device that doesn't support them may trip Metal validation and abort.
      if (!supports_bc_texture_compression_) {
        return true;
      }
      const FormatInfo* format_info = FormatInfo::Get(key.format);
      if (!format_info) {
        return false;
      }
      if (!(key.GetWidth() & (format_info->block_width - 1)) &&
          !(key.GetHeight() & (format_info->block_height - 1))) {
        return false;
      }
      return true;
    }
    case xenos::TextureFormat::k_CTX1:
      // CTX1 must be decompressed (no hardware support on Metal).
      return true;
    default:
      return false;
  }
}

TextureCache::LoadShaderIndex MetalTextureCache::GetLoadShaderIndexForKey(
    TextureKey key) const {
  bool decompress = IsDecompressionNeededForKey(key);
  switch (key.format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
      return kLoadShaderIndex8bpb;
    case xenos::TextureFormat::k_8_8:
      return kLoadShaderIndex16bpb;
    case xenos::TextureFormat::k_1_5_5_5:
      return kLoadShaderIndexR5G5B5A1ToB5G5R5A1;
    case xenos::TextureFormat::k_5_6_5:
      return kLoadShaderIndexR5G6B5ToB5G6R5;
    case xenos::TextureFormat::k_6_5_5:
      return kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle;
    case xenos::TextureFormat::k_8_8_8_8:
      return kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_2_10_10_10:
      return kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_4_4_4_4:
      return kLoadShaderIndexRGBA4ToBGRA4;
    case xenos::TextureFormat::k_10_11_11:
      return key.signed_separate ? kLoadShaderIndexR11G11B10ToRGBA16SNorm
                                 : kLoadShaderIndexR11G11B10ToRGBA16;
    case xenos::TextureFormat::k_11_11_10:
      return key.signed_separate ? kLoadShaderIndexR10G11B11ToRGBA16SNorm
                                 : kLoadShaderIndexR10G11B11ToRGBA16;

    case xenos::TextureFormat::k_DXT1:
      return decompress ? kLoadShaderIndexDXT1ToRGBA8 : kLoadShaderIndex64bpb;
    case xenos::TextureFormat::k_DXT2_3:
      return decompress ? kLoadShaderIndexDXT3ToRGBA8 : kLoadShaderIndex128bpb;
    case xenos::TextureFormat::k_DXT4_5:
      return decompress ? kLoadShaderIndexDXT5ToRGBA8 : kLoadShaderIndex128bpb;
    case xenos::TextureFormat::k_DXN:
      return decompress ? kLoadShaderIndexDXNToRG8 : kLoadShaderIndex128bpb;
    case xenos::TextureFormat::k_DXT3A:
      return kLoadShaderIndexDXT3A;
    case xenos::TextureFormat::k_DXT5A:
      return kLoadShaderIndexDXT5AToR8;
    case xenos::TextureFormat::k_DXT3A_AS_1_1_1_1:
      return kLoadShaderIndexDXT3AAs1111ToBGRA4;
    case xenos::TextureFormat::k_CTX1:
      return kLoadShaderIndexCTX1;

    case xenos::TextureFormat::k_24_8:
      return kLoadShaderIndexDepthUnorm;
    case xenos::TextureFormat::k_24_8_FLOAT:
      return kLoadShaderIndexDepthFloat;

    case xenos::TextureFormat::k_16:
      if (key.signed_separate) {
        return r16_selection_.signed_uses_float
                   ? kLoadShaderIndexR16SNormToFloat
                   : kLoadShaderIndex16bpb;
      }
      return r16_selection_.unsigned_uses_float
                 ? kLoadShaderIndexR16UNormToFloat
                 : kLoadShaderIndex16bpb;
    case xenos::TextureFormat::k_16_EXPAND:
    case xenos::TextureFormat::k_16_FLOAT:
      return kLoadShaderIndex16bpb;
    case xenos::TextureFormat::k_16_16:
      if (key.signed_separate) {
        return rg16_selection_.signed_uses_float
                   ? kLoadShaderIndexRG16SNormToFloat
                   : kLoadShaderIndex32bpb;
      }
      return rg16_selection_.unsigned_uses_float
                 ? kLoadShaderIndexRG16UNormToFloat
                 : kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_FLOAT:
      return kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_16_16_16_16:
      if (key.signed_separate) {
        return rgba16_selection_.signed_uses_float
                   ? kLoadShaderIndexRGBA16SNormToFloat
                   : kLoadShaderIndex64bpb;
      }
      return rgba16_selection_.unsigned_uses_float
                 ? kLoadShaderIndexRGBA16UNormToFloat
                 : kLoadShaderIndex64bpb;
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return kLoadShaderIndex64bpb;

    case xenos::TextureFormat::k_32:
    case xenos::TextureFormat::k_32_FLOAT:
      return kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_32_32:
    case xenos::TextureFormat::k_32_32_FLOAT:
      return kLoadShaderIndex64bpb;
    case xenos::TextureFormat::k_32_32_32_32:
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return kLoadShaderIndex128bpb;

    case xenos::TextureFormat::k_8_B:
      return kLoadShaderIndex8bpb;
    case xenos::TextureFormat::k_8_8_8_8_A:
      return kLoadShaderIndex32bpb;

    default:
      return kLoadShaderIndexUnknown;
  }
}

MTL::PixelFormat MetalTextureCache::GetPixelFormatForKey(TextureKey key) const {
  bool decompress = IsDecompressionNeededForKey(key);
  switch (key.format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
      return MTL::PixelFormatR8Unorm;
    case xenos::TextureFormat::k_8_8:
      return MTL::PixelFormatRG8Unorm;
    case xenos::TextureFormat::k_1_5_5_5:
      return MTL::PixelFormatA1BGR5Unorm;
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_6_5_5:
      return MTL::PixelFormatB5G6R5Unorm;
    case xenos::TextureFormat::k_4_4_4_4:
      return MTL::PixelFormatABGR4Unorm;
    case xenos::TextureFormat::k_8_8_8_8:
      return MTL::PixelFormatRGBA8Unorm;
    case xenos::TextureFormat::k_2_10_10_10:
      return MTL::PixelFormatRGB10A2Unorm;
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
      return key.signed_separate ? MTL::PixelFormatRGBA16Snorm
                                 : MTL::PixelFormatRGBA16Unorm;

    case xenos::TextureFormat::k_16:
      if (key.signed_separate) {
        return r16_selection_.signed_uses_float ? MTL::PixelFormatR16Float
                                                : MTL::PixelFormatR16Snorm;
      }
      return r16_selection_.unsigned_uses_float ? MTL::PixelFormatR16Float
                                                : MTL::PixelFormatR16Unorm;
    case xenos::TextureFormat::k_16_16:
      if (key.signed_separate) {
        return rg16_selection_.signed_uses_float ? MTL::PixelFormatRG16Float
                                                 : MTL::PixelFormatRG16Snorm;
      }
      return rg16_selection_.unsigned_uses_float ? MTL::PixelFormatRG16Float
                                                 : MTL::PixelFormatRG16Unorm;
    case xenos::TextureFormat::k_16_16_16_16:
      if (key.signed_separate) {
        return rgba16_selection_.signed_uses_float
                   ? MTL::PixelFormatRGBA16Float
                   : MTL::PixelFormatRGBA16Snorm;
      }
      return rgba16_selection_.unsigned_uses_float
                 ? MTL::PixelFormatRGBA16Float
                 : MTL::PixelFormatRGBA16Unorm;
    case xenos::TextureFormat::k_16_EXPAND:
    case xenos::TextureFormat::k_16_FLOAT:
      return MTL::PixelFormatR16Float;
    case xenos::TextureFormat::k_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;

    case xenos::TextureFormat::k_DXT1:
      return decompress ? MTL::PixelFormatRGBA8Unorm : MTL::PixelFormatBC1_RGBA;
    case xenos::TextureFormat::k_DXT2_3:
      return decompress ? MTL::PixelFormatRGBA8Unorm : MTL::PixelFormatBC2_RGBA;
    case xenos::TextureFormat::k_DXT4_5:
      return decompress ? MTL::PixelFormatRGBA8Unorm : MTL::PixelFormatBC3_RGBA;
    case xenos::TextureFormat::k_DXN:
      return decompress ? MTL::PixelFormatRG8Unorm
                        : MTL::PixelFormatBC5_RGUnorm;
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_DXT5A:
      return MTL::PixelFormatR8Unorm;
    case xenos::TextureFormat::k_DXT3A_AS_1_1_1_1:
      return MTL::PixelFormatABGR4Unorm;
    case xenos::TextureFormat::k_CTX1:
      // CTX1 is always decoded via the texture load shader to RG8.
      return MTL::PixelFormatRG8Unorm;

    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      return MTL::PixelFormatR32Float;

    case xenos::TextureFormat::k_8_B:
      return MTL::PixelFormatR8Unorm;
    case xenos::TextureFormat::k_8_8_8_8_A:
      return MTL::PixelFormatRGBA8Unorm;

    case xenos::TextureFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return MTL::PixelFormatRGBA32Float;

    default:
      return MTL::PixelFormatInvalid;
  }
}

bool MetalTextureCache::TryGpuLoadTexture(Texture& texture, bool load_base,
                                          bool load_mips) {
  MetalTexture* metal_texture = static_cast<MetalTexture*>(&texture);
  if (!metal_texture || !metal_texture->metal_texture()) {
    return false;
  }

  const TextureKey& key = texture.key();
  bool texture_resolution_scaled =
      key.scaled_resolve && IsDrawResolutionScaled();
  uint32_t texture_resolution_scale_x =
      texture_resolution_scaled ? draw_resolution_scale_x() : 1;
  uint32_t texture_resolution_scale_y =
      texture_resolution_scaled ? draw_resolution_scale_y() : 1;
  uint32_t texture_resolution_scale_area =
      texture_resolution_scale_x * texture_resolution_scale_y;

  const texture_util::TextureGuestLayout& guest_layout = texture.guest_layout();
  xenos::DataDimension dimension = key.dimension;
  bool is_3d = dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || texture.force_load_3d_tiling();

  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;

  const FormatInfo* guest_format_info = FormatInfo::Get(key.format);
  if (!guest_format_info) {
    return false;
  }
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();

  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? key.mip_max_level : 0;
  if (level_first > level_last) {
    return false;
  }

  bool decompress = IsDecompressionNeededForKey(key);
  TextureCache::LoadShaderIndex load_shader = GetLoadShaderIndexForKey(key);
  if (load_shader == TextureCache::kLoadShaderIndexUnknown) {
    return false;
  }

  MTL::ComputePipelineState* pipeline =
      texture_resolution_scaled
          ? load_pipelines_scaled_[static_cast<size_t>(load_shader)]
          : load_pipelines_[static_cast<size_t>(load_shader)];
  if (!pipeline) {
    return false;
  }

  const TextureCache::LoadShaderInfo& load_shader_info =
      GetLoadShaderInfo(load_shader);
  if (texture_resolution_scaled) {
    static uint32_t scaled_load_log_count = 0;
    if (scaled_load_log_count < 8) {
      ++scaled_load_log_count;
    }
  }

  bool is_block_compressed_format =
      key.format == xenos::TextureFormat::k_DXT1 ||
      key.format == xenos::TextureFormat::k_DXT2_3 ||
      key.format == xenos::TextureFormat::k_DXT4_5 ||
      key.format == xenos::TextureFormat::k_DXN;
  bool host_block_compressed = is_block_compressed_format && !decompress;
  uint32_t host_block_width = host_block_compressed ? block_width : 1;
  uint32_t host_block_height = host_block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread =
      UINT32_C(1) << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_block_compressed) {
    host_x_blocks_per_thread *= block_width;
  }

  struct StoredLevelHostLayout {
    bool is_base;
    uint32_t level;
    uint32_t dest_offset_bytes;
    uint32_t slice_size_bytes;
    uint32_t row_pitch_bytes;
    uint32_t height_blocks;
    uint32_t depth_slices;
    uint32_t width_texels;
    uint32_t height_texels;
  };

  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);

  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  std::vector<StoredLevelHostLayout> stored_levels;
  stored_levels.reserve(loop_level_last - loop_level_first + 1);
  uint64_t dest_buffer_size = 0;

  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last;
       ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    if (!level_guest_layout.level_data_extent_bytes) {
      continue;
    }

    uint32_t level_width_unscaled, level_height_unscaled, level_depth;
    if (level == level_packed) {
      level_width_unscaled = level_guest_layout.x_extent_blocks * block_width;
      level_height_unscaled = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width_unscaled = std::max(width >> level, uint32_t(1));
      level_height_unscaled = std::max(height >> level, uint32_t(1));
      level_depth = std::max(depth >> level, uint32_t(1));
    }

    uint32_t width_texels_scaled = xe::round_up(
        level_width_unscaled * texture_resolution_scale_x, host_block_width);
    uint32_t height_texels_scaled = xe::round_up(
        level_height_unscaled * texture_resolution_scale_y, host_block_height);
    uint32_t width_blocks = width_texels_scaled / host_block_width;
    uint32_t height_blocks = height_texels_scaled / host_block_height;

    const uint32_t row_pitch_alignment =
        ShouldUploadViaBlit() ? uint32_t(256) : uint32_t(16);
    uint32_t row_pitch_bytes =
        xe::align(xe::round_up(width_blocks, host_x_blocks_per_thread) *
                      load_shader_info.bytes_per_host_block,
                  row_pitch_alignment);
    uint32_t slice_size_bytes = xe::align(
        row_pitch_bytes * height_blocks * level_depth, row_pitch_alignment);

    StoredLevelHostLayout host_layout = {};
    host_layout.is_base = is_base;
    host_layout.level = level;
    host_layout.dest_offset_bytes = uint32_t(dest_buffer_size);
    host_layout.slice_size_bytes = slice_size_bytes;
    host_layout.row_pitch_bytes = row_pitch_bytes;
    host_layout.height_blocks = height_blocks;
    host_layout.depth_slices = level_depth;
    host_layout.width_texels = level_width_unscaled;
    host_layout.height_texels = level_height_unscaled;
    stored_levels.push_back(host_layout);

    dest_buffer_size += uint64_t(slice_size_bytes) * uint64_t(array_size);
  }

  if (stored_levels.empty()) {
    return false;
  }
  if (dest_buffer_size > SIZE_MAX) {
    return false;
  }

  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    return false;
  }
  MTL::CommandQueue* queue = command_processor_->GetMetalCommandQueue();
  if (!queue) {
    return false;
  }

  MetalSharedMemory& metal_shared_memory =
      static_cast<MetalSharedMemory&>(shared_memory());
  MTL::Buffer* shared_buffer = metal_shared_memory.GetBuffer();
  if (!shared_buffer) {
    return false;
  }

  std::shared_ptr<UploadBufferPool> buffer_pool;
  {
    std::lock_guard<std::mutex> lock(upload_buffer_pool_mutex_);
    buffer_pool = upload_buffer_pool_;
  }
  auto acquire_buffer = [&](size_t size) -> MTL::Buffer* {
    if (buffer_pool) {
      return buffer_pool->Acquire(size);
    }
    MTL::Buffer* buffer =
        device->newBuffer(size, MTL::ResourceStorageModeShared);
    if (buffer) {
    }
    return buffer;
  };
  auto release_buffer_immediate = [&](MTL::Buffer* buffer, size_t size) {
    if (!buffer) {
      return;
    }
    if (buffer_pool) {
      buffer_pool->ReleaseImmediate(buffer);
      return;
    }
    buffer->release();
  };
  auto release_buffer_after = [&](MTL::CommandBuffer* cmd, MTL::Buffer* buffer,
                                  size_t size) {
    if (!buffer) {
      return;
    }
    if (buffer_pool) {
      buffer_pool->ReleaseAfter(cmd, buffer);
      return;
    }
    cmd->addCompletedHandler(^(MTL::CommandBuffer*) {
      buffer->release();
    });
  };

  MTL::Buffer* dest_buffer = acquire_buffer(size_t(dest_buffer_size));
  if (!dest_buffer) {
    return false;
  }

  uint32_t base_guest_address = key.base_page << 12;
  uint32_t mips_guest_address = key.mip_page << 12;

  size_t constants_size = xe::align(sizeof(MetalLoadConstants), size_t(16));
  size_t dispatch_count = stored_levels.size() * size_t(array_size);
  size_t constants_buffer_size = constants_size * dispatch_count;
  MTL::Buffer* constants_buffer = acquire_buffer(constants_buffer_size);
  if (!constants_buffer) {
    release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
    return false;
  }

  const bool use_blit_upload = ShouldUploadViaBlit();

  auto find_stored_level =
      [&](bool is_base_storage,
          uint32_t stored_level) -> const StoredLevelHostLayout* {
    for (const StoredLevelHostLayout& layout : stored_levels) {
      if (layout.is_base == is_base_storage && layout.level == stored_level) {
        return &layout;
      }
    }
    return nullptr;
  };

  MTL::CommandBuffer* current_command_buffer =
      command_processor_ ? command_processor_->GetCurrentCommandBuffer()
                         : nullptr;
  bool use_upload_batch = use_blit_upload && upload_batch_command_buffer_ &&
                          command_processor_ && !current_command_buffer;
  // Reuse the current command buffer whenever no render pass encoder is active.
  // This keeps copy/resolve and texture-upload ordering within one submission.
  bool use_current_command_buffer =
      use_blit_upload && command_processor_ && current_command_buffer &&
      !command_processor_->HasActiveRenderEncoder();
  if (use_upload_batch && texture_resolution_scaled) {
    bool needs_base_scaled_range = false;
    bool needs_mips_scaled_range = false;
    for (const StoredLevelHostLayout& stored_level : stored_levels) {
      if (stored_level.is_base) {
        needs_base_scaled_range = true;
      } else {
        needs_mips_scaled_range = true;
      }
    }
    if (needs_base_scaled_range &&
        !IsScaledResolveRangeResident(base_guest_address,
                                      texture.GetGuestBaseSize(), 4)) {
      use_upload_batch = false;
    }
    if (use_upload_batch && needs_mips_scaled_range &&
        !IsScaledResolveRangeResident(mips_guest_address,
                                      texture.GetGuestMipsSize(), 4)) {
      use_upload_batch = false;
    }
  }

  ScopedAutoreleasePool autorelease_pool;
  MTL::CommandBuffer* cmd = nullptr;
  if (use_upload_batch) {
    cmd = upload_batch_command_buffer_;
  } else if (use_current_command_buffer) {
    cmd = current_command_buffer;
  } else {
    cmd = queue->commandBuffer();
  }
  if (!cmd) {
    release_buffer_immediate(constants_buffer, constants_buffer_size);
    release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
    return false;
  }
  bool command_buffer_has_work = false;
  auto handle_upload_failure = [&](bool abort_batch) {
    if ((use_upload_batch || use_current_command_buffer) &&
        command_buffer_has_work) {
      release_buffer_after(cmd, constants_buffer, constants_buffer_size);
      release_buffer_after(cmd, dest_buffer, size_t(dest_buffer_size));
      if (use_upload_batch) {
        upload_batch_command_buffer_has_work_ = true;
        AbortUploadCommandBufferBatch();
      }
      return;
    }
    release_buffer_immediate(constants_buffer, constants_buffer_size);
    release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
    if (use_upload_batch && abort_batch) {
      AbortUploadCommandBufferBatch();
    }
  };

  MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
  if (!encoder) {
    handle_upload_failure(true);
    return false;
  }
  encoder->setComputePipelineState(pipeline);
  if (!texture_resolution_scaled) {
    encoder->setBuffer(shared_buffer, 0, 2);
  }

  uint32_t guest_x_blocks_per_group_log2 =
      load_shader_info.GetGuestXBlocksPerGroupLog2();
  MTL::Size threads_per_group =
      MTL::Size::Make(UINT32_C(1) << kLoadGuestXThreadsPerGroupLog2,
                      UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2, 1);

  bool scaled_mips_source_set_up = false;
  MTL::Buffer* source_buffer = shared_buffer;
  size_t source_buffer_offset = 0;
  size_t source_buffer_length = 0;

  size_t dispatch_index = 0;
  for (const StoredLevelHostLayout& stored_level : stored_levels) {
    bool is_base_storage = stored_level.is_base;
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base_storage ? guest_layout.base
                        : guest_layout.mips[stored_level.level];

    if (texture_resolution_scaled &&
        (is_base_storage || !scaled_mips_source_set_up)) {
      uint32_t guest_address =
          is_base_storage ? base_guest_address : mips_guest_address;
      uint32_t guest_size_unscaled = is_base_storage
                                         ? texture.GetGuestBaseSize()
                                         : texture.GetGuestMipsSize();
      if (!MakeScaledResolveRangeCurrent(guest_address, guest_size_unscaled,
                                         4) ||
          !GetCurrentScaledResolveBuffer(source_buffer, source_buffer_offset,
                                         source_buffer_length)) {
        handle_upload_failure(false);
        return false;
      }
      encoder->setBuffer(source_buffer, source_buffer_offset, 2);
      if (!is_base_storage) {
        scaled_mips_source_set_up = true;
      }
    }

    uint32_t level_guest_offset = 0;
    if (!texture_resolution_scaled) {
      level_guest_offset =
          is_base_storage ? base_guest_address : mips_guest_address;
    }
    if (!is_base_storage) {
      uint32_t mip_offset = guest_layout.mip_offsets_bytes[stored_level.level];
      if (texture_resolution_scaled) {
        mip_offset *= texture_resolution_scale_area;
      }
      level_guest_offset += mip_offset;
    }
    // Use guest layout pitch (blocks) - new XeSL expects blocks for both tiled
    // and linear
    uint32_t guest_pitch_aligned =
        level_guest_layout.row_pitch_bytes / bytes_per_block;

    uint32_t size_blocks_x =
        (stored_level.width_texels + (block_width - 1)) / block_width;
    uint32_t size_blocks_y =
        (stored_level.height_texels + (block_height - 1)) / block_height;
    size_blocks_x *= texture_resolution_scale_x;
    size_blocks_y *= texture_resolution_scale_y;

    uint32_t group_count_x =
        (size_blocks_x +
         ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    uint32_t group_count_y =
        (size_blocks_y +
         ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;
    MTL::Size threadgroups = MTL::Size::Make(group_count_x, group_count_y,
                                             stored_level.depth_slices);

    for (uint32_t slice = 0; slice < array_size; ++slice) {
      MetalLoadConstants constants = {};
      constants.is_tiled_3d_endian_scale =
          uint32_t(key.tiled) | (uint32_t(is_3d_tiling) << 1) |
          (uint32_t(key.endianness) << 2) | (texture_resolution_scale_x << 4) |
          (texture_resolution_scale_y << 7);
      constants.guest_offset = level_guest_offset;
      if (!is_3d) {
        uint32_t slice_stride = level_guest_layout.array_slice_stride_bytes;
        if (texture_resolution_scaled) {
          slice_stride *= texture_resolution_scale_area;
        }
        constants.guest_offset += slice * slice_stride;
      }
      constants.guest_pitch_aligned = guest_pitch_aligned;
      constants.guest_z_stride_block_rows_aligned =
          level_guest_layout.z_slice_stride_block_rows;
      constants.size_blocks[0] = size_blocks_x;
      constants.size_blocks[1] = size_blocks_y;
      constants.size_blocks[2] = stored_level.depth_slices;
      constants.padding0 = 0;
      constants.host_offset = 0;
      constants.host_pitch = stored_level.row_pitch_bytes;
      constants.height_texels = stored_level.height_texels;

      uint8_t* constants_ptr =
          static_cast<uint8_t*>(constants_buffer->contents()) +
          dispatch_index * constants_size;
      std::memcpy(constants_ptr, &constants, sizeof(constants));

      encoder->setBuffer(constants_buffer, dispatch_index * constants_size, 0);
      encoder->setBuffer(dest_buffer,
                         stored_level.dest_offset_bytes +
                             slice * stored_level.slice_size_bytes,
                         1);
      encoder->dispatchThreadgroups(threadgroups, threads_per_group);
      command_buffer_has_work = true;
      ++dispatch_index;
    }
  }

  encoder->endEncoding();

  MTL::Texture* mtl_texture = metal_texture->metal_texture();
  if (use_blit_upload) {
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    if (!blit) {
      handle_upload_failure(true);
      return false;
    }

    uint32_t bytes_per_host_block = load_shader_info.bytes_per_host_block;
    const uint32_t blit_alignment = 256;

    for (uint32_t level = level_first; level <= level_last; ++level) {
      uint32_t stored_level = std::min(level, level_packed);
      bool is_base_storage =
          stored_level == 0 && (level_packed != 0 || level == 0);
      const StoredLevelHostLayout* stored_layout =
          find_stored_level(is_base_storage, stored_level);
      if (!stored_layout) {
        continue;
      }

      uint32_t level_width_unscaled = std::max(width >> level, uint32_t(1));
      uint32_t level_height_unscaled = std::max(height >> level, uint32_t(1));
      uint32_t level_depth = std::max(depth >> level, uint32_t(1));
      uint32_t level_width_scaled =
          level_width_unscaled * texture_resolution_scale_x;
      uint32_t level_height_scaled =
          level_height_unscaled * texture_resolution_scale_y;

      const uint32_t upload_width_texels = level_width_scaled;
      const uint32_t upload_height_texels = level_height_scaled;

      uint32_t packed_offset_blocks_x = 0;
      uint32_t packed_offset_blocks_y = 0;
      uint32_t packed_offset_z = 0;
      if (level >= level_packed) {
        texture_util::GetPackedMipOffset(
            width, height, depth, key.format, level, packed_offset_blocks_x,
            packed_offset_blocks_y, packed_offset_z);
      }

      uint32_t upload_blocks_x =
          xe::round_up(upload_width_texels, host_block_width) /
          host_block_width;
      uint32_t upload_blocks_y =
          xe::round_up(upload_height_texels, host_block_height) /
          host_block_height;
      uint32_t upload_row_bytes = upload_blocks_x * bytes_per_host_block;
      uint32_t upload_row_count = upload_blocks_y;
      uint32_t blit_row_pitch = xe::align(upload_row_bytes, blit_alignment);
      size_t blit_bytes_per_image = size_t(blit_row_pitch) * upload_row_count;
      size_t bytes_per_image =
          size_t(stored_layout->row_pitch_bytes) * stored_layout->height_blocks;

      for (uint32_t slice = 0; slice < array_size; ++slice) {
        size_t source_offset_bytes = stored_layout->dest_offset_bytes +
                                     slice * stored_layout->slice_size_bytes;
        if (level >= level_packed) {
          if (host_block_compressed) {
            uint32_t packed_offset_blocks_x_scaled =
                packed_offset_blocks_x * texture_resolution_scale_x;
            uint32_t packed_offset_blocks_y_scaled =
                packed_offset_blocks_y * texture_resolution_scale_y;
            source_offset_bytes += packed_offset_z * bytes_per_image;
            source_offset_bytes +=
                packed_offset_blocks_y_scaled * stored_layout->row_pitch_bytes;
            source_offset_bytes +=
                packed_offset_blocks_x_scaled * bytes_per_block;
          } else {
            uint32_t packed_offset_texels_x =
                packed_offset_blocks_x * block_width;
            uint32_t packed_offset_texels_y =
                packed_offset_blocks_y * block_height;
            packed_offset_texels_x *= texture_resolution_scale_x;
            packed_offset_texels_y *= texture_resolution_scale_y;
            source_offset_bytes += packed_offset_z * bytes_per_image;
            source_offset_bytes +=
                packed_offset_texels_y * stored_layout->row_pitch_bytes;
            source_offset_bytes +=
                packed_offset_texels_x * bytes_per_host_block;
          }
        }

        bool requires_staging = (source_offset_bytes % blit_alignment) != 0;
        if (requires_staging) {
          size_t staging_size = blit_bytes_per_image * level_depth;
          MTL::Buffer* staging_buffer = acquire_buffer(staging_size);
          if (!staging_buffer) {
            blit->endEncoding();
            handle_upload_failure(true);
            return false;
          }

          for (uint32_t z = 0; z < level_depth; ++z) {
            size_t src_z_offset =
                source_offset_bytes + size_t(z) * bytes_per_image;
            size_t dst_z_offset = size_t(z) * blit_bytes_per_image;
            for (uint32_t y = 0; y < upload_row_count; ++y) {
              size_t src_row_offset =
                  src_z_offset + size_t(y) * stored_layout->row_pitch_bytes;
              size_t dst_row_offset = dst_z_offset + size_t(y) * blit_row_pitch;
              blit->copyFromBuffer(dest_buffer, src_row_offset, staging_buffer,
                                   dst_row_offset, upload_row_bytes);
            }
          }

          blit->copyFromBuffer(
              staging_buffer, 0, blit_row_pitch, blit_bytes_per_image,
              MTL::Size::Make(upload_width_texels, upload_height_texels,
                              level_depth),
              mtl_texture, is_3d ? 0 : slice, level,
              MTL::Origin::Make(0, 0, 0));

          release_buffer_after(cmd, staging_buffer, staging_size);
        } else {
          blit->copyFromBuffer(
              dest_buffer, source_offset_bytes, stored_layout->row_pitch_bytes,
              bytes_per_image,
              MTL::Size::Make(upload_width_texels, upload_height_texels,
                              level_depth),
              mtl_texture, is_3d ? 0 : slice, level,
              MTL::Origin::Make(0, 0, 0));
        }
      }
    }

    blit->endEncoding();
    release_buffer_after(cmd, constants_buffer, constants_buffer_size);
    release_buffer_after(cmd, dest_buffer, size_t(dest_buffer_size));
    if (use_upload_batch) {
      upload_batch_command_buffer_has_work_ = true;
    } else if (!use_current_command_buffer) {
      cmd->retain();
      cmd->addCompletedHandler(^(MTL::CommandBuffer* cb) {
        cb->release();
      });
      cmd->commit();
    }
  } else {
    cmd->commit();
    cmd->waitUntilCompleted();

    uint8_t* dest_data = static_cast<uint8_t*>(dest_buffer->contents());
    if (!dest_data) {
      release_buffer_immediate(constants_buffer, constants_buffer_size);
      release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
      return false;
    }

    auto find_stored_level =
        [&](bool is_base_storage,
            uint32_t stored_level) -> const StoredLevelHostLayout* {
      for (const StoredLevelHostLayout& layout : stored_levels) {
        if (layout.is_base == is_base_storage && layout.level == stored_level) {
          return &layout;
        }
      }
      return nullptr;
    };

    uint32_t bytes_per_host_block = load_shader_info.bytes_per_host_block;

    for (uint32_t level = level_first; level <= level_last; ++level) {
      uint32_t stored_level = std::min(level, level_packed);
      bool is_base_storage =
          stored_level == 0 && (level_packed != 0 || level == 0);
      const StoredLevelHostLayout* stored_layout =
          find_stored_level(is_base_storage, stored_level);
      if (!stored_layout) {
        continue;
      }

      uint32_t level_width_unscaled = std::max(width >> level, uint32_t(1));
      uint32_t level_height_unscaled = std::max(height >> level, uint32_t(1));
      uint32_t level_depth = std::max(depth >> level, uint32_t(1));
      uint32_t level_width_scaled =
          level_width_unscaled * texture_resolution_scale_x;
      uint32_t level_height_scaled =
          level_height_unscaled * texture_resolution_scale_y;

      uint32_t upload_width = level_width_scaled;
      uint32_t upload_height = level_height_scaled;
      if (host_block_compressed) {
        upload_width = xe::round_up(upload_width, host_block_width);
        upload_height = xe::round_up(upload_height, host_block_height);
      }

      uint32_t packed_offset_blocks_x = 0;
      uint32_t packed_offset_blocks_y = 0;
      uint32_t packed_offset_z = 0;
      if (level >= level_packed) {
        texture_util::GetPackedMipOffset(
            width, height, depth, key.format, level, packed_offset_blocks_x,
            packed_offset_blocks_y, packed_offset_z);
      }

      for (uint32_t slice = 0; slice < array_size; ++slice) {
        const uint8_t* slice_base = dest_data +
                                    stored_layout->dest_offset_bytes +
                                    slice * stored_layout->slice_size_bytes;

        const uint8_t* source_ptr = slice_base;
        size_t bytes_per_image = size_t(stored_layout->row_pitch_bytes) *
                                 stored_layout->height_blocks;
        if (level >= level_packed) {
          if (host_block_compressed) {
            uint32_t packed_offset_blocks_x_scaled =
                packed_offset_blocks_x * texture_resolution_scale_x;
            uint32_t packed_offset_blocks_y_scaled =
                packed_offset_blocks_y * texture_resolution_scale_y;
            source_ptr += packed_offset_z * bytes_per_image;
            source_ptr +=
                packed_offset_blocks_y_scaled * stored_layout->row_pitch_bytes;
            source_ptr += packed_offset_blocks_x_scaled * bytes_per_block;
          } else {
            uint32_t packed_offset_texels_x =
                packed_offset_blocks_x * block_width;
            uint32_t packed_offset_texels_y =
                packed_offset_blocks_y * block_height;
            packed_offset_texels_x *= texture_resolution_scale_x;
            packed_offset_texels_y *= texture_resolution_scale_y;
            source_ptr += packed_offset_z * bytes_per_image;
            source_ptr +=
                packed_offset_texels_y * stored_layout->row_pitch_bytes;
            source_ptr += packed_offset_texels_x * bytes_per_host_block;
          }
        }

        if (dimension == xenos::DataDimension::k3D) {
          MTL::Region region = MTL::Region::Make3D(0, 0, 0, upload_width,
                                                   upload_height, level_depth);
          mtl_texture->replaceRegion(region, level, 0, source_ptr,
                                     stored_layout->row_pitch_bytes,
                                     bytes_per_image);
        } else {
          MTL::Region region =
              MTL::Region::Make2D(0, 0, upload_width, upload_height);
          mtl_texture->replaceRegion(region, level, slice, source_ptr,
                                     stored_layout->row_pitch_bytes, 0);
        }
      }
    }
  }

  if (!use_blit_upload) {
    release_buffer_immediate(constants_buffer, constants_buffer_size);
    release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
  }

  return true;
}

void MetalTextureCache::DumpTextureToFile(MTL::Texture* texture,
                                          const std::string& filename,
                                          uint32_t width, uint32_t height) {
  if (!texture) {
    XELOGE("DumpTextureToFile: null texture");
    return;
  }

  MTL::Device* device = command_processor_->GetMetalDevice();
  MTL::CommandQueue* queue = command_processor_->GetMetalCommandQueue();
  if (!device || !queue) {
    XELOGE("DumpTextureToFile: missing Metal device or command queue");
    return;
  }

  // Calculate bytes per row (align for blit requirements).
  size_t bytes_per_pixel = 4;  // Assuming RGBA8
  size_t bytes_per_row_unaligned = width * bytes_per_pixel;
  size_t bytes_per_row = xe::align(bytes_per_row_unaligned, size_t(256));
  size_t buffer_size = bytes_per_row * height;

  MTL::Buffer* readback =
      device->newBuffer(buffer_size, MTL::ResourceStorageModeShared);
  if (!readback) {
    XELOGE("DumpTextureToFile: failed to allocate readback buffer");
    return;
  }

  ScopedAutoreleasePool autorelease_pool;
  MTL::CommandBuffer* cmd = queue->commandBuffer();
  if (!cmd) {
    readback->release();
    XELOGE("DumpTextureToFile: failed to create command buffer");
    return;
  }
  MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
  if (!blit) {
    readback->release();
    XELOGE("DumpTextureToFile: failed to create blit encoder");
    return;
  }

  blit->copyFromTexture(texture, 0, 0, MTL::Origin::Make(0, 0, 0),
                        MTL::Size::Make(width, height, 1), readback, 0,
                        bytes_per_row, 0);
  blit->endEncoding();
  cmd->commit();
  cmd->waitUntilCompleted();

  // Allocate buffer for packed texture data (tightly packed rows).
  std::vector<uint8_t> data(bytes_per_row_unaligned * height);
  const uint8_t* src = static_cast<const uint8_t*>(readback->contents());
  if (!src) {
    readback->release();
    XELOGE("DumpTextureToFile: failed to map readback buffer");
    return;
  }
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(data.data() + y * bytes_per_row_unaligned,
                src + y * bytes_per_row, bytes_per_row_unaligned);
  }

  readback->release();

  if (texture->pixelFormat() == MTL::PixelFormatBGRA8Unorm) {
    // Convert BGRA to RGBA for stb_image_write
    for (size_t i = 0; i < data.size(); i += 4) {
      std::swap(data[i], data[i + 2]);
    }
  }

  // Write PNG file
  if (stbi_write_png(filename.c_str(), width, height, 4, data.data(),
                     bytes_per_row)) {
  } else {
    XELOGE("Failed to write texture to: {}", filename);
  }
}

bool MetalTextureCache::Initialize() {
  SCOPE_profile_cpu_f("gpu");
  XE_SCOPED_AUTORELEASE_POOL("MetalTextureCache::Initialize");

  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE(
        "Metal texture cache: Failed to get Metal device from command "
        "processor");
    return false;
  }
  supports_bc_texture_compression_ = device->supportsBCTextureCompression();
  if (!supports_bc_texture_compression_) {
    XELOGW(
        "Metal: BC texture compression not supported by this device; forcing "
        "BCn decompression");
  }
  if (::cvars::metal_texture_cache_use_private &&
      !::cvars::metal_texture_upload_via_blit) {
    XELOGW(
        "Metal texture cache: private textures requested but blit uploads "
        "disabled; forcing shared textures");
  }

  {
    std::lock_guard<std::mutex> lock(upload_buffer_pool_mutex_);
    upload_buffer_pool_ =
        std::make_shared<UploadBufferPool>(device, kUploadBufferPoolMaxBytes);
  }
  if (::cvars::metal_use_heaps) {
    size_t min_heap_bytes = std::max<int32_t>(0, ::cvars::metal_heap_min_bytes);
    texture_heap_pool_ = std::make_unique<MetalHeapPool>(
        device, GetCacheTextureStorageMode(), min_heap_bytes, "XeniaTex");
  }

  InitializeNorm16Selection(device);

  // Create null textures following existing factory pattern
  null_texture_2d_ = CreateNullTexture2D();
  null_texture_3d_ = CreateNullTexture3D();
  null_texture_cube_ = CreateNullTextureCube();

  if (!null_texture_2d_ || !null_texture_3d_ || !null_texture_cube_) {
    XELOGE("Failed to create null textures");
    return false;
  }

  if (!InitializeLoadPipelines()) {
    XELOGE("Metal texture cache: Failed to initialize texture_load pipelines");
    return false;
  }

  XELOGD(
      "Metal texture cache: Initialized successfully (null textures + GPU "
      "texture_load pipelines)");

  return true;
}

bool MetalTextureCache::InitializeLoadPipelines() {
  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    return false;
  }

  NS::Error* error = nullptr;

  auto create_pipeline_from_metallib =
      [&](const uint8_t* data, size_t size) -> MTL::ComputePipelineState* {
    if (!data || !size) {
      return nullptr;
    }
    dispatch_data_t dispatch_data = dispatch_data_create(
        data, size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* lib = device->newLibrary(dispatch_data, &error);
    dispatch_release(dispatch_data);
    if (!lib) {
      XELOGE("MetalTextureCache: failed to create texture_load library: {}",
             error ? error->localizedDescription()->utf8String() : "unknown");
      return nullptr;
    }
    NS::String* fn_name =
        NS::String::string("entry_xe", NS::UTF8StringEncoding);
    MTL::Function* fn = lib->newFunction(fn_name);
    if (!fn) {
      XELOGE(
          "MetalTextureCache: texture_load metallib missing entry_xe "
          "function");
      lib->release();
      return nullptr;
    }
    MTL::ComputePipelineState* pipeline =
        device->newComputePipelineState(fn, &error);
    fn->release();
    lib->release();
    if (!pipeline) {
      XELOGE("MetalTextureCache: failed to create texture_load pipeline: {}",
             error ? error->localizedDescription()->utf8String() : "unknown");
      return nullptr;
    }
    return pipeline;
  };

  auto init_pipeline = [&](TextureCache::LoadShaderIndex index,
                           const uint8_t* data, size_t size) -> void {
    load_pipelines_[index] = create_pipeline_from_metallib(data, size);
  };
  auto init_pipeline_scaled = [&](TextureCache::LoadShaderIndex index,
                                  const uint8_t* data, size_t size) -> void {
    load_pipelines_scaled_[index] = create_pipeline_from_metallib(data, size);
  };

  init_pipeline(TextureCache::kLoadShaderIndex8bpb,
                texture_load_8bpb_cs_metallib,
                sizeof(texture_load_8bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex8bpb,
                       texture_load_8bpb_scaled_cs_metallib,
                       sizeof(texture_load_8bpb_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndex16bpb,
                texture_load_16bpb_cs_metallib,
                sizeof(texture_load_16bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex16bpb,
                       texture_load_16bpb_scaled_cs_metallib,
                       sizeof(texture_load_16bpb_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndex32bpb,
                texture_load_32bpb_cs_metallib,
                sizeof(texture_load_32bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex32bpb,
                       texture_load_32bpb_scaled_cs_metallib,
                       sizeof(texture_load_32bpb_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndex64bpb,
                texture_load_64bpb_cs_metallib,
                sizeof(texture_load_64bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex64bpb,
                       texture_load_64bpb_scaled_cs_metallib,
                       sizeof(texture_load_64bpb_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndex128bpb,
                texture_load_128bpb_cs_metallib,
                sizeof(texture_load_128bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex128bpb,
                       texture_load_128bpb_scaled_cs_metallib,
                       sizeof(texture_load_128bpb_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
                texture_load_r5g5b5a1_b5g5r5a1_cs_metallib,
                sizeof(texture_load_r5g5b5a1_b5g5r5a1_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
      texture_load_r5g5b5a1_b5g5r5a1_scaled_cs_metallib,
      sizeof(texture_load_r5g5b5a1_b5g5r5a1_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR5G6B5ToB5G6R5,
                texture_load_r5g6b5_b5g6r5_cs_metallib,
                sizeof(texture_load_r5g6b5_b5g6r5_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexR5G6B5ToB5G6R5,
                       texture_load_r5g6b5_b5g6r5_scaled_cs_metallib,
                       sizeof(texture_load_r5g6b5_b5g6r5_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
                texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs_metallib,
                sizeof(texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
      texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs_metallib,
      sizeof(texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexR10G11B11ToRGBA16,
                texture_load_r10g11b11_rgba16_cs_metallib,
                sizeof(texture_load_r10g11b11_rgba16_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR10G11B11ToRGBA16,
      texture_load_r10g11b11_rgba16_scaled_cs_metallib,
      sizeof(texture_load_r10g11b11_rgba16_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR10G11B11ToRGBA16SNorm,
                texture_load_r10g11b11_rgba16_snorm_cs_metallib,
                sizeof(texture_load_r10g11b11_rgba16_snorm_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR10G11B11ToRGBA16SNorm,
      texture_load_r10g11b11_rgba16_snorm_scaled_cs_metallib,
      sizeof(texture_load_r10g11b11_rgba16_snorm_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR11G11B10ToRGBA16,
                texture_load_r11g11b10_rgba16_cs_metallib,
                sizeof(texture_load_r11g11b10_rgba16_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR11G11B10ToRGBA16,
      texture_load_r11g11b10_rgba16_scaled_cs_metallib,
      sizeof(texture_load_r11g11b10_rgba16_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR11G11B10ToRGBA16SNorm,
                texture_load_r11g11b10_rgba16_snorm_cs_metallib,
                sizeof(texture_load_r11g11b10_rgba16_snorm_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR11G11B10ToRGBA16SNorm,
      texture_load_r11g11b10_rgba16_snorm_scaled_cs_metallib,
      sizeof(texture_load_r11g11b10_rgba16_snorm_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexR16UNormToFloat,
                texture_load_r16_unorm_float_cs_metallib,
                sizeof(texture_load_r16_unorm_float_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexR16UNormToFloat,
                       texture_load_r16_unorm_float_scaled_cs_metallib,
                       sizeof(texture_load_r16_unorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR16SNormToFloat,
                texture_load_r16_snorm_float_cs_metallib,
                sizeof(texture_load_r16_snorm_float_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexR16SNormToFloat,
                       texture_load_r16_snorm_float_scaled_cs_metallib,
                       sizeof(texture_load_r16_snorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRG16UNormToFloat,
                texture_load_rg16_unorm_float_cs_metallib,
                sizeof(texture_load_rg16_unorm_float_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRG16UNormToFloat,
      texture_load_rg16_unorm_float_scaled_cs_metallib,
      sizeof(texture_load_rg16_unorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRG16SNormToFloat,
                texture_load_rg16_snorm_float_cs_metallib,
                sizeof(texture_load_rg16_snorm_float_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRG16SNormToFloat,
      texture_load_rg16_snorm_float_scaled_cs_metallib,
      sizeof(texture_load_rg16_snorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRGBA16UNormToFloat,
                texture_load_rgba16_unorm_float_cs_metallib,
                sizeof(texture_load_rgba16_unorm_float_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRGBA16UNormToFloat,
      texture_load_rgba16_unorm_float_scaled_cs_metallib,
      sizeof(texture_load_rgba16_unorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRGBA16SNormToFloat,
                texture_load_rgba16_snorm_float_cs_metallib,
                sizeof(texture_load_rgba16_snorm_float_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRGBA16SNormToFloat,
      texture_load_rgba16_snorm_float_scaled_cs_metallib,
      sizeof(texture_load_rgba16_snorm_float_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexRGBA4ToBGRA4,
                texture_load_r4g4b4a4_b4g4r4a4_cs_metallib,
                sizeof(texture_load_r4g4b4a4_b4g4r4a4_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRGBA4ToBGRA4,
      texture_load_r4g4b4a4_b4g4r4a4_scaled_cs_metallib,
      sizeof(texture_load_r4g4b4a4_b4g4r4a4_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRGBA4ToARGB4,
                texture_load_r4g4b4a4_a4r4g4b4_cs_metallib,
                sizeof(texture_load_r4g4b4a4_a4r4g4b4_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRGBA4ToARGB4,
      texture_load_r4g4b4a4_a4r4g4b4_scaled_cs_metallib,
      sizeof(texture_load_r4g4b4a4_a4r4g4b4_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexGBGR8ToGRGB8,
                texture_load_gbgr8_grgb8_cs_metallib,
                sizeof(texture_load_gbgr8_grgb8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexGBGR8ToRGB8,
                texture_load_gbgr8_rgb8_cs_metallib,
                sizeof(texture_load_gbgr8_rgb8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexBGRG8ToRGBG8,
                texture_load_bgrg8_rgbg8_cs_metallib,
                sizeof(texture_load_bgrg8_rgbg8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexBGRG8ToRGB8,
                texture_load_bgrg8_rgb8_cs_metallib,
                sizeof(texture_load_bgrg8_rgb8_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexDXT1ToRGBA8,
                texture_load_dxt1_rgba8_cs_metallib,
                sizeof(texture_load_dxt1_rgba8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT3A,
                texture_load_dxt3a_cs_metallib,
                sizeof(texture_load_dxt3a_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT3AAs1111ToBGRA4,
                texture_load_dxt3aas1111_bgra4_cs_metallib,
                sizeof(texture_load_dxt3aas1111_bgra4_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT3AAs1111ToARGB4,
                texture_load_dxt3aas1111_argb4_cs_metallib,
                sizeof(texture_load_dxt3aas1111_argb4_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT3ToRGBA8,
                texture_load_dxt3_rgba8_cs_metallib,
                sizeof(texture_load_dxt3_rgba8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT5ToRGBA8,
                texture_load_dxt5_rgba8_cs_metallib,
                sizeof(texture_load_dxt5_rgba8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT5AToR8,
                texture_load_dxt5a_r8_cs_metallib,
                sizeof(texture_load_dxt5a_r8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXNToRG8,
                texture_load_dxn_rg8_cs_metallib,
                sizeof(texture_load_dxn_rg8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexCTX1,
                texture_load_ctx1_cs_metallib,
                sizeof(texture_load_ctx1_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexDepthUnorm,
                texture_load_depth_unorm_cs_metallib,
                sizeof(texture_load_depth_unorm_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexDepthUnorm,
                       texture_load_depth_unorm_scaled_cs_metallib,
                       sizeof(texture_load_depth_unorm_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDepthFloat,
                texture_load_depth_float_cs_metallib,
                sizeof(texture_load_depth_float_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexDepthFloat,
                       texture_load_depth_float_scaled_cs_metallib,
                       sizeof(texture_load_depth_float_scaled_cs_metallib));

  // Require at least the common loaders.
  return load_pipelines_[TextureCache::kLoadShaderIndex32bpb] != nullptr &&
         load_pipelines_[TextureCache::kLoadShaderIndex16bpb] != nullptr &&
         load_pipelines_[TextureCache::kLoadShaderIndex8bpb] != nullptr;
}

void MetalTextureCache::InitializeNorm16Selection(MTL::Device* device) {
  r16_selection_.unsigned_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatR16Unorm);
  r16_selection_.signed_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatR16Snorm);

  rg16_selection_.unsigned_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatRG16Unorm);
  rg16_selection_.signed_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatRG16Snorm);

  rgba16_selection_.unsigned_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatRGBA16Unorm);
  rgba16_selection_.signed_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatRGBA16Snorm);
}

void MetalTextureCache::Shutdown() {
  SCOPE_profile_cpu_f("gpu");

  AbortUploadCommandBufferBatch(false);

  ClearCache();

  for (size_t i = 0; i < kLoadShaderCount; ++i) {
    if (load_pipelines_[i]) {
      load_pipelines_[i]->release();
      load_pipelines_[i] = nullptr;
    }
    if (load_pipelines_scaled_[i]) {
      load_pipelines_scaled_[i]->release();
      load_pipelines_scaled_[i] = nullptr;
    }
  }

  // Follow existing shutdown pattern - explicit null checks and release
  if (null_texture_2d_) {
    null_texture_2d_->release();
    null_texture_2d_ = nullptr;
  }
  if (null_texture_3d_) {
    null_texture_3d_->release();
    null_texture_3d_ = nullptr;
  }
  if (null_texture_cube_) {
    null_texture_cube_->release();
    null_texture_cube_ = nullptr;
  }

  {
    std::shared_ptr<UploadBufferPool> buffer_pool;
    {
      std::lock_guard<std::mutex> lock(upload_buffer_pool_mutex_);
      buffer_pool = std::move(upload_buffer_pool_);
    }
    if (buffer_pool) {
      buffer_pool->Shutdown();
    }
  }
  if (texture_heap_pool_) {
    texture_heap_pool_->Shutdown();
    texture_heap_pool_.reset();
  }

  XELOGD("Metal texture cache: Shutdown complete");
}

void MetalTextureCache::ClearScaledResolveBuffers() {
  for (auto& buffer : scaled_resolve_buffers_) {
    if (buffer.buffer) {
      buffer.buffer->release();
      buffer.buffer = nullptr;
    }
  }
  scaled_resolve_buffers_.clear();
  for (auto& retired : scaled_resolve_retired_buffers_) {
    if (retired.buffer) {
      retired.buffer->release();
      retired.buffer = nullptr;
    }
  }
  scaled_resolve_retired_buffers_.clear();
  scaled_resolve_retired_bytes_ = 0;
  scaled_resolve_current_buffer_index_ = size_t(-1);
  scaled_resolve_current_range_start_scaled_ = 0;
  scaled_resolve_current_range_length_scaled_ = 0;
}

void MetalTextureCache::CompletedSubmissionUpdated(
    uint64_t completed_submission_index) {
  TextureCache::CompletedSubmissionUpdated(completed_submission_index);
  if (scaled_resolve_retired_buffers_.empty()) {
    return;
  }
  for (auto it = scaled_resolve_retired_buffers_.begin();
       it != scaled_resolve_retired_buffers_.end();) {
    if (it->submission_id <= completed_submission_index) {
      if (it->buffer) {
        it->buffer->release();
      }
      scaled_resolve_retired_bytes_ =
          scaled_resolve_retired_bytes_ > it->length_scaled
              ? (scaled_resolve_retired_bytes_ - it->length_scaled)
              : 0;
      it = scaled_resolve_retired_buffers_.erase(it);
    } else {
      ++it;
    }
  }
}

void MetalTextureCache::ClearCache() {
  SCOPE_profile_cpu_f("gpu");

  for (auto& sampler_pair : sampler_cache_) {
    if (sampler_pair.second) {
      sampler_pair.second->release();
    }
  }
  sampler_cache_.clear();
  ClearScaledResolveBuffers();

  XELOGD("Metal texture cache: Cache cleared");
}

// Legacy method - kept for compatibility but now uses base class texture
// management
bool MetalTextureCache::UploadTexture2D(const TextureInfo& texture_info) {
  XELOGD("UploadTexture2D: Legacy method called - delegating to base class");
  // The base class RequestTextures will handle texture creation and loading
  return true;
}

// Legacy method - kept for compatibility but now uses base class texture
// management
bool MetalTextureCache::UploadTextureCube(const TextureInfo& texture_info) {
  XELOGD("UploadTextureCube: Legacy method called - delegating to base class");
  // The base class RequestTextures will handle texture creation and loading
  return true;
}

MTL::PixelFormat MetalTextureCache::ConvertXenosFormat(
    xenos::TextureFormat format, xenos::Endian endian) {
  // Convert Xbox 360 texture formats to Metal pixel formats
  // This is a simplified mapping - the full implementation would handle all
  // Xbox 360 formats
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
      // Xbox 360 k_8_8_8_8 is stored as ARGB in big-endian. After k_8in32
      // endian swap on little-endian, the byte order is BGRA, and we swizzle
      // to RGBA for Metal.
      return MTL::PixelFormatRGBA8Unorm;
    case xenos::TextureFormat::k_1_5_5_5:
      return MTL::PixelFormatA1BGR5Unorm;
    case xenos::TextureFormat::k_5_6_5:
      return MTL::PixelFormatB5G6R5Unorm;
    case xenos::TextureFormat::k_8:
      return MTL::PixelFormatR8Unorm;
    case xenos::TextureFormat::k_8_8:
      return MTL::PixelFormatRG8Unorm;
    case xenos::TextureFormat::k_DXT1:
      return MTL::PixelFormatBC1_RGBA;
    case xenos::TextureFormat::k_DXT2_3:
      return MTL::PixelFormatBC2_RGBA;
    case xenos::TextureFormat::k_DXT4_5:
      return MTL::PixelFormatBC3_RGBA;
    case xenos::TextureFormat::k_16_16_16_16:
      return MTL::PixelFormatRGBA16Unorm;
    case xenos::TextureFormat::k_2_10_10_10:
      return MTL::PixelFormatRGB10A2Unorm;
    case xenos::TextureFormat::k_16_FLOAT:
      return MTL::PixelFormatR16Float;
    case xenos::TextureFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;
    case xenos::TextureFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return MTL::PixelFormatRGBA32Float;
    case xenos::TextureFormat::k_DXN:  // BC5
      return MTL::PixelFormatBC5_RGUnorm;
    default:
      // Don't log here - caller will log the error with more context
      return MTL::PixelFormatInvalid;
  }
}

MTL::Texture* MetalTextureCache::CreateTexture2D(
    uint32_t width, uint32_t height, uint32_t array_length,
    MTL::PixelFormat format, MTL::TextureSwizzleChannels swizzle,
    uint32_t mip_levels) {
  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE(
        "Metal texture cache: Failed to get Metal device from command "
        "processor");
    return nullptr;
  }

  // Always create 2D array textures (even with a single layer) so that the
  // Metal texture type matches the shader expectation of texture2d_array,
  // mirroring the D3D12 backend which uses TEXTURE2DARRAY SRVs for 1D/2D
  // textures.
  array_length = std::max(array_length, 1u);

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  descriptor->setTextureType(MTL::TextureType2DArray);
  descriptor->setPixelFormat(format);
  descriptor->setWidth(width);
  descriptor->setHeight(std::max(height, 1u));
  descriptor->setDepth(1);
  descriptor->setArrayLength(array_length);
  descriptor->setMipmapLevelCount(mip_levels);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(GetCacheTextureStorageMode());
  descriptor->setSwizzle(swizzle);

  MTL::Texture* texture = nullptr;
  if (texture_heap_pool_ &&
      descriptor->storageMode() == MTL::StorageModePrivate) {
    texture = texture_heap_pool_->CreateTexture(descriptor);
  }
  if (!texture) {
    texture = device->newTexture(descriptor);
  }

  descriptor->release();

  if (!texture) {
    XELOGE(
        "Metal texture cache: Failed to create 2D array texture {}x{} (layers "
        "{})",
        width, height, array_length);
    return nullptr;
  }

  return texture;
}

MTL::Texture* MetalTextureCache::CreateTexture3D(
    uint32_t width, uint32_t height, uint32_t depth, MTL::PixelFormat format,
    MTL::TextureSwizzleChannels swizzle, uint32_t mip_levels) {
  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE(
        "Metal texture cache: Failed to get Metal device from command "
        "processor");
    return nullptr;
  }

  depth = std::max(depth, 1u);

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  descriptor->setTextureType(MTL::TextureType3D);
  descriptor->setPixelFormat(format);
  descriptor->setWidth(width);
  descriptor->setHeight(std::max(height, 1u));
  descriptor->setDepth(depth);
  descriptor->setArrayLength(1);
  descriptor->setMipmapLevelCount(mip_levels);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(GetCacheTextureStorageMode());
  descriptor->setSwizzle(swizzle);

  MTL::Texture* texture = nullptr;
  if (texture_heap_pool_ &&
      descriptor->storageMode() == MTL::StorageModePrivate) {
    texture = texture_heap_pool_->CreateTexture(descriptor);
  }
  if (!texture) {
    texture = device->newTexture(descriptor);
  }

  descriptor->release();

  if (!texture) {
    XELOGE("Metal texture cache: Failed to create 3D texture {}x{}x{}", width,
           height, depth);
    return nullptr;
  }

  return texture;
}

MTL::Texture* MetalTextureCache::CreateTextureCube(
    uint32_t width, MTL::PixelFormat format,
    MTL::TextureSwizzleChannels swizzle, uint32_t mip_levels,
    uint32_t cube_count) {
  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE(
        "Metal texture cache: Failed to get Metal device from command "
        "processor");
    return nullptr;
  }

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  // Use cube for single-cube textures to match non-array cube bindings in the
  // translated MSL, and cube-array only when multiple cubes are present.
  descriptor->setTextureType(cube_count > 1 ? MTL::TextureTypeCubeArray
                                            : MTL::TextureTypeCube);
  descriptor->setArrayLength(std::max(cube_count, 1u));
  descriptor->setPixelFormat(format);
  descriptor->setWidth(width);
  descriptor->setHeight(width);
  descriptor->setDepth(1);
  descriptor->setMipmapLevelCount(mip_levels);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(GetCacheTextureStorageMode());
  descriptor->setSwizzle(swizzle);

  MTL::Texture* texture = nullptr;
  if (texture_heap_pool_ &&
      descriptor->storageMode() == MTL::StorageModePrivate) {
    texture = texture_heap_pool_->CreateTexture(descriptor);
  }
  if (!texture) {
    texture = device->newTexture(descriptor);
  }

  descriptor->release();

  if (!texture) {
    XELOGE("Metal texture cache: Failed to create Cube texture {}x{}", width,
           width);
    return nullptr;
  }

  return texture;
}

MTL::Texture* MetalTextureCache::CreateNullTexture2D() {
  SCOPE_profile_cpu_f("gpu");

  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE("Metal texture cache: Failed to get Metal device for null texture");
    return nullptr;
  }

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  // Null 2D textures are created as 2D arrays with a single layer so they can
  // be bound wherever shaders expect texture2d_array.
  descriptor->setTextureType(MTL::TextureType2DArray);
  descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  descriptor->setWidth(1);
  descriptor->setHeight(1);
  descriptor->setArrayLength(1);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(MTL::StorageModeShared);

  MTL::Texture* texture = device->newTexture(descriptor);
  descriptor->release();  // Immediate release following pattern

  if (texture) {
    // Initialize with black color (0xFF000000 for RGBA8)
    uint32_t default_color = 0xFF000000;
    MTL::Region region = MTL::Region::Make2D(0, 0, 1, 1);
    texture->replaceRegion(region, 0, &default_color, 4);
  } else {
    XELOGE("Failed to create null 2D texture");
  }

  return texture;  // No retain needed - newTexture returns retained object
}

MTL::Texture* MetalTextureCache::CreateNullTexture3D() {
  SCOPE_profile_cpu_f("gpu");

  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE(
        "Metal texture cache: Failed to get Metal device for null 3D texture");
    return nullptr;
  }

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  descriptor->setTextureType(MTL::TextureType3D);
  descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  descriptor->setWidth(1);
  descriptor->setHeight(1);
  descriptor->setDepth(1);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(MTL::StorageModeShared);

  MTL::Texture* texture = device->newTexture(descriptor);
  descriptor->release();  // Immediate release following pattern

  if (texture) {
    // Initialize with black color (0xFF000000 for RGBA8)
    uint32_t default_color = 0xFF000000;
    MTL::Region region = MTL::Region::Make3D(0, 0, 0, 1, 1, 1);
    texture->replaceRegion(region, 0, 0, &default_color, 4, 4);
  } else {
    XELOGE("Failed to create null 3D texture");
  }

  return texture;  // No retain needed - newTexture returns retained object
}

MTL::Texture* MetalTextureCache::CreateNullTextureCube() {
  SCOPE_profile_cpu_f("gpu");

  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE(
        "Metal texture cache: Failed to get Metal device for null cube "
        "texture");
    return nullptr;
  }

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  // Null cube texture must match non-array cube bindings in translated MSL.
  descriptor->setTextureType(MTL::TextureTypeCube);
  descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  descriptor->setWidth(1);
  descriptor->setHeight(1);
  descriptor->setDepth(1);
  descriptor->setArrayLength(1);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(MTL::StorageModeShared);

  MTL::Texture* texture = device->newTexture(descriptor);
  descriptor->release();  // Immediate release following pattern

  if (texture) {
    // Initialize all 6 faces with black color (0xFF000000 for RGBA8)
    uint32_t default_color = 0xFF000000;
    MTL::Region region = MTL::Region::Make2D(0, 0, 1, 1);
    for (uint32_t face = 0; face < 6; ++face) {
      texture->replaceRegion(region, 0, face, &default_color, 4, 0);
    }
  } else {
    XELOGE("Failed to create null cube texture");
  }

  return texture;  // No retain needed - newTexture returns retained object
}

// RequestTextures override - integrates with standard texture binding pipeline
void MetalTextureCache::RequestTextures(uint32_t used_texture_mask) {
  SCOPE_profile_cpu_f("gpu");

  BeginUploadCommandBufferBatch();

  // Call base class implementation first
  TextureCache::RequestTextures(used_texture_mask);

  EndUploadCommandBufferBatch();

  // Intentionally no Metal-specific per-fetch logging here - invalid fetch
  // constants are already reported by the shared TextureCache logic.
}

MTL::Texture* MetalTextureCache::GetTextureForBinding(
    uint32_t fetch_constant, xenos::FetchOpDimension dimension,
    bool is_signed) {
  static std::array<bool, 32> logged_missing_binding{};
  static std::array<bool, 32> logged_missing_texture{};

  auto get_null_texture_for_dimension = [&]() -> MTL::Texture* {
    switch (dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return null_texture_2d_;
      case xenos::FetchOpDimension::k3DOrStacked:
        return null_texture_3d_;
      case xenos::FetchOpDimension::kCube:
        return null_texture_cube_;
      default:
        return null_texture_2d_;
    }
  };

  const TextureBinding* binding = GetValidTextureBinding(fetch_constant);
  if (!binding) {
    if (fetch_constant < logged_missing_binding.size() &&
        !logged_missing_binding[fetch_constant]) {
      xenos::xe_gpu_texture_fetch_t fetch =
          register_file().GetTextureFetch(fetch_constant);
      TextureKey decoded_key;
      uint8_t swizzled_signs = 0;
      BindingInfoFromFetchConstant(fetch, decoded_key, &swizzled_signs);
      XELOGW(
          "GetTextureForBinding: No valid binding for fetch {} (type={}, "
          "format={}, swizzle=0x{:08X}, dwords={:08X} {:08X} {:08X} {:08X} "
          "{:08X} {:08X})",
          fetch_constant, uint32_t(fetch.type), uint32_t(fetch.format),
          fetch.swizzle, fetch.dword_0, fetch.dword_1, fetch.dword_2,
          fetch.dword_3, fetch.dword_4, fetch.dword_5);
      if (decoded_key.is_valid) {
        const char* format_name = FormatInfo::GetName(decoded_key.format);
        XELOGW(
            "GetTextureForBinding: Decoded fetch {} -> format={} ({}), "
            "size={}x{}x{}, pitch={}, mips={}, tiled={}, packed_mips={}, "
            "endian={}, swizzled_signs=0x{:02X}",
            fetch_constant, uint32_t(decoded_key.format), format_name,
            decoded_key.GetWidth(), decoded_key.GetHeight(),
            decoded_key.GetDepthOrArraySize(), decoded_key.pitch,
            decoded_key.mip_max_level + 1, decoded_key.tiled ? 1 : 0,
            decoded_key.packed_mips ? 1 : 0, uint32_t(decoded_key.endianness),
            swizzled_signs);
      }
      logged_missing_binding[fetch_constant] = true;
    }
    return get_null_texture_for_dimension();
  }

  if (!AreDimensionsCompatible(dimension, binding->key.dimension)) {
    return get_null_texture_for_dimension();
  }

  Texture* texture = nullptr;
  if (is_signed) {
    bool needs_signed_components =
        texture_util::IsAnySignSigned(binding->swizzled_signs);
    if (needs_signed_components &&
        IsSignedVersionSeparateForFormat(binding->key)) {
      texture =
          binding->texture_signed ? binding->texture_signed : binding->texture;
    } else {
      texture = binding->texture;
    }
  } else {
    bool has_unsigned_components =
        texture_util::IsAnySignNotSigned(binding->swizzled_signs);
    if (has_unsigned_components) {
      texture = binding->texture;
    } else if (!has_unsigned_components && binding->texture_signed != nullptr) {
      texture = binding->texture_signed;
    } else {
      texture = binding->texture;
    }
  }

  if (!texture) {
    if (fetch_constant < logged_missing_texture.size() &&
        !logged_missing_texture[fetch_constant]) {
      XELOGW("GetTextureForBinding: No texture object for fetch {}",
             fetch_constant);
      logged_missing_texture[fetch_constant] = true;
    }
    return get_null_texture_for_dimension();
  }

  texture->MarkAsUsed();
  auto* metal_texture = static_cast<MetalTexture*>(texture);
  MTL::Texture* result = nullptr;
  const bool use_3d_as_2d =
      binding->key.dimension == xenos::DataDimension::k3D &&
      (dimension == xenos::FetchOpDimension::k1D ||
       dimension == xenos::FetchOpDimension::k2D);
  if (metal_texture) {
    if (use_3d_as_2d) {
      result = metal_texture->GetOrCreate3DAs2DView(binding->host_swizzle,
                                                    dimension, is_signed);
      if (!result) {
        return get_null_texture_for_dimension();
      }
    } else {
      result = metal_texture->GetOrCreateView(binding->host_swizzle, dimension,
                                              is_signed);
    }
  }
  return result ? result : get_null_texture_for_dimension();
}

MTL::Texture* MetalTextureCache::RequestSwapTexture(
    uint32_t& width_scaled_out, uint32_t& height_scaled_out,
    xenos::TextureFormat& format_out) {
  static bool logged_valid = false;
  static bool logged_invalid = false;
  enum class SwapFailure : uint8_t {
    kCreateTexture = 0,
    kView = 1,
    kLoad = 2,
    kScaledResolve = 3,
    kInvalidPixelFormat = 4,
    kUnsupportedPixelFormat = 5,
    kMissingLoadShader = 6,
    kMissingPipeline = 7,
  };
  auto log_swap_failure_once =
      [&](SwapFailure reason, const TextureKey& log_key, const char* detail) {
        static std::unordered_set<uint64_t> logged_failures;
        uint64_t tag = (uint64_t(reason) << 56) |
                       (uint64_t(log_key.format) << 48) |
                       (uint64_t(log_key.dimension) << 40) |
                       (uint64_t(log_key.scaled_resolve) << 39) |
                       (uint64_t(log_key.endianness) << 37) |
                       (uint64_t(log_key.signed_separate) << 36);
        if (!logged_failures.insert(tag).second) {
          return;
        }
        XELOGW("MetalSwap: request failed: {}", detail);
        XELOGW(
            "MetalSwap: base=0x{:X} mip=0x{:X} {}x{} pitch={} mip_levels={} "
            "format={} dim={} scaled={} endian={} signed={}",
            log_key.base_page << 12, log_key.mip_page << 12, log_key.GetWidth(),
            log_key.GetHeight(), log_key.pitch, log_key.mip_max_level + 1,
            static_cast<uint32_t>(log_key.format),
            static_cast<uint32_t>(log_key.dimension),
            log_key.scaled_resolve ? 1 : 0,
            static_cast<uint32_t>(log_key.endianness),
            log_key.signed_separate ? 1 : 0);
      };

  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 ||
      key.dimension != xenos::DataDimension::k2DOrStacked) {
    if (!logged_invalid) {
      XELOGW("MetalSwap: fetch0 invalid (valid={}, base_page=0x{:X}, dim={})",
             key.is_valid ? 1 : 0, key.base_page,
             static_cast<uint32_t>(key.dimension));
      logged_invalid = true;
    }
    return nullptr;
  }

  auto* texture = static_cast<MetalTexture*>(FindOrCreateTexture(key));
  if (!texture) {
    log_swap_failure_once(SwapFailure::kCreateTexture, key,
                          "failed to create swap texture");
    return nullptr;
  }

  uint32_t host_swizzle =
      GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key));
  MTL::Texture* view = texture->GetOrCreateView(
      host_swizzle, xenos::FetchOpDimension::k2D, false);
  if (!view) {
    log_swap_failure_once(SwapFailure::kView, key,
                          "failed to create swap texture view");
    return nullptr;
  }

  if (!LoadTextureData(*texture)) {
    bool logged_reason = false;
    if (key.scaled_resolve && !IsScaledResolveSupportedForFormat(key)) {
      log_swap_failure_once(SwapFailure::kScaledResolve, key,
                            "scaled resolve not supported");
      logged_reason = true;
    }
    MTL::PixelFormat pixel_format = GetPixelFormatForKey(key);
    if (pixel_format == MTL::PixelFormatInvalid) {
      log_swap_failure_once(SwapFailure::kInvalidPixelFormat, key,
                            "invalid Metal pixel format");
      logged_reason = true;
    }
    MTL::Device* device =
        command_processor_ ? command_processor_->GetMetalDevice() : nullptr;
    if (device && !SupportsPixelFormat(device, pixel_format)) {
      log_swap_failure_once(SwapFailure::kUnsupportedPixelFormat, key,
                            "unsupported Metal pixel format");
      logged_reason = true;
    }
    TextureCache::LoadShaderIndex load_shader = GetLoadShaderIndexForKey(key);
    if (load_shader == TextureCache::kLoadShaderIndexUnknown) {
      log_swap_failure_once(SwapFailure::kMissingLoadShader, key,
                            "missing load shader");
      logged_reason = true;
    } else {
      MTL::ComputePipelineState* load_pipeline =
          key.scaled_resolve
              ? load_pipelines_scaled_[static_cast<size_t>(load_shader)]
              : load_pipelines_[static_cast<size_t>(load_shader)];
      if (!load_pipeline) {
        log_swap_failure_once(SwapFailure::kMissingPipeline, key,
                              "missing load pipeline");
        logged_reason = true;
      }
    }
    if (!logged_reason) {
      log_swap_failure_once(SwapFailure::kLoad, key, "LoadTextureData failed");
    }
    return nullptr;
  }

  texture->MarkAsUsed();
  key = texture->key();
  width_scaled_out =
      key.GetWidth() * (key.scaled_resolve ? draw_resolution_scale_x() : 1);
  height_scaled_out =
      key.GetHeight() * (key.scaled_resolve ? draw_resolution_scale_y() : 1);
  format_out = key.format;
  if (!logged_valid) {
    logged_valid = true;
  }
  return view;
}

// Normalize clamp modes to values Metal supports.
static xenos::ClampMode NormalizeClampModeStatic(xenos::ClampMode clamp_mode) {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    return xenos::ClampMode::kMirrorClampToEdge;
  }
  return clamp_mode;
}

// Shared helper: build SamplerParameters from fetch constant + filter
// overrides.
static MetalTextureCache::SamplerParameters BuildSamplerParametersFromFetch(
    const RegisterFile& regs, uint32_t fetch_constant,
    xenos::TextureFilter req_mag_filter, xenos::TextureFilter req_min_filter,
    xenos::TextureFilter req_mip_filter, xenos::AnisoFilter req_aniso_filter) {
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(fetch_constant);

  MetalTextureCache::SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y,
                                          fetch_clamp_z);
  parameters.clamp_x = NormalizeClampModeStatic(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampModeStatic(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampModeStatic(fetch_clamp_z);

  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  uint32_t mip_min_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr,
                                                 nullptr, nullptr, nullptr,
                                                 &mip_min_level, nullptr);
  parameters.mip_min_level = mip_min_level;

  xenos::AnisoFilter aniso_filter =
      req_aniso_filter == xenos::AnisoFilter::kUseFetchConst
          ? fetch.aniso_filter
          : req_aniso_filter;
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  parameters.aniso_filter = aniso_filter;

  xenos::TextureFilter mip_filter =
      req_mip_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mip_filter
                                                             : req_mip_filter;

  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  } else {
    xenos::TextureFilter mag_filter =
        req_mag_filter == xenos::TextureFilter::kUseFetchConst
            ? fetch.mag_filter
            : req_mag_filter;
    parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;

    xenos::TextureFilter min_filter =
        req_min_filter == xenos::TextureFilter::kUseFetchConst
            ? fetch.min_filter
            : req_min_filter;
    parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;

    parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  }

  parameters.mip_base_map =
      mip_filter == xenos::TextureFilter::kBaseMap ? 1 : 0;

  return parameters;
}

MetalTextureCache::SamplerParameters MetalTextureCache::GetSamplerParameters(
    const SpirvShader::SamplerBinding& binding) const {
  return BuildSamplerParametersFromFetch(
      register_file(), binding.fetch_constant, binding.mag_filter,
      binding.min_filter, binding.mip_filter, binding.aniso_filter);
}

MTL::SamplerState* MetalTextureCache::GetOrCreateSampler(
    SamplerParameters parameters) {
  auto it = sampler_cache_.find(parameters.value);
  if (it != sampler_cache_.end()) {
    return it->second;
  }

  MTL::SamplerDescriptor* desc = MTL::SamplerDescriptor::alloc()->init();
  if (!desc) {
    XELOGE("Failed to allocate Metal sampler descriptor");
    return nullptr;
  }

  auto convert_clamp = [](xenos::ClampMode mode) {
    switch (mode) {
      case xenos::ClampMode::kRepeat:
        return MTL::SamplerAddressModeRepeat;
      case xenos::ClampMode::kMirroredRepeat:
        return MTL::SamplerAddressModeMirrorRepeat;
      case xenos::ClampMode::kClampToEdge:
        return MTL::SamplerAddressModeClampToEdge;
      case xenos::ClampMode::kMirrorClampToEdge:
        return MTL::SamplerAddressModeMirrorClampToEdge;
      case xenos::ClampMode::kClampToBorder:
        return MTL::SamplerAddressModeClampToBorderColor;
      default:
        return MTL::SamplerAddressModeClampToEdge;
    }
  };

  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
    desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
    desc->setMipFilter(MTL::SamplerMipFilterLinear);
    desc->setMaxAnisotropy(1u << (uint32_t(parameters.aniso_filter) - 1));
  } else {
    desc->setMinFilter(parameters.min_linear ? MTL::SamplerMinMagFilterLinear
                                             : MTL::SamplerMinMagFilterNearest);
    desc->setMagFilter(parameters.mag_linear ? MTL::SamplerMinMagFilterLinear
                                             : MTL::SamplerMinMagFilterNearest);
    desc->setMipFilter(parameters.mip_linear ? MTL::SamplerMipFilterLinear
                                             : MTL::SamplerMipFilterNearest);
    desc->setMaxAnisotropy(1);
  }

  desc->setSAddressMode(convert_clamp(xenos::ClampMode(parameters.clamp_x)));
  desc->setTAddressMode(convert_clamp(xenos::ClampMode(parameters.clamp_y)));
  desc->setRAddressMode(convert_clamp(xenos::ClampMode(parameters.clamp_z)));

  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      desc->setBorderColor(MTL::SamplerBorderColorOpaqueWhite);
      break;
    case xenos::BorderColor::k_ABGR_Black:
      desc->setBorderColor(MTL::SamplerBorderColorTransparentBlack);
      break;
    default:
      desc->setBorderColor(MTL::SamplerBorderColorOpaqueBlack);
      break;
  }

  desc->setLodMinClamp(static_cast<float>(parameters.mip_min_level));
  float max_lod = parameters.mip_base_map
                      ? static_cast<float>(parameters.mip_min_level)
                      : FLT_MAX;
  if (parameters.mip_base_map &&
      parameters.aniso_filter == xenos::AnisoFilter::kDisabled &&
      !parameters.mip_linear) {
    max_lod += 0.25f;
  }
  desc->setLodMaxClamp(max_lod);
  desc->setLodAverage(false);
  desc->setSupportArgumentBuffers(true);

  MTL::SamplerState* sampler_state =
      command_processor_->GetMetalDevice()->newSamplerState(desc);
  desc->release();

  if (!sampler_state) {
    XELOGE("Failed to create Metal sampler state");
    return nullptr;
  }

  sampler_cache_.emplace(parameters.value, sampler_state);
  return sampler_state;
}

xenos::ClampMode MetalTextureCache::NormalizeClampMode(
    xenos::ClampMode clamp_mode) const {
  return NormalizeClampModeStatic(clamp_mode);
}

// GetHostFormatSwizzle implementation
uint32_t MetalTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  switch (key.format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_DXT5A:
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_EXPAND:
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;

    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_32_32_FLOAT:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;

    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB;

    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_2_10_10_10:
      // Stored as BGRA after endian swap; CPU path swaps to RGBA8.
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;

    default:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
}

bool MetalTextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  switch (key.format) {
    case xenos::TextureFormat::k_16:
      return r16_selection_.unsigned_uses_float ||
             r16_selection_.signed_uses_float;
    case xenos::TextureFormat::k_16_16:
      return rg16_selection_.unsigned_uses_float ||
             rg16_selection_.signed_uses_float;
    case xenos::TextureFormat::k_16_16_16_16:
      return rgba16_selection_.unsigned_uses_float ||
             rgba16_selection_.signed_uses_float;
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
      return true;
    default:
      return false;
  }
}

bool MetalTextureCache::IsScaledResolveSupportedForFormat(
    TextureKey key) const {
  LoadShaderIndex load_shader = GetLoadShaderIndexForKey(key);
  return load_shader != kLoadShaderIndexUnknown &&
         load_pipelines_scaled_[load_shader] != nullptr;
}

bool MetalTextureCache::EnsureScaledResolveMemoryCommitted(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }
  if (!length_unscaled) {
    return true;
  }

  uint64_t start_scaled = 0;
  uint64_t length_scaled = 0;
  if (!GetScaledResolveRange(start_unscaled, length_unscaled,
                             length_scaled_alignment_log2, start_scaled,
                             length_scaled)) {
    return false;
  }
  return EnsureScaledResolveBufferRange(start_scaled, length_scaled);
}

bool MetalTextureCache::MakeScaledResolveRangeCurrent(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }
  if (!length_unscaled) {
    return false;
  }

  uint64_t start_scaled = 0;
  uint64_t length_scaled = 0;
  if (!GetScaledResolveRange(start_unscaled, length_unscaled,
                             length_scaled_alignment_log2, start_scaled,
                             length_scaled)) {
    return false;
  }
  if (!length_scaled) {
    return false;
  }

  uint64_t end_scaled = start_scaled + length_scaled - 1;
  for (size_t i = scaled_resolve_buffers_.size(); i-- > 0;) {
    const ScaledResolveBuffer& buffer = scaled_resolve_buffers_[i];
    uint64_t buffer_end = buffer.base_scaled + buffer.length_scaled - 1;
    if (start_scaled >= buffer.base_scaled && end_scaled <= buffer_end) {
      scaled_resolve_current_buffer_index_ = i;
      scaled_resolve_current_range_start_scaled_ = start_scaled;
      scaled_resolve_current_range_length_scaled_ = length_scaled;
      return true;
    }
  }
  return false;
}

bool MetalTextureCache::GetCurrentScaledResolveBuffer(
    MTL::Buffer*& buffer_out, size_t& buffer_offset_out,
    size_t& buffer_length_out) const {
  if (scaled_resolve_current_buffer_index_ == size_t(-1) ||
      scaled_resolve_current_buffer_index_ >= scaled_resolve_buffers_.size()) {
    return false;
  }
  const ScaledResolveBuffer& buffer =
      scaled_resolve_buffers_[scaled_resolve_current_buffer_index_];
  uint64_t offset =
      scaled_resolve_current_range_start_scaled_ - buffer.base_scaled;
  uint64_t end_offset = offset + scaled_resolve_current_range_length_scaled_;
  if (end_offset > buffer.length_scaled) {
    return false;
  }
  if (offset > std::numeric_limits<size_t>::max() ||
      scaled_resolve_current_range_length_scaled_ >
          std::numeric_limits<size_t>::max()) {
    return false;
  }
  buffer_out = buffer.buffer;
  buffer_offset_out = size_t(offset);
  buffer_length_out = size_t(scaled_resolve_current_range_length_scaled_);
  return buffer_out != nullptr;
}

bool MetalTextureCache::GetScaledResolveRange(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2, uint64_t& start_scaled_out,
    uint64_t& length_scaled_out) const {
  if (!length_unscaled) {
    start_scaled_out = 0;
    length_scaled_out = 0;
    return true;
  }
  if (start_unscaled >= SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    return false;
  }

  uint32_t scale_area = draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t start_scaled = uint64_t(start_unscaled) * scale_area;
  uint64_t end_scaled =
      uint64_t(start_unscaled + (length_unscaled - 1)) * scale_area;
  if (length_scaled_alignment_log2) {
    uint64_t alignment_mask = (uint64_t(1) << length_scaled_alignment_log2) - 1;
    end_scaled = (end_scaled + alignment_mask) & ~alignment_mask;
  }
  start_scaled_out = start_scaled;
  length_scaled_out = end_scaled - start_scaled + 1;
  return true;
}

bool MetalTextureCache::IsScaledResolveRangeResident(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) const {
  if (!IsDrawResolutionScaled() || !length_unscaled) {
    return false;
  }

  uint64_t start_scaled = 0;
  uint64_t length_scaled = 0;
  if (!GetScaledResolveRange(start_unscaled, length_unscaled,
                             length_scaled_alignment_log2, start_scaled,
                             length_scaled) ||
      !length_scaled) {
    return false;
  }

  for (const ScaledResolveBuffer& buffer : scaled_resolve_buffers_) {
    if (!buffer.buffer || !buffer.length_scaled ||
        start_scaled < buffer.base_scaled) {
      continue;
    }
    uint64_t buffer_offset = start_scaled - buffer.base_scaled;
    if (buffer_offset > buffer.length_scaled) {
      continue;
    }
    uint64_t buffer_remaining = buffer.length_scaled - buffer_offset;
    if (length_scaled <= buffer_remaining) {
      return true;
    }
  }
  return false;
}

bool MetalTextureCache::EnsureScaledResolveBufferRange(uint64_t start_scaled,
                                                       uint64_t length_scaled) {
  if (!length_scaled) {
    return true;
  }
  uint64_t end_scaled = start_scaled + length_scaled - 1;

  for (const ScaledResolveBuffer& buffer : scaled_resolve_buffers_) {
    uint64_t buffer_end = buffer.base_scaled + buffer.length_scaled - 1;
    if (start_scaled >= buffer.base_scaled && end_scaled <= buffer_end) {
      return true;
    }
  }

  uint64_t new_base_scaled = start_scaled;
  uint64_t new_end_scaled = end_scaled;
  std::vector<size_t> overlap_indices;
  overlap_indices.reserve(scaled_resolve_buffers_.size());

  for (size_t i = 0; i < scaled_resolve_buffers_.size(); ++i) {
    const ScaledResolveBuffer& buffer = scaled_resolve_buffers_[i];
    uint64_t buffer_end = buffer.base_scaled + buffer.length_scaled - 1;
    if (buffer.base_scaled <= end_scaled && buffer_end >= start_scaled) {
      overlap_indices.push_back(i);
      new_base_scaled = std::min(new_base_scaled, buffer.base_scaled);
      new_end_scaled = std::max(new_end_scaled, buffer_end);
    }
  }

  uint64_t new_length_scaled = new_end_scaled - new_base_scaled + 1;
  new_length_scaled = xe::align(new_length_scaled, uint64_t(16));
  if (new_length_scaled > std::numeric_limits<size_t>::max()) {
    XELOGE("Metal scaled resolve: buffer size too large ({} bytes)",
           new_length_scaled);
    return false;
  }

  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE("Metal scaled resolve: missing Metal device");
    return false;
  }
  if (new_length_scaled > device->maxBufferLength()) {
    XELOGE("Metal scaled resolve: requested {} bytes exceeds maxBufferLength",
           new_length_scaled);
    return false;
  }

  MTL::Buffer* new_buffer = device->newBuffer(size_t(new_length_scaled),
                                              MTL::ResourceStorageModePrivate);
  if (!new_buffer) {
    XELOGE("Metal scaled resolve: failed to allocate {} bytes",
           new_length_scaled);
    return false;
  }
  new_buffer->setLabel(
      NS::String::string("XeniaScaledResolveBuffer", NS::UTF8StringEncoding));

  uint64_t overlap_total_bytes = 0;
  for (size_t overlap_index : overlap_indices) {
    overlap_total_bytes += scaled_resolve_buffers_[overlap_index].length_scaled;
  }
  MTL::CommandBuffer* current_cmd =
      command_processor_->GetCurrentCommandBuffer();
  bool retain_overlaps = current_cmd != nullptr;
  if (retain_overlaps) {
    bool exceeds_retired_budget =
        overlap_total_bytes > kScaledResolveRetiredMaxBytes ||
        scaled_resolve_retired_bytes_ >
            (kScaledResolveRetiredMaxBytes - overlap_total_bytes);
    if (exceeds_retired_budget) {
      retain_overlaps = false;
    }
  }

  if (!overlap_indices.empty()) {
    MTL::CommandBuffer* cmd = retain_overlaps ? current_cmd : nullptr;
    if (!cmd) {
      MTL::CommandQueue* queue = command_processor_->GetMetalCommandQueue();
      if (!queue) {
        new_buffer->release();
        return false;
      }
      cmd = queue->commandBuffer();
      if (!cmd) {
        new_buffer->release();
        return false;
      }
    }

    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    if (!blit) {
      new_buffer->release();
      return false;
    }

    for (size_t index : overlap_indices) {
      const ScaledResolveBuffer& old_buffer = scaled_resolve_buffers_[index];
      uint64_t dst_offset = old_buffer.base_scaled - new_base_scaled;
      if (dst_offset > std::numeric_limits<size_t>::max()) {
        continue;
      }
      blit->copyFromBuffer(old_buffer.buffer, 0, new_buffer, size_t(dst_offset),
                           size_t(old_buffer.length_scaled));
    }

    blit->endEncoding();
    if (cmd != current_cmd) {
      cmd->commit();
      cmd->waitUntilCompleted();
    }
  }

  std::vector<ScaledResolveBuffer> new_buffers;
  new_buffers.reserve(scaled_resolve_buffers_.size() - overlap_indices.size() +
                      1);
  for (size_t i = 0; i < scaled_resolve_buffers_.size(); ++i) {
    bool overlapping = false;
    for (size_t overlap_index : overlap_indices) {
      if (overlap_index == i) {
        overlapping = true;
        break;
      }
    }
    if (overlapping) {
      if (retain_overlaps) {
        RetiredScaledResolveBuffer retired;
        retired.buffer = scaled_resolve_buffers_[i].buffer;
        retired.submission_id = command_processor_->GetCurrentSubmission();
        retired.length_scaled = scaled_resolve_buffers_[i].length_scaled;
        scaled_resolve_retired_buffers_.push_back(retired);
        if (std::numeric_limits<uint64_t>::max() -
                scaled_resolve_retired_bytes_ <
            retired.length_scaled) {
          scaled_resolve_retired_bytes_ = std::numeric_limits<uint64_t>::max();
        } else {
          scaled_resolve_retired_bytes_ += retired.length_scaled;
        }
      } else if (scaled_resolve_buffers_[i].buffer) {
        scaled_resolve_buffers_[i].buffer->release();
      }
      continue;
    }
    new_buffers.push_back(scaled_resolve_buffers_[i]);
  }

  ScaledResolveBuffer new_entry;
  new_entry.buffer = new_buffer;
  new_entry.base_scaled = new_base_scaled;
  new_entry.length_scaled = new_length_scaled;
  new_buffers.push_back(new_entry);

  scaled_resolve_buffers_.swap(new_buffers);
  scaled_resolve_current_buffer_index_ = size_t(-1);
  scaled_resolve_current_range_start_scaled_ = 0;
  scaled_resolve_current_range_length_scaled_ = 0;

  return true;
}

// GetMaxHostTextureWidthHeight implementation
uint32_t MetalTextureCache::GetMaxHostTextureWidthHeight(
    xenos::DataDimension dimension) const {
  // Metal supports up to 16384x16384 for 2D textures on most devices
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return 16384;
    case xenos::DataDimension::k2DOrStacked:
      return 16384;
    case xenos::DataDimension::k3D:
      return 2048;  // 3D textures have lower limits
    case xenos::DataDimension::kCube:
      return 16384;
    default:
      return 16384;
  }
}

// GetMaxHostTextureDepthOrArraySize implementation
uint32_t MetalTextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  // Metal array and 3D texture limits
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return 2048;  // Array size limit
    case xenos::DataDimension::k2DOrStacked:
      return 2048;  // Array size limit
    case xenos::DataDimension::k3D:
      return 2048;  // Depth limit for 3D textures
    case xenos::DataDimension::kCube:
      return 2048;  // Array size for cube arrays
    default:
      return 2048;
  }
}

// CreateTexture implementation - creates MetalTexture from TextureKey
std::unique_ptr<TextureCache::Texture> MetalTextureCache::CreateTexture(
    TextureKey key) {
  SCOPE_profile_cpu_f("gpu");

  MTL::PixelFormat metal_format = GetPixelFormatForKey(key);
  if (metal_format == MTL::PixelFormatInvalid) {
    XELOGE("CreateTexture: Unsupported texture format {}",
           static_cast<uint32_t>(key.format));
    return nullptr;
  }

  MTL::TextureSwizzleChannels metal_swizzle =
      ToMetalTextureSwizzle(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);

  MTL::Texture* metal_texture = nullptr;
  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  if (key.scaled_resolve) {
    width *= draw_resolution_scale_x();
    height *= draw_resolution_scale_y();
  }

  // Create Metal texture based on dimension
  switch (key.dimension) {
    case xenos::DataDimension::k1D: {
      metal_texture =
          CreateTexture2D(width, height, key.GetDepthOrArraySize(),
                          metal_format, metal_swizzle, key.mip_max_level + 1);
      break;
    }
    case xenos::DataDimension::k2DOrStacked: {
      metal_texture =
          CreateTexture2D(width, height, key.GetDepthOrArraySize(),
                          metal_format, metal_swizzle, key.mip_max_level + 1);
      break;
    }
    case xenos::DataDimension::k3D: {
      metal_texture =
          CreateTexture3D(width, height, key.GetDepthOrArraySize(),
                          metal_format, metal_swizzle, key.mip_max_level + 1);
      break;
    }
    case xenos::DataDimension::kCube: {
      uint32_t array_size = key.GetDepthOrArraySize();
      if (array_size % 6 != 0) {
        XELOGW(
            "CreateTexture: Cube texture array size {} is not divisible by 6",
            array_size);
      }
      uint32_t cube_count = std::max(1u, array_size / 6);
      metal_texture = CreateTextureCube(width, metal_format, metal_swizzle,
                                        key.mip_max_level + 1, cube_count);
      break;
    }
    default: {
      XELOGE("CreateTexture: Unsupported texture dimension {}",
             static_cast<uint32_t>(key.dimension));
      return nullptr;
    }
  }

  if (!metal_texture) {
    XELOGE("CreateTexture: Failed to create Metal texture");
    return nullptr;
  }

  // Create MetalTexture wrapper
  return std::make_unique<MetalTexture>(*this, key, metal_texture);
}

// LoadTextureDataFromResidentMemoryImpl implementation
bool MetalTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture,
                                                              bool load_base,
                                                              bool load_mips) {
  SCOPE_profile_cpu_f("gpu");

  MetalTexture* metal_texture = static_cast<MetalTexture*>(&texture);
  if (!metal_texture || !metal_texture->metal_texture()) {
    XELOGE("LoadTextureDataFromResidentMemoryImpl: Invalid Metal texture");
    return false;
  }

  // GPU-based loading path for Metal texture_load_* shaders only (parity with
  // D3D12/Vulkan; no CPU untile fallback).
  return TryGpuLoadTexture(texture, load_base, load_mips);
}

// MetalTexture implementation
MetalTextureCache::MetalTexture::MetalTexture(MetalTextureCache& texture_cache,
                                              const TextureKey& key,
                                              MTL::Texture* metal_texture,
                                              bool track_usage)
    : Texture(texture_cache, key, track_usage),
      texture_cache_(texture_cache),
      metal_texture_(metal_texture) {
  if (metal_texture_) {
    SetHostMemoryUsage(EstimateTextureBytes(metal_texture_));
  }
}

MetalTextureCache::MetalTexture::~MetalTexture() {
  uint64_t views_released = 0;
  for (auto& entry : swizzled_view_cache_) {
    if (entry.second) {
      ++views_released;
      entry.second->release();
    }
  }
  if (metal_texture_) {
    metal_texture_->release();
    metal_texture_ = nullptr;
  }
}

MTL::Texture* MetalTextureCache::MetalTexture::GetOrCreateView(
    uint32_t host_swizzle, xenos::FetchOpDimension dimension, bool is_signed) {
  if (!metal_texture_) {
    return nullptr;
  }

  auto get_view_pixel_format =
      [&](const TextureKey& key, bool view_signed,
          MTL::PixelFormat base_format) -> MTL::PixelFormat {
    if (!view_signed) {
      return base_format;
    }
    switch (key.format) {
      case xenos::TextureFormat::k_8:
      case xenos::TextureFormat::k_8_A:
      case xenos::TextureFormat::k_8_B:
        return MTL::PixelFormatR8Snorm;
      case xenos::TextureFormat::k_8_8:
        return MTL::PixelFormatRG8Snorm;
      case xenos::TextureFormat::k_8_8_8_8:
      case xenos::TextureFormat::k_8_8_8_8_A:
        return MTL::PixelFormatRGBA8Snorm;
      case xenos::TextureFormat::k_16:
        return MTL::PixelFormatR16Snorm;
      case xenos::TextureFormat::k_16_16:
        return MTL::PixelFormatRG16Snorm;
      case xenos::TextureFormat::k_16_16_16_16:
        return MTL::PixelFormatRGBA16Snorm;
      default:
        return base_format;
    }
  };

  MTL::PixelFormat view_format =
      get_view_pixel_format(key(), is_signed, metal_texture_->pixelFormat());

  MTL::TextureType view_type = metal_texture_->textureType();
  switch (dimension) {
    case xenos::FetchOpDimension::kCube:
      // SPIRV-Cross translates cube fetches to non-array cube textures.
      view_type = MTL::TextureTypeCube;
      break;
    case xenos::FetchOpDimension::k3DOrStacked:
      view_type = key().dimension == xenos::DataDimension::k3D
                      ? MTL::TextureType3D
                      : MTL::TextureType2DArray;
      break;
    default:
      view_type = MTL::TextureType2DArray;
      break;
  }

  if (host_swizzle == xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA) {
    if ((!is_signed || view_format == metal_texture_->pixelFormat()) &&
        metal_texture_->textureType() == view_type) {
      return metal_texture_;
    }
  }

  uint64_t view_key = uint64_t(host_swizzle) | (uint64_t(dimension) << 32) |
                      (uint64_t(is_signed) << 40) |
                      (uint64_t(view_format) << 48);
  auto found = swizzled_view_cache_.find(view_key);
  if (found != swizzled_view_cache_.end()) {
    return found->second;
  }

  uint32_t slice_count = 1;
  switch (view_type) {
    case MTL::TextureType2DArray:
      slice_count = metal_texture_->arrayLength();
      break;
    case MTL::TextureTypeCube:
      slice_count = 6;
      break;
    case MTL::TextureTypeCubeArray:
      slice_count = metal_texture_->arrayLength() * 6;
      break;
    case MTL::TextureType3D:
      // Metal requires a single slice range for 3D texture views.
      slice_count = 1;
      break;
    default:
      slice_count = 1;
      break;
  }

  NS::Range level_range =
      NS::Range::Make(0, metal_texture_->mipmapLevelCount());
  NS::Range slice_range = NS::Range::Make(0, slice_count);
  MTL::TextureSwizzleChannels swizzle = ToMetalTextureSwizzle(host_swizzle);
  MTL::Texture* view = metal_texture_->newTextureView(
      view_format, view_type, level_range, slice_range, swizzle);
  if (!view) {
    // Returning a mismatched base type can trigger Metal validation asserts.
    return metal_texture_->textureType() == view_type ? metal_texture_
                                                      : nullptr;
  }

  swizzled_view_cache_.emplace(view_key, view);
  return view;
}

MTL::Texture* MetalTextureCache::MetalTexture::GetOrCreate3DAs2DView(
    uint32_t host_swizzle, xenos::FetchOpDimension dimension, bool is_signed) {
  if (!metal_texture_ || key().dimension != xenos::DataDimension::k3D) {
    return nullptr;
  }
  if (!::cvars::gpu_3d_to_2d_texture) {
    return nullptr;
  }

  if (!texture_3d_as_2d_) {
    TextureKey key_2d = key();
    key_2d.depth_or_array_size_minus_1 = 0;
    key_2d.mip_max_level = 0;

    uint32_t width = key_2d.GetWidth();
    uint32_t height = key_2d.GetHeight();
    if (key_2d.scaled_resolve && texture_cache_.IsDrawResolutionScaled()) {
      width *= texture_cache_.draw_resolution_scale_x();
      height *= texture_cache_.draw_resolution_scale_y();
    }

    MTL::TextureSwizzleChannels metal_swizzle =
        ToMetalTextureSwizzle(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
    MTL::Texture* texture_2d = texture_cache_.CreateTexture2D(
        width, height, 1, metal_texture_->pixelFormat(), metal_swizzle, 1);
    if (!texture_2d) {
      XELOGE("MetalTexture: Failed to create 3D-as-2D wrapper texture");
      return nullptr;
    }

    texture_3d_as_2d_ = std::make_unique<MetalTexture>(texture_cache_, key_2d,
                                                       texture_2d, false);
    texture_3d_as_2d_->SetForceLoad3DTiling(true);
    if (!texture_cache_.LoadTextureData(*texture_3d_as_2d_)) {
      XELOGE("MetalTexture: Failed to load 3D-as-2D texture data");
      texture_3d_as_2d_.reset();
      return nullptr;
    }
  }

  return texture_3d_as_2d_->GetOrCreateView(host_swizzle, dimension, is_signed);
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
