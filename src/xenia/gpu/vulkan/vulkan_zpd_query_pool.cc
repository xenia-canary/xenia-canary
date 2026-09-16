/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_zpd_query_pool.h"

#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace gpu {
namespace vulkan {

bool VulkanZPDQueryPool::EnsureInitialized(
    const ui::vulkan::VulkanDevice* vulkan_device, uint32_t requested_capacity,
    bool can_recreate, bool initialize_counter) {
  vulkan_device_ = vulkan_device;
  if (!vulkan_device_ || !requested_capacity) {
    return false;
  }

  if (capacity_ != 0 && capacity_ != requested_capacity) {
    if (!can_recreate) {
      return fbo_initialized() || (initialize_counter && counter_initialized());
    }
    Shutdown();
  }

  if (capacity_ == requested_capacity && fbo_initialized() &&
      (!initialize_counter || counter_initialized())) {
    return true;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  auto initialize_shared_capacity = [this, requested_capacity]() {
    if (capacity_ == requested_capacity) {
      return;
    }
    capacity_ = requested_capacity;

    free_indices_.clear();
    free_indices_.reserve(requested_capacity);
    for (uint32_t i = 0; i < requested_capacity; ++i) {
      free_indices_.push_back(requested_capacity - 1 - i);
    }
    index_generations_.assign(requested_capacity, 0);

    resolve_batch_indices_.clear();
    counter_resolve_batch_indices_.clear();
  };

  auto initialize_host_query_path = [&]() -> bool {
    if (fbo_initialized()) {
      return true;
    }

    // Need VK_EXT_host_query_reset (1.2 core) to reset slots on the CPU
    // without a paired vkCmdEndQuery. May be unavailable on older drivers or
    // devices.
    if (!vulkan_device_->properties().hostQueryReset ||
        dfn.vkResetQueryPool == nullptr) {
      return false;
    }

    VkQueryPoolCreateInfo pool_info;
    pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    pool_info.pNext = nullptr;
    pool_info.flags = 0;
    pool_info.queryType = VK_QUERY_TYPE_OCCLUSION;
    pool_info.queryCount = requested_capacity;
    pool_info.pipelineStatistics = 0;
    if (dfn.vkCreateQueryPool(device, &pool_info, nullptr, &query_pool_) !=
        VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to create the ZPD query "
          "pool, falling back to fake sample counts.");
      query_pool_ = VK_NULL_HANDLE;
      return false;
    }

    VkBufferCreateInfo readback_buffer_info;
    readback_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    readback_buffer_info.pNext = nullptr;
    readback_buffer_info.flags = 0;
    readback_buffer_info.size = sizeof(uint64_t) * requested_capacity;
    readback_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readback_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    readback_buffer_info.queueFamilyIndexCount = 0;
    readback_buffer_info.pQueueFamilyIndices = nullptr;
    if (dfn.vkCreateBuffer(device, &readback_buffer_info, nullptr,
                           &readback_buffer_) != VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to create the ZPD query "
          "readback buffer, falling back to fake sample counts.");
      dfn.vkDestroyQueryPool(device, query_pool_, nullptr);
      query_pool_ = VK_NULL_HANDLE;
      return false;
    }

    VkMemoryRequirements readback_mem_reqs;
    dfn.vkGetBufferMemoryRequirements(device, readback_buffer_,
                                      &readback_mem_reqs);

    VkMemoryAllocateInfo readback_alloc_info;
    readback_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    readback_alloc_info.pNext = nullptr;
    readback_alloc_info.allocationSize = readback_mem_reqs.size;
    readback_alloc_info.memoryTypeIndex = ui::vulkan::util::ChooseMemoryType(
        vulkan_device_->memory_types(), readback_mem_reqs.memoryTypeBits,
        ui::vulkan::util::MemoryPurpose::kReadback);
    if (readback_alloc_info.memoryTypeIndex == UINT32_MAX ||
        dfn.vkAllocateMemory(device, &readback_alloc_info, nullptr,
                             &readback_memory_) != VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to allocate ZPD query "
          "readback memory, falling back to fake sample counts.");
      dfn.vkDestroyBuffer(device, readback_buffer_, nullptr);
      readback_buffer_ = VK_NULL_HANDLE;
      dfn.vkDestroyQueryPool(device, query_pool_, nullptr);
      query_pool_ = VK_NULL_HANDLE;
      return false;
    }

