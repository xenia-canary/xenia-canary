/**
 *******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 *******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_RESOURCE_TRACKER_H_
#define XENIA_GPU_METAL_METAL_RESOURCE_TRACKER_H_

#include <cstddef>
#include <cstdint>

#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

struct MetalResourceStats {
  uint64_t buffers_created = 0;
  uint64_t buffers_released = 0;
  uint64_t buffer_bytes_created = 0;
  uint64_t buffer_bytes_released = 0;
  uint64_t heap_bytes_created = 0;
  uint64_t heap_bytes_released = 0;
  uint64_t textures_created = 0;
  uint64_t textures_released = 0;
  uint64_t texture_bytes_created = 0;
  uint64_t texture_bytes_released = 0;
};

void TrackMetalBufferCreated(size_t bytes);
void TrackMetalBufferReleased(size_t bytes);
void TrackMetalHeapCreated(size_t bytes);
void TrackMetalHeapReleased(size_t bytes);
void TrackMetalTextureCreated(MTL::Texture* texture);
void TrackMetalTextureReleased(MTL::Texture* texture);
uint64_t EstimateMetalTextureBytes(MTL::Texture* texture);
MetalResourceStats GetMetalResourceStats();

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_RESOURCE_TRACKER_H_
