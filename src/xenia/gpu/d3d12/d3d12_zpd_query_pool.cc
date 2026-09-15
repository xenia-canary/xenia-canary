/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/d3d12_zpd_query_pool.h"

#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/gpu/d3d12/deferred_command_list.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"

namespace xe {
namespace gpu {
namespace d3d12 {

bool D3D12ZPDQueryPool::EnsureInitialized(
    const ui::d3d12::D3D12Provider& provider, uint32_t requested_capacity,
    bool can_recreate, bool initialize_counter) {
  if (rtv_initialized() && (!initialize_counter || counter_initialized()) &&
      (capacity_ == requested_capacity || !can_recreate)) {
    return true;
  }

  if (rtv_initialized() && capacity_ != requested_capacity) {
    if (!can_recreate) {
      requested_capacity = capacity_;
    } else {
      // Can't recreate while resolves are in-flight, that would destroy the
      // backing resources under pending resolve or copy work.
      assert_true(!has_pending_resolve_batch());
      Shutdown();
    }
  }

  ID3D12Device* device = provider.GetDevice();

  if (!rtv_initialized()) {
    D3D12_QUERY_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    heap_desc.Count = requested_capacity;
    heap_desc.NodeMask = 0;

    if (FAILED(
            device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&query_heap_)))) {
      XELOGW(
          "D3D12ZPDQueryPool: Failed to create the ZPD query "
          "heap, falling back to fake sample counts.");
      return false;
    }

    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(
        buffer_desc, sizeof(uint64_t) * requested_capacity,
        D3D12_RESOURCE_FLAG_NONE);

    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback,
            provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback_buffer_)))) {
      XELOGW(
          "D3D12ZPDQueryPool: Failed to allocate the ZPD query "
          "readback buffer, falling back to fake sample counts.");
      Shutdown();
      return false;
    }

    D3D12_RANGE read_range = {};
    read_range.Begin = 0;
    read_range.End = sizeof(uint64_t) * requested_capacity;

    void* mapping = nullptr;
    if (FAILED(readback_buffer_->Map(0, &read_range, &mapping))) {
      XELOGW(
          "D3D12ZPDQueryPool: Failed to map the ZPD query "
          "readback buffer, falling back to fake sample counts.");
      Shutdown();
      return false;
    }

    readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);
    capacity_ = requested_capacity;

    resolve_batch_indices_.clear();
    counter_resolve_batch_indices_.clear();
    resolve_batch_ranges_.clear();

    free_indices_.clear();
    free_indices_.reserve(requested_capacity);
    for (uint32_t i = requested_capacity; i > 0; --i) {
      free_indices_.push_back(i - 1);
    }
    index_generations_.assign(requested_capacity, 0);
  }

  if (!initialize_counter || counter_initialized()) {
    return true;
  }

  if (counter_readback_mapping_ && counter_readback_buffer_) {
    D3D12_RANGE written_range = {0, 0};
    counter_readback_buffer_->Unmap(0, &written_range);
  }
  counter_readback_mapping_ = nullptr;
  counter_readback_buffer_.Reset();
  counter_buffer_.Reset();
  counter_resolve_batch_indices_.clear();

  D3D12_RESOURCE_DESC counter_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      counter_buffer_desc,
      XenosZPDReport::kCounterSizeBytes * requested_capacity,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault,
          provider.GetHeapFlagCreateNotZeroed(), &counter_buffer_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
          IID_PPV_ARGS(&counter_buffer_)))) {
    XELOGW(
        "D3D12ZPDQueryPool: Failed to create the ZPD counter "
        "buffer, falling back to fake sample counts.");
    return false;
  }

  D3D12_RESOURCE_DESC readback_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      readback_buffer_desc,
      XenosZPDReport::kCounterSizeBytes * requested_capacity,
      D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          provider.GetHeapFlagCreateNotZeroed(), &readback_buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&counter_readback_buffer_)))) {
    XELOGW(
        "D3D12ZPDQueryPool: Failed to create the ZPD counter readback "
        "buffer, falling back to fake sample counts.");
    counter_buffer_.Reset();
    return false;
  }

  D3D12_RANGE read_range = {};
  read_range.Begin = 0;
  read_range.End = XenosZPDReport::kCounterSizeBytes * requested_capacity;

  void* mapping = nullptr;
  if (FAILED(counter_readback_buffer_->Map(0, &read_range, &mapping))) {
    XELOGW(
        "D3D12ZPDQueryPool: Failed to map the ZPD counter readback "
        "buffer, falling back to fake sample counts.");
    counter_readback_buffer_.Reset();
    counter_buffer_.Reset();
    return false;
  }

  counter_readback_mapping_ = reinterpret_cast<uint32_t*>(mapping);
  return true;
}

void D3D12ZPDQueryPool::Shutdown() {
  resolve_batch_indices_.clear();
  resolve_batch_ranges_.clear();
  counter_resolve_batch_indices_.clear();
  free_indices_.clear();
  index_generations_.clear();

  capacity_ = 0;

  if (readback_mapping_ && readback_buffer_) {
    // CPU never writes to this READBACK buffer - empty written range.
    D3D12_RANGE written_range = {0, 0};
    readback_buffer_->Unmap(0, &written_range);
  }

  readback_mapping_ = nullptr;
  readback_buffer_.Reset();
  query_heap_.Reset();

  if (counter_readback_mapping_ && counter_readback_buffer_) {
    D3D12_RANGE written_range = {0, 0};
    counter_readback_buffer_->Unmap(0, &written_range);
  }

  counter_readback_mapping_ = nullptr;
  counter_readback_buffer_.Reset();
  counter_buffer_.Reset();
}

