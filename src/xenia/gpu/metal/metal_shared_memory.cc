#include "xenia/gpu/metal/metal_shared_memory.h"

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_resource_tracker.h"
#include "xenia/ui/metal/metal_util.h"

namespace xe {
namespace gpu {
namespace metal {

MetalSharedMemory::MetalSharedMemory(MetalCommandProcessor& command_processor,
                                     Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

MetalSharedMemory::~MetalSharedMemory() { Shutdown(); }

bool MetalSharedMemory::Initialize() {
  // Try to alias guest memory on unified-memory devices and fall back to a
  // dedicated shared buffer when not supported.
  // Initialize base class
  InitializeCommon();

  const ui::metal::MetalProvider& provider =
      command_processor_.GetMetalProvider();
  MTL::Device* device = provider.GetDevice();

  if (!device) {
    XELOGE("Metal device is null in MetalSharedMemory::Initialize");
    return false;
  }

  // Create Metal buffer - similar to D3D12's approach
  // On Apple Silicon, ResourceStorageModeShared gives CPU/GPU access
  void* xbox_ram = memory().TranslatePhysical(0);
  if (!xbox_ram) {
    XELOGE("Metal shared memory: Xbox RAM is null");
    return false;
  }

  if (cvars::metal_shared_memory_zero_copy && device->hasUnifiedMemory()) {
    size_t system_page_size = xe::memory::page_size();
    if (reinterpret_cast<uintptr_t>(xbox_ram) % system_page_size == 0) {
      buffer_ = device->newBuffer(xbox_ram, kBufferSize,
                                  MTL::ResourceStorageModeShared, nullptr);
      if (buffer_) {
        use_zero_copy_ = true;
        XELOGD("Metal shared memory: using bytes-no-copy buffer");
      } else {
        XELOGW("Metal shared memory: bytes-no-copy buffer creation failed");
      }
    } else {
      XELOGW(
          "Metal shared memory: Xbox RAM not page-aligned for bytes-no-copy");
    }
  }

  if (!buffer_) {
    buffer_ = device->newBuffer(kBufferSize, MTL::ResourceStorageModeShared);
  }
  if (!buffer_) {
    XELOGE("Failed to create Metal shared memory buffer");
    return false;
  }
  TrackMetalBufferCreated(kBufferSize);

  // For trace dump, do initial full copy; UploadRanges handles incremental
  // updates for normal runs.
  if (!use_zero_copy_) {
    if (xbox_ram) {
      memcpy(buffer_->contents(), xbox_ram, kBufferSize);
    }
  } else {
    XELOGD("Metal shared memory: skipping initial copy (zero-copy)");
  }

  return true;
}

void MetalSharedMemory::ClearCache() { SharedMemory::ClearCache(); }

bool MetalSharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  // Copy modified ranges from Xbox memory to Metal buffer when not using
  // bytes-no-copy shared memory.

  static bool first_upload = true;
  if (first_upload) {
    first_upload = false;
    const uint32_t page_size = 1u << page_size_log2();
    XELOGD("MetalSharedMemory::UploadRanges: page_size={}, {} ranges to upload",
           page_size, num_upload_ranges);
    for (uint32_t i = 0; i < std::min<uint32_t>(5, num_upload_ranges); ++i) {
      uint32_t start_byte = upload_page_ranges[i].first * page_size;
      uint32_t length_bytes = upload_page_ranges[i].second * page_size;
      XELOGD("  Range[{}]: page={} count={} -> byte offset=0x{:08X} length={}",
             i, upload_page_ranges[i].first, upload_page_ranges[i].second,
             start_byte, length_bytes);
    }
  }

  if (!buffer_ || !num_upload_ranges) {
    return true;
  }

  uint8_t* buffer_data = nullptr;
  uint8_t* xbox_data = nullptr;
  if (!use_zero_copy_) {
    void* xbox_ram = memory().TranslatePhysical(0);
    if (!xbox_ram) {
      XELOGE("MetalSharedMemory::UploadRanges: Xbox RAM is null");
      return false;
    }
    buffer_data = static_cast<uint8_t*>(buffer_->contents());
    xbox_data = static_cast<uint8_t*>(xbox_ram);
  }

  const uint32_t page_size = 1u << page_size_log2();

  uint32_t merged_start = 0;
  uint32_t merged_end = 0;
  bool have_merged = false;

  auto flush_merged_range = [&](uint32_t start, uint32_t end) {
    if (end <= start) {
      return;
    }
    uint32_t length = end - start;
    MakeRangeValid(start, length, false, false);
    if (!use_zero_copy_) {
      memcpy(buffer_data + start, xbox_data + start, length);
    }
  };

  for (uint32_t i = 0; i < num_upload_ranges; ++i) {
    const auto& range = upload_page_ranges[i];
    uint32_t start = range.first * page_size;
    uint32_t end = start + range.second * page_size;
    if (start >= kBufferSize) {
      continue;
    }
    if (end > kBufferSize) {
      end = kBufferSize;
    }

    if (!have_merged) {
      merged_start = start;
      merged_end = end;
      have_merged = true;
      continue;
    }

    // Merge overlapping/adjacent ranges.
    if (start <= merged_end) {
      if (end > merged_end) {
        merged_end = end;
      }
    } else {
      flush_merged_range(merged_start, merged_end);
      merged_start = start;
      merged_end = end;
    }
  }

  if (have_merged) {
    flush_merged_range(merged_start, merged_end);
  }

  XELOGD("MetalSharedMemory::UploadRanges: Copied {} ranges to Metal buffer",
         num_upload_ranges);

  return true;
}

void MetalSharedMemory::Shutdown() {
  if (buffer_) {
    TrackMetalBufferReleased(buffer_->length());
    buffer_->release();
    buffer_ = nullptr;
  }
  use_zero_copy_ = false;

  ShutdownCommon();  // Base class cleanup
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
