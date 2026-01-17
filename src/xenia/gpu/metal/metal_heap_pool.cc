/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_heap_pool.h"

#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/metal/metal_resource_tracker.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

constexpr size_t kDefaultMaxHeapBytes = 512ull * 1024ull * 1024ull;
constexpr size_t kMinMaxHeapBytes = 256ull * 1024ull * 1024ull;
constexpr size_t kMaxMaxHeapBytes = 1024ull * 1024ull * 1024ull;

size_t GetMaxHeapBytes(MTL::Device* device) {
  if (!device) {
    return kDefaultMaxHeapBytes;
  }
  uint64_t recommended = device->recommendedMaxWorkingSetSize();
  if (!recommended) {
    return kDefaultMaxHeapBytes;
  }
  uint64_t budget = recommended / 4;
  budget = std::max<uint64_t>(budget, kMinMaxHeapBytes);
  budget = std::min<uint64_t>(budget, kMaxMaxHeapBytes);
  return static_cast<size_t>(budget);
}

}  // namespace

MetalHeapPool::MetalHeapPool(MTL::Device* device, MTL::StorageMode storage_mode,
                             size_t min_heap_size, const char* label_prefix)
    : device_(device),
      storage_mode_(storage_mode),
      min_heap_size_(min_heap_size),
      max_heap_bytes_(GetMaxHeapBytes(device)),
      label_prefix_(label_prefix ? label_prefix : "") {}

MetalHeapPool::~MetalHeapPool() { Shutdown(); }

void MetalHeapPool::Shutdown() {
  for (auto& entry : heaps_) {
    if (entry.heap) {
      TrackMetalHeapReleased(entry.size);
      entry.heap->release();
      entry.heap = nullptr;
    }
  }
  heaps_.clear();
  total_heap_bytes_ = 0;
}

MTL::Texture* MetalHeapPool::CreateTexture(MTL::TextureDescriptor* descriptor) {
  if (!device_ || !descriptor) {
    return nullptr;
  }
  MTL::SizeAndAlign size_align = device_->heapTextureSizeAndAlign(descriptor);
  if (!size_align.size || !size_align.align) {
    return nullptr;
  }

  MTL::Heap* heap =
      GetHeapForSize(size_t(size_align.size), size_t(size_align.align));
  if (!heap) {
    return nullptr;
  }
  return heap->newTexture(descriptor);
}

MTL::Heap* MetalHeapPool::GetHeapForSize(size_t size, size_t alignment) {
  for (auto& entry : heaps_) {
    if (!entry.heap) {
      continue;
    }
    size_t available = size_t(
        entry.heap->maxAvailableSize(static_cast<NS::UInteger>(alignment)));
    if (available >= size) {
      return entry.heap;
    }
  }

  size_t heap_size = std::max(size, min_heap_size_);
  heap_size = xe::next_pow2(heap_size);
  heap_size = xe::round_up(heap_size, alignment);
  if (max_heap_bytes_ && heap_size > max_heap_bytes_) {
    return nullptr;
  }
  if (max_heap_bytes_ && total_heap_bytes_ > max_heap_bytes_ - heap_size) {
    return nullptr;
  }

  MTL::HeapDescriptor* desc = MTL::HeapDescriptor::alloc()->init();
  desc->setStorageMode(storage_mode_);
  desc->setHazardTrackingMode(MTL::HazardTrackingModeTracked);
  desc->setSize(heap_size);

  MTL::Heap* heap = device_->newHeap(desc);
  desc->release();
  if (!heap) {
    XELOGE("MetalHeapPool: failed to create heap ({} bytes)", heap_size);
    return nullptr;
  }
  if (!label_prefix_.empty()) {
    std::string label =
        label_prefix_ + "_heap_" + std::to_string(heaps_.size());
    heap->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
  }

  heaps_.push_back({heap, heap_size});
  total_heap_bytes_ += heap_size;
  TrackMetalHeapCreated(heap_size);
  return heap;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