    readback_is_coherent_ = (vulkan_device_->memory_types().host_coherent &
                             (1u << readback_alloc_info.memoryTypeIndex)) != 0;

    if (dfn.vkBindBufferMemory(device, readback_buffer_, readback_memory_, 0) !=
        VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to bind ZPD query readback "
          "buffer memory, falling back to fake sample counts.");
      dfn.vkFreeMemory(device, readback_memory_, nullptr);
      readback_memory_ = VK_NULL_HANDLE;
      dfn.vkDestroyBuffer(device, readback_buffer_, nullptr);
      readback_buffer_ = VK_NULL_HANDLE;
      dfn.vkDestroyQueryPool(device, query_pool_, nullptr);
      query_pool_ = VK_NULL_HANDLE;
      return false;
    }

    void* mapping = nullptr;
    if (dfn.vkMapMemory(device, readback_memory_, 0, VK_WHOLE_SIZE, 0,
                        &mapping) != VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to map ZPD query readback "
          "memory, falling back to fake sample counts.");
      dfn.vkFreeMemory(device, readback_memory_, nullptr);
      readback_memory_ = VK_NULL_HANDLE;
      dfn.vkDestroyBuffer(device, readback_buffer_, nullptr);
      readback_buffer_ = VK_NULL_HANDLE;
      dfn.vkDestroyQueryPool(device, query_pool_, nullptr);
      query_pool_ = VK_NULL_HANDLE;
      return false;
    }

    readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);

    dfn.vkResetQueryPool(device, query_pool_, 0, requested_capacity);
    initialize_shared_capacity();
    return true;
  };

  auto initialize_counter_path = [&]() -> bool {
    if (counter_initialized()) {
      return true;
    }

    VkBufferCreateInfo counter_buffer_info;
    counter_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    counter_buffer_info.pNext = nullptr;
    counter_buffer_info.flags = 0;
    counter_buffer_info.size =
        XenosZPDReport::kCounterSizeBytes * requested_capacity;
    counter_buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    counter_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    counter_buffer_info.queueFamilyIndexCount = 0;
    counter_buffer_info.pQueueFamilyIndices = nullptr;
    if (dfn.vkCreateBuffer(device, &counter_buffer_info, nullptr,
                           &counter_buffer_) != VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to create the ZPD counter "
          "buffer, falling back to fake sample counts.");
      counter_buffer_ = VK_NULL_HANDLE;
      return false;
    }

    VkMemoryRequirements counter_mem_reqs;
    dfn.vkGetBufferMemoryRequirements(device, counter_buffer_,
                                      &counter_mem_reqs);

    VkMemoryAllocateInfo counter_alloc_info;
    counter_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    counter_alloc_info.pNext = nullptr;
    counter_alloc_info.allocationSize = counter_mem_reqs.size;
    counter_alloc_info.memoryTypeIndex = ui::vulkan::util::ChooseMemoryType(
        vulkan_device_->memory_types(), counter_mem_reqs.memoryTypeBits,
        ui::vulkan::util::MemoryPurpose::kDeviceLocal);
    if (counter_alloc_info.memoryTypeIndex == UINT32_MAX ||
        dfn.vkAllocateMemory(device, &counter_alloc_info, nullptr,
                             &counter_memory_) != VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to allocate ZPD counter "
          "memory, falling back to fake sample counts.");
      dfn.vkDestroyBuffer(device, counter_buffer_, nullptr);
      counter_buffer_ = VK_NULL_HANDLE;
      return false;
    }

    if (dfn.vkBindBufferMemory(device, counter_buffer_, counter_memory_, 0) !=
        VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to bind ZPD counter "
          "buffer memory, falling back to fake sample counts.");
      dfn.vkFreeMemory(device, counter_memory_, nullptr);
      counter_memory_ = VK_NULL_HANDLE;
      dfn.vkDestroyBuffer(device, counter_buffer_, nullptr);
      counter_buffer_ = VK_NULL_HANDLE;
      return false;
    }

    VkBufferCreateInfo readback_buffer_info;
    readback_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    readback_buffer_info.pNext = nullptr;
    readback_buffer_info.flags = 0;
    readback_buffer_info.size =
        XenosZPDReport::kCounterSizeBytes * requested_capacity;
    readback_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readback_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    readback_buffer_info.queueFamilyIndexCount = 0;
    readback_buffer_info.pQueueFamilyIndices = nullptr;
    if (dfn.vkCreateBuffer(device, &readback_buffer_info, nullptr,
                           &counter_readback_buffer_) != VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to create the ZPD counter "
          "readback buffer, falling back to fake sample counts.");
      dfn.vkFreeMemory(device, counter_memory_, nullptr);
      counter_memory_ = VK_NULL_HANDLE;
      dfn.vkDestroyBuffer(device, counter_buffer_, nullptr);
      counter_buffer_ = VK_NULL_HANDLE;
      return false;
    }

    VkMemoryRequirements readback_mem_reqs;
    dfn.vkGetBufferMemoryRequirements(device, counter_readback_buffer_,
                                      &readback_mem_reqs);

    VkMemoryAllocateInfo readback_alloc_info;
    readback_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    readback_alloc_info.pNext = nullptr;
    readback_alloc_info.allocationSize = readback_mem_reqs.size;
    readback_alloc_info.memoryTypeIndex = ui::vulkan::util::ChooseMemoryType(
        vulkan_device_->memory_types(), readback_mem_reqs.memoryTypeBits,
        ui::vulkan::util::MemoryPurpose::kReadback);
    if (readback_alloc_info.memoryTypeIndex == UINT32_MAX ||
        dfn.vkAllocateMemory(device, &readback_alloc_info, nullptr,
                             &counter_readback_memory_) != VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to allocate ZPD counter "
          "readback memory, falling back to fake sample counts.");
      dfn.vkDestroyBuffer(device, counter_readback_buffer_, nullptr);
      counter_readback_buffer_ = VK_NULL_HANDLE;
      dfn.vkFreeMemory(device, counter_memory_, nullptr);
      counter_memory_ = VK_NULL_HANDLE;
      dfn.vkDestroyBuffer(device, counter_buffer_, nullptr);
      counter_buffer_ = VK_NULL_HANDLE;
      return false;
    }

    counter_readback_is_coherent_ =
        (vulkan_device_->memory_types().host_coherent &
         (1u << readback_alloc_info.memoryTypeIndex)) != 0;

    if (dfn.vkBindBufferMemory(device, counter_readback_buffer_,
                               counter_readback_memory_, 0) != VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to bind ZPD counter "
          "readback buffer memory, falling back to fake sample counts.");
      dfn.vkFreeMemory(device, counter_readback_memory_, nullptr);
      counter_readback_memory_ = VK_NULL_HANDLE;
      dfn.vkDestroyBuffer(device, counter_readback_buffer_, nullptr);
      counter_readback_buffer_ = VK_NULL_HANDLE;
      dfn.vkFreeMemory(device, counter_memory_, nullptr);
      counter_memory_ = VK_NULL_HANDLE;
      dfn.vkDestroyBuffer(device, counter_buffer_, nullptr);
      counter_buffer_ = VK_NULL_HANDLE;
      return false;
    }

    void* counter_readback_mapping = nullptr;
    if (dfn.vkMapMemory(device, counter_readback_memory_, 0, VK_WHOLE_SIZE, 0,
                        &counter_readback_mapping) != VK_SUCCESS) {
      XELOGW(
          "VulkanZPDQueryPool: Failed to map ZPD counter "
          "readback memory, falling back to fake sample counts.");
      dfn.vkFreeMemory(device, counter_readback_memory_, nullptr);
      counter_readback_memory_ = VK_NULL_HANDLE;
      dfn.vkDestroyBuffer(device, counter_readback_buffer_, nullptr);
      counter_readback_buffer_ = VK_NULL_HANDLE;
      dfn.vkFreeMemory(device, counter_memory_, nullptr);
      counter_memory_ = VK_NULL_HANDLE;
      dfn.vkDestroyBuffer(device, counter_buffer_, nullptr);
      counter_buffer_ = VK_NULL_HANDLE;
      return false;
    }

    counter_readback_mapping_ =
        reinterpret_cast<uint32_t*>(counter_readback_mapping);

    initialize_shared_capacity();

    // Every slot starts out dirty and gets cleared before using.
    counter_dirty_.assign(requested_capacity, 1);
    counter_dirty_indices_ = std::move(free_indices_);
    free_indices_.clear();
    return true;
  };

  bool any_initialized = false;
  if (initialize_host_query_path()) {
    any_initialized = true;
  }
  if (initialize_counter && initialize_counter_path()) {
    any_initialized = true;
  }
  return any_initialized;
}

