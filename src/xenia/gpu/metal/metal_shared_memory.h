/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_
#define XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_

// Metal shared memory attempts bytes-no-copy aliasing on unified-memory
// devices and falls back to staged uploads when unsupported.

#include "xenia/gpu/shared_memory.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;
class MetalSharedMemory : public SharedMemory {
 public:
  MetalSharedMemory(MetalCommandProcessor& command_processor, Memory& memory);
  ~MetalSharedMemory() override;
  bool Initialize();
  void Shutdown();
  void ClearCache() override;

  MTL::Buffer* GetBuffer() const { return buffer_; }

  // For trace dump, simplified - just make buffer available for reading
  void UseForReading() {
    // No state transitions needed in Metal
  }
  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_upload_ranges) override;

 private:
  MetalCommandProcessor& command_processor_;
  MTL::Buffer* buffer_ = nullptr;
  bool use_zero_copy_ = false;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif
