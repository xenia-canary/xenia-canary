/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_ZPD_QUERY_POOL_H_
#define XENIA_GPU_VULKAN_VULKAN_ZPD_QUERY_POOL_H_

#include <cstdint>
#include <vector>

#include "xenia/gpu/command_processor.h"
#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace ui {
namespace vulkan {
class VulkanDevice;
}
}  // namespace ui

namespace gpu {
namespace vulkan {

class DeferredCommandBuffer;

// Vulkan occlusion query pool for ZPD reports. Queries live in VkQueryPool,
// results are copied to a persistent buffer via vkCmdCopyQueryPoolResults.
// vkCmdBeginQuery is only valid inside a render pass, queries get deferred
// when no pass is open and segments split at pass boundaries.
//
// Requires VK_EXT_host_query_reset (1.2 core) so slots can be reset on the
// CPU at release time, no paired vkCmdEndQuery needed, and also allows
// DiscardZPDQuery work outside a pass.
//
// VK_QUERY_RESULT_WAIT_BIT in the copy removes the need for a separate
// availability check. Transfer barrier before InvalidateReadback covers non-
// coherent memory.
class VulkanZPDQueryPool {
 public:
  VulkanZPDQueryPool() = default;
  VulkanZPDQueryPool(const VulkanZPDQueryPool&) = delete;
  VulkanZPDQueryPool& operator=(const VulkanZPDQueryPool&) = delete;
  ~VulkanZPDQueryPool() { Shutdown(); }

  bool EnsureInitialized(const ui::vulkan::VulkanDevice* vulkan_device,
                         uint32_t requested_capacity, bool can_recreate,
                         bool initialize_fsi_counter = false);
  void Shutdown();

  bool fbo_initialized() const {
    return query_pool_ != VK_NULL_HANDLE &&
           readback_buffer_ != VK_NULL_HANDLE && readback_mapping_ != nullptr &&
           capacity_ != 0;
  }

  bool fsi_initialized() const {
    return fsi_counter_buffer_ != VK_NULL_HANDLE &&
           fsi_counter_memory_ != VK_NULL_HANDLE &&
           fsi_counter_readback_buffer_ != VK_NULL_HANDLE &&
           fsi_counter_readback_mapping_ != nullptr && capacity_ != 0;
  }

  uint32_t capacity() const { return capacity_; }

  bool has_pending_resolve_batch() const {
    return !resolve_batch_indices_.empty() ||
           !fsi_counter_resolve_batch_indices_.empty();
  }

  VkBuffer fsi_counter_buffer() const { return fsi_counter_buffer_; }

  bool has_free_indices() const { return !free_indices_.empty(); }

  bool AcquireQueryIndex(uint32_t& query_index, uint32_t& query_generation);
  void ReleaseQueryIndex(uint32_t query_index, uint32_t query_generation);
  bool GenerationMatches(uint32_t query_index, uint32_t query_generation) const;

  void BeginQuery(DeferredCommandBuffer& deferred_command_buffer,
                  uint32_t query_index) const;
  void EndQuery(DeferredCommandBuffer& deferred_command_buffer,
                uint32_t query_index) const;
  void QueueQueryResolve(uint32_t query_index, bool uses_fsi_counter = false);
  void ClearFSICounter(DeferredCommandBuffer& deferred_command_buffer,
                       uint32_t query_index) const;
  void RecordResolveBatch(VkCommandBuffer command_buffer);

  void InvalidateReadback();

  uint64_t GetQueryReadbackValue(uint32_t query_index,
                                 bool uses_fsi_counter = false) const;

 private:
  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;

  VkQueryPool query_pool_ = VK_NULL_HANDLE;

  VkBuffer readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
  uint64_t* readback_mapping_ = nullptr;
  // If not HOST_COHERENT, call InvalidateReadback before reading.
  bool readback_is_coherent_ = true;

  VkBuffer fsi_counter_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory fsi_counter_memory_ = VK_NULL_HANDLE;

  VkBuffer fsi_counter_readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory fsi_counter_readback_memory_ = VK_NULL_HANDLE;
  uint32_t* fsi_counter_readback_mapping_ = nullptr;
  bool fsi_counter_readback_is_coherent_ = true;

  uint32_t capacity_ = 0;
  std::vector<uint32_t> free_indices_;

  // Bumped on each acquire so stale copies from recycled slots get dropped.
  std::vector<uint32_t> index_generations_;

  std::vector<uint8_t> resolve_batch_pending_;
  // Active indices with resolve_batch_pending_[i] == 1, so flush iterates
  // only the active entries instead of scanning the full capacity.
  std::vector<uint32_t> resolve_batch_indices_;
  std::vector<uint8_t> fsi_counter_resolve_batch_pending_;
  std::vector<uint32_t> fsi_counter_resolve_batch_indices_;
  // Reusable scratch for coalesced contiguous ranges during flush.
  std::vector<ResolveRange> resolve_batch_ranges_;
  // Reusable scratch for FSI counter resolve barriers and copy regions.
  std::vector<VkBufferMemoryBarrier> fsi_resolve_barrier_scratch_;
  std::vector<VkBufferCopy> fsi_resolve_copy_scratch_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_ZPD_QUERY_POOL_H_