bool D3D12ZPDQueryPool::AcquireQueryIndex(uint32_t& query_index,
                                          uint32_t& query_generation) {
  if (free_indices_.empty()) {
    query_index = UINT32_MAX;
    query_generation = 0;
    return false;
  }

  query_index = free_indices_.back();
  free_indices_.pop_back();

  assert_true(query_index < index_generations_.size());
  // Bump the generation. Any in-flight readbacks for the slot's previous
  // occupants are ignored.
  query_generation = ++index_generations_[query_index];
  return true;
}

void D3D12ZPDQueryPool::ReleaseQueryIndex(uint32_t query_index,
                                          uint32_t query_generation) {
  if (!GenerationMatches(query_index, query_generation)) {
    return;
  }

  // Bump generation so a second release with the same generation is rejected.
  ++index_generations_[query_index];
  free_indices_.push_back(query_index);
}

bool D3D12ZPDQueryPool::GenerationMatches(uint32_t query_index,
                                          uint32_t query_generation) const {
  return query_index < index_generations_.size() &&
         index_generations_[query_index] == query_generation;
}

void D3D12ZPDQueryPool::BeginQuery(DeferredCommandList& deferred_command_list,
                                   uint32_t query_index) const {
  assert_true(query_heap_ && query_index < capacity_);
  deferred_command_list.D3DBeginQuery(query_heap_.Get(),
                                      D3D12_QUERY_TYPE_OCCLUSION, query_index);
}

void D3D12ZPDQueryPool::EndQuery(DeferredCommandList& deferred_command_list,
                                 uint32_t query_index) const {
  assert_true(query_heap_ && query_index < capacity_);
  deferred_command_list.D3DEndQuery(query_heap_.Get(),
                                    D3D12_QUERY_TYPE_OCCLUSION, query_index);
}

void D3D12ZPDQueryPool::QueueQueryResolve(uint32_t query_index, bool counter) {
  assert_true(query_index < capacity_);
  if (counter) {
    counter_resolve_batch_indices_.push_back(query_index);
    return;
  }
  resolve_batch_indices_.push_back(query_index);
}

void D3D12ZPDQueryPool::ClearCounter(DeferredCommandList& deferred_command_list,
                                     uint32_t query_index) const {
  assert_true(counter_initialized() && query_index < capacity_);
  // This buffer stays in UNORDERED_ACCESS for the duration of its use. Before
  // reusing a slot, order this write after any atomic adds issued by the
  // previous query that owned the same index.
  D3D12_RESOURCE_BARRIER uav_barrier = {};
  uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav_barrier.UAV.pResource = counter_buffer_.Get();
  deferred_command_list.D3DResourceBarrier(1, &uav_barrier);

  // WriteBufferImmediate resets just this slot without a state transition.
  // Only the selected slot needs to be reset, so use WriteBufferImmediate
  // instead of transitioning the whole buffer through a copy path.
  D3D12_GPU_VIRTUAL_ADDRESS counter_address =
      counter_buffer_->GetGPUVirtualAddress() +
      static_cast<uint64_t>(query_index) * XenosZPDReport::kCounterSizeBytes;
  for (uint32_t i = 0; i < XenosZPDReport::kCount; ++i) {
    deferred_command_list.D3DWriteBufferImmediate(
        counter_address + i * sizeof(uint32_t), 0u);
  }

  // Order the zero write before any upcoming PS atomic adds so the next query
  // using this slot sees the cleared counter value.
  deferred_command_list.D3DResourceBarrier(1, &uav_barrier);
}

void D3D12ZPDQueryPool::FlushResolveBatch(
    DeferredCommandList& deferred_command_list, bool submission_open) {
  if (!submission_open || (resolve_batch_indices_.empty() &&
                           counter_resolve_batch_indices_.empty())) {
    return;
  }

  // Sorts indices, coalesces contiguous runs into resolve_batch_ranges_, and
  // clears the index list.
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

  if (!resolve_batch_indices_.empty()) {
    assert_true(rtv_initialized());
    build_ranges(resolve_batch_indices_);
    for (const ResolveRange& range : resolve_batch_ranges_) {
      deferred_command_list.D3DResolveQueryData(
          query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, range.start,
          range.count, readback_buffer_.Get(), range.start * sizeof(uint64_t));
    }
  }

  if (counter_resolve_batch_indices_.empty()) {
    return;
  }

  assert_true(counter_initialized());

  // The shader path writes counters through UAV atomics, so resolve on
  // this path means copying the finished slots out of the UAV buffer.
  // The whole buffer is transitioned for the copy and then returned to
  // UNORDERED_ACCESS since D3D12 state is tracked per resource, not per range.
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = counter_buffer_.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  deferred_command_list.D3DResourceBarrier(1, &barrier);

  build_ranges(counter_resolve_batch_indices_);
  for (const ResolveRange& range : resolve_batch_ranges_) {
    uint64_t offset =
        static_cast<uint64_t>(range.start) * XenosZPDReport::kCounterSizeBytes;
    uint64_t size =
        static_cast<uint64_t>(range.count) * XenosZPDReport::kCounterSizeBytes;
    deferred_command_list.D3DCopyBufferRegion(counter_readback_buffer_.Get(),
                                              offset, counter_buffer_.Get(),
                                              offset, size);
  }

  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  deferred_command_list.D3DResourceBarrier(1, &barrier);
}

XenosZPDReport D3D12ZPDQueryPool::GetQueryReadbackValue(uint32_t query_index,
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

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