void VulkanZPDQueryPool::Shutdown() {
  if (!vulkan_device_) {
    query_pool_ = VK_NULL_HANDLE;
    readback_buffer_ = VK_NULL_HANDLE;
    readback_memory_ = VK_NULL_HANDLE;
    readback_mapping_ = nullptr;
    readback_is_coherent_ = true;
    counter_buffer_ = VK_NULL_HANDLE;
    counter_memory_ = VK_NULL_HANDLE;
    counter_readback_buffer_ = VK_NULL_HANDLE;
    counter_readback_memory_ = VK_NULL_HANDLE;
    counter_readback_mapping_ = nullptr;
    counter_readback_is_coherent_ = true;
    capacity_ = 0;
    free_indices_.clear();
    index_generations_.clear();
    resolve_batch_indices_.clear();
    counter_resolve_batch_indices_.clear();
    resolve_batch_ranges_.clear();
    counter_resolve_barrier_scratch_.clear();
    counter_resolve_copy_scratch_.clear();
    return;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  free_indices_.clear();
  index_generations_.clear();
  resolve_batch_indices_.clear();
  counter_resolve_batch_indices_.clear();
  resolve_batch_ranges_.clear();
  counter_resolve_barrier_scratch_.clear();
  counter_resolve_copy_scratch_.clear();

  counter_dirty_.clear();
  counter_dirty_indices_.clear();

  capacity_ = 0;
  readback_is_coherent_ = true;
  counter_readback_is_coherent_ = true;

  if (counter_readback_mapping_ && counter_readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkUnmapMemory(device, counter_readback_memory_);
  }
  counter_readback_mapping_ = nullptr;

  if (counter_readback_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, counter_readback_buffer_, nullptr);
  }
  counter_readback_buffer_ = VK_NULL_HANDLE;

  if (counter_readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, counter_readback_memory_, nullptr);
  }
  counter_readback_memory_ = VK_NULL_HANDLE;

  if (counter_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, counter_buffer_, nullptr);
  }
  counter_buffer_ = VK_NULL_HANDLE;

  if (counter_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, counter_memory_, nullptr);
  }
  counter_memory_ = VK_NULL_HANDLE;

  if (readback_mapping_ && readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkUnmapMemory(device, readback_memory_);
  }
  readback_mapping_ = nullptr;

  if (readback_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, readback_buffer_, nullptr);
  }
  readback_buffer_ = VK_NULL_HANDLE;

  if (readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, readback_memory_, nullptr);
  }
  readback_memory_ = VK_NULL_HANDLE;

  if (query_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyQueryPool(device, query_pool_, nullptr);
  }
  query_pool_ = VK_NULL_HANDLE;
}

