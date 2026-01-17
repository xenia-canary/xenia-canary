/**
 *******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 *******************************************************************************
 */

#include "xenia/gpu/metal/metal_resource_tracker.h"

#include <algorithm>
#include <atomic>

namespace xe {
namespace gpu {
namespace metal {

namespace {

std::atomic<uint64_t> g_buffers_created{0};
std::atomic<uint64_t> g_buffers_released{0};
std::atomic<uint64_t> g_buffer_bytes_created{0};
std::atomic<uint64_t> g_buffer_bytes_released{0};
std::atomic<uint64_t> g_heap_bytes_created{0};
std::atomic<uint64_t> g_heap_bytes_released{0};
std::atomic<uint64_t> g_textures_created{0};
std::atomic<uint64_t> g_textures_released{0};
std::atomic<uint64_t> g_texture_bytes_created{0};
std::atomic<uint64_t> g_texture_bytes_released{0};

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

}  // namespace

void TrackMetalBufferCreated(size_t bytes) {
  g_buffers_created.fetch_add(1, std::memory_order_relaxed);
  g_buffer_bytes_created.fetch_add(bytes, std::memory_order_relaxed);
}

void TrackMetalBufferReleased(size_t bytes) {
  g_buffers_released.fetch_add(1, std::memory_order_relaxed);
  g_buffer_bytes_released.fetch_add(bytes, std::memory_order_relaxed);
}

void TrackMetalHeapCreated(size_t bytes) {
  g_heap_bytes_created.fetch_add(bytes, std::memory_order_relaxed);
}

void TrackMetalHeapReleased(size_t bytes) {
  g_heap_bytes_released.fetch_add(bytes, std::memory_order_relaxed);
}

void TrackMetalTextureCreated(MTL::Texture* texture) {
  if (!texture) {
    return;
  }
  g_textures_created.fetch_add(1, std::memory_order_relaxed);
  g_texture_bytes_created.fetch_add(EstimateTextureBytes(texture),
                                    std::memory_order_relaxed);
}

void TrackMetalTextureReleased(MTL::Texture* texture) {
  if (!texture) {
    return;
  }
  g_textures_released.fetch_add(1, std::memory_order_relaxed);
  g_texture_bytes_released.fetch_add(EstimateTextureBytes(texture),
                                     std::memory_order_relaxed);
}

uint64_t EstimateMetalTextureBytes(MTL::Texture* texture) {
  return EstimateTextureBytes(texture);
}

MetalResourceStats GetMetalResourceStats() {
  MetalResourceStats stats;
  stats.buffers_created = g_buffers_created.load(std::memory_order_relaxed);
  stats.buffers_released = g_buffers_released.load(std::memory_order_relaxed);
  stats.buffer_bytes_created =
      g_buffer_bytes_created.load(std::memory_order_relaxed);
  stats.buffer_bytes_released =
      g_buffer_bytes_released.load(std::memory_order_relaxed);
  stats.heap_bytes_created =
      g_heap_bytes_created.load(std::memory_order_relaxed);
  stats.heap_bytes_released =
      g_heap_bytes_released.load(std::memory_order_relaxed);
  stats.textures_created = g_textures_created.load(std::memory_order_relaxed);
  stats.textures_released = g_textures_released.load(std::memory_order_relaxed);
  stats.texture_bytes_created =
      g_texture_bytes_created.load(std::memory_order_relaxed);
  stats.texture_bytes_released =
      g_texture_bytes_released.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