bool VulkanZPDQueryPool::AcquireQueryIndex(uint32_t& query_index,
                                           uint32_t& query_generation) {
  // Clean slots first, so counter queries rarely have to clear in place.
  std::vector<uint32_t>& indices =
      free_indices_.empty() ? counter_dirty_indices_ : free_indices_;
  if (indices.empty()) {
    query_index = UINT32_MAX;
    query_generation = 0;
    return false;
  }

  query_index = indices.back();
  indices.pop_back();

  assert_true(query_index < index_generations_.size());
  // Bump before returning - invalidates in-flight copies from prior occupant.
  query_generation = ++index_generations_[query_index];
  return true;
}

void VulkanZPDQueryPool::ReleaseQueryIndex(uint32_t query_index,
                                           uint32_t query_generation) {
  if (!GenerationMatches(query_index, query_generation)) {
    return;
  }

  // Bump generation so a second release with the same generation is rejected.
  ++index_generations_[query_index];

  if (query_pool_ != VK_NULL_HANDLE) {
    const ui::vulkan::VulkanDevice::Functions& dfn =
        vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();

    // Immediately reset the slot on the CPU so it's ready for the next
    // AcquireQueryIndex without requiring a paired EndQuery.
    dfn.vkResetQueryPool(device, query_pool_, query_index, 1);
  }

  if (query_index < counter_dirty_.size()) {
    counter_dirty_[query_index] = 1;
    counter_dirty_indices_.push_back(query_index);
  } else {
    free_indices_.push_back(query_index);
  }
}

bool VulkanZPDQueryPool::GenerationMatches(uint32_t query_index,
                                           uint32_t query_generation) const {
  return query_index < index_generations_.size() &&
         index_generations_[query_index] == query_generation;
}

void VulkanZPDQueryPool::BeginQuery(
    DeferredCommandBuffer& deferred_command_buffer,
    uint32_t query_index) const {
  assert_true(query_pool_ != VK_NULL_HANDLE && query_index < capacity_);
  // Precise bit is crucial. Most titles tested actually care about the sample
  // counts, not just 0 vs non-zero.
  deferred_command_buffer.CmdVkBeginQuery(query_pool_, query_index,
                                          VK_QUERY_CONTROL_PRECISE_BIT);
}

void VulkanZPDQueryPool::EndQuery(
    DeferredCommandBuffer& deferred_command_buffer,
    uint32_t query_index) const {
  assert_true(query_pool_ != VK_NULL_HANDLE && query_index < capacity_);
  deferred_command_buffer.CmdVkEndQuery(query_pool_, query_index);
}

void VulkanZPDQueryPool::QueueQueryResolve(uint32_t query_index, bool counter) {
  assert_true(query_index < capacity_);
  if (counter) {
    counter_resolve_batch_indices_.push_back(query_index);
    return;
  }
  resolve_batch_indices_.push_back(query_index);
}

void VulkanZPDQueryPool::ClearCounter(
    DeferredCommandBuffer& deferred_command_buffer, uint32_t query_index) {
  assert_true(counter_initialized() && query_index < capacity_);
  counter_dirty_[query_index] = 0;

  VkDeviceSize offset = static_cast<VkDeviceSize>(query_index) *
                        XenosZPDReport::kCounterSizeBytes;

  VkBufferMemoryBarrier drain_barrier;
  drain_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  drain_barrier.pNext = nullptr;
  drain_barrier.srcAccessMask =
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  drain_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  drain_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  drain_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  drain_barrier.buffer = counter_buffer_;
  drain_barrier.offset = offset;
  drain_barrier.size = XenosZPDReport::kCounterSizeBytes;
  deferred_command_buffer.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &drain_barrier, 0,
      nullptr);

  deferred_command_buffer.CmdVkFillBuffer(counter_buffer_, offset,
                                          XenosZPDReport::kCounterSizeBytes, 0);

  VkBufferMemoryBarrier ready_barrier;
  ready_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  ready_barrier.pNext = nullptr;
  ready_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  ready_barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  ready_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ready_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ready_barrier.buffer = counter_buffer_;
  ready_barrier.offset = offset;
  ready_barrier.size = XenosZPDReport::kCounterSizeBytes;

  deferred_command_buffer.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
      0, nullptr, 1, &ready_barrier, 0, nullptr);
}

void VulkanZPDQueryPool::FlushCounterClears(
    DeferredCommandBuffer& deferred_command_buffer) {
  if (counter_dirty_indices_.empty()) {
    return;
  }
  assert_true(counter_initialized());

  // One barrier pair around all of the fills, since a released slot can have
  // been written by shader atomics or by a previous fill.
  VkBufferMemoryBarrier drain_barrier;
  drain_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  drain_barrier.pNext = nullptr;
  drain_barrier.srcAccessMask =
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  drain_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  drain_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  drain_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  drain_barrier.buffer = counter_buffer_;
  drain_barrier.offset = 0;
  drain_barrier.size = VK_WHOLE_SIZE;
  deferred_command_buffer.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &drain_barrier, 0,
      nullptr);

  // Coalesce the dirty slots into contiguous fills, then they're clean and
  // free.
  std::ranges::sort(counter_dirty_indices_);
  uint32_t range_start = 0;
  uint32_t range_count = 0;
  auto fill_range = [&]() {
    deferred_command_buffer.CmdVkFillBuffer(
        counter_buffer_,
        static_cast<VkDeviceSize>(range_start) *
            XenosZPDReport::kCounterSizeBytes,
        static_cast<VkDeviceSize>(range_count) *
            XenosZPDReport::kCounterSizeBytes,
        0);
  };
  for (uint32_t index : counter_dirty_indices_) {
    counter_dirty_[index] = 0;
    free_indices_.push_back(index);
    if (!range_count) {
      range_start = index;
      range_count = 1;
      continue;
    }
    if (index == range_start + range_count) {
      ++range_count;
      continue;
    }
    fill_range();
    range_start = index;
    range_count = 1;
  }
  fill_range();
  counter_dirty_indices_.clear();

  VkBufferMemoryBarrier ready_barrier;
  ready_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  ready_barrier.pNext = nullptr;
  ready_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  ready_barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  ready_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ready_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ready_barrier.buffer = counter_buffer_;
  ready_barrier.offset = 0;
  ready_barrier.size = VK_WHOLE_SIZE;
  deferred_command_buffer.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
      0, nullptr, 1, &ready_barrier, 0, nullptr);
}

void VulkanZPDQueryPool::RecordResolveBatch(VkCommandBuffer command_buffer) {
  if (resolve_batch_indices_.empty() &&
      counter_resolve_batch_indices_.empty()) {
    return;
  }

  auto build_ranges = [this](std::vector<uint32_t>& indices) {
    std::ranges::sort(indices);
    resolve_batch_ranges_.clear();
    uint32_t range_start = 0;
    uint32_t range_count = 0;
    for (uint32_t index : indices) {
      if (range_count == 0) {
        range_start = index;
        range_count = 1;
        continue;
      }
      if (index == range_start + range_count) {
        ++range_count;
        continue;
      }
      resolve_batch_ranges_.push_back({range_start, range_count});
      range_start = index;
      range_count = 1;
    }
    if (range_count != 0) {
      resolve_batch_ranges_.push_back({range_start, range_count});
    }
    indices.clear();
  };

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();

  if (!resolve_batch_indices_.empty()) {
    assert_true(fbo_initialized());
    build_ranges(resolve_batch_indices_);

    VkDeviceSize barrier_offset = VK_WHOLE_SIZE;
    VkDeviceSize barrier_end = 0;
    for (const ResolveRange& range : resolve_batch_ranges_) {
      VkDeviceSize offset =
          static_cast<VkDeviceSize>(range.start) * sizeof(uint64_t);
      VkDeviceSize size =
          static_cast<VkDeviceSize>(range.count) * sizeof(uint64_t);

      // WAIT_BIT blocks until available. No separate availability check
      // needed.
      dfn.vkCmdCopyQueryPoolResults(
          command_buffer, query_pool_, range.start, range.count,
          readback_buffer_, offset, sizeof(uint64_t),
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

      barrier_offset = std::min(barrier_offset, offset);
      barrier_end = std::max(barrier_end, offset + size);
    }

    VkBufferMemoryBarrier readback_barrier;
    readback_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    readback_barrier.pNext = nullptr;
    readback_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    readback_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readback_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_barrier.buffer = readback_buffer_;
    readback_barrier.offset = barrier_offset;
    readback_barrier.size = barrier_end - barrier_offset;
    dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &readback_barrier, 0, nullptr);
  }

  if (counter_resolve_batch_indices_.empty()) {
    return;
  }

  assert_true(counter_initialized());

  build_ranges(counter_resolve_batch_indices_);

  VkDeviceSize readback_barrier_offset = VK_WHOLE_SIZE;
  VkDeviceSize readback_barrier_end = 0;
  counter_resolve_barrier_scratch_.clear();
  counter_resolve_copy_scratch_.clear();
  counter_resolve_barrier_scratch_.reserve(resolve_batch_ranges_.size());
  counter_resolve_copy_scratch_.reserve(resolve_batch_ranges_.size());
  for (const ResolveRange& range : resolve_batch_ranges_) {
    VkDeviceSize offset = static_cast<VkDeviceSize>(range.start) *
                          XenosZPDReport::kCounterSizeBytes;
    VkDeviceSize size = static_cast<VkDeviceSize>(range.count) *
                        XenosZPDReport::kCounterSizeBytes;

    VkBufferMemoryBarrier counter_barrier;
    counter_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    counter_barrier.pNext = nullptr;
    // Include TRANSFER_WRITE so that slots cleared by vkCmdFillBuffer with no
    // subsequent fragment writes are also covered. A fully occluded query sees
    // the fill as its last write, not a shader atomic.
    counter_barrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    counter_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    counter_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    counter_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    counter_barrier.buffer = counter_buffer_;
    counter_barrier.offset = offset;
    counter_barrier.size = size;
    counter_resolve_barrier_scratch_.push_back(counter_barrier);

    VkBufferCopy copy_region;
    copy_region.srcOffset = offset;
    copy_region.dstOffset = offset;
    copy_region.size = size;
    counter_resolve_copy_scratch_.push_back(copy_region);

    readback_barrier_offset = std::min(readback_barrier_offset, offset);
    readback_barrier_end = std::max(readback_barrier_end, offset + size);
  }

  dfn.vkCmdPipelineBarrier(
      command_buffer,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
      uint32_t(counter_resolve_barrier_scratch_.size()),
      counter_resolve_barrier_scratch_.data(), 0, nullptr);
  dfn.vkCmdCopyBuffer(command_buffer, counter_buffer_, counter_readback_buffer_,
                      uint32_t(counter_resolve_copy_scratch_.size()),
                      counter_resolve_copy_scratch_.data());

  VkBufferMemoryBarrier readback_barrier;
  readback_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  readback_barrier.pNext = nullptr;
  readback_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  readback_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  readback_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  readback_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  readback_barrier.buffer = counter_readback_buffer_;
  readback_barrier.offset = readback_barrier_offset;
  readback_barrier.size = readback_barrier_end - readback_barrier_offset;
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                           &readback_barrier, 0, nullptr);
}

void VulkanZPDQueryPool::InvalidateReadback() {
  if (vulkan_device_) {
    const ui::vulkan::VulkanDevice::Functions& dfn =
        vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();

    if (!readback_is_coherent_ && readback_memory_ != VK_NULL_HANDLE &&
        readback_mapping_) {
      // Invalidates the CPU-side cache so the persistent mapping reflects the
      // GPU writes made by vkCmdCopyQueryPoolResults. Not needed on
      // HOST_COHERENT.
      VkMappedMemoryRange range;
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.pNext = nullptr;
      range.memory = readback_memory_;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
    }

    if (!counter_readback_is_coherent_ &&
        counter_readback_memory_ != VK_NULL_HANDLE &&
        counter_readback_mapping_) {
      VkMappedMemoryRange range;
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.pNext = nullptr;
      range.memory = counter_readback_memory_;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
    }
  }
}

XenosZPDReport VulkanZPDQueryPool::GetQueryReadbackValue(uint32_t query_index,
                                                         bool counter,
                                                         bool hybrid) const {
  assert_true(query_index < capacity_ && readback_mapping_);
  const uint32_t* counter_slot =
      counter_readback_mapping_
          ? counter_readback_mapping_ + query_index * XenosZPDReport::kCount
          : nullptr;
  if (counter) {
    assert_not_null(counter_slot);
    return XenosZPDReport::FromCounterSlot(counter_slot);
  }
  uint64_t passed = readback_mapping_[query_index];
  if (hybrid) {
    assert_not_null(counter_slot);
    return XenosZPDReport::FromNativeQueryAndTotal(
        passed, counter_slot[XenosZPDReport::kTotal]);
  }
  return XenosZPDReport::FromNativeQuery(passed);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
