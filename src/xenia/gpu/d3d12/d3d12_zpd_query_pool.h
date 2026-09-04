/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_D3D12_ZPD_QUERY_POOL_H_
#define XENIA_GPU_D3D12_D3D12_ZPD_QUERY_POOL_H_

#include <cstdint>
#include <vector>

#include "xenia/gpu/command_processor.h"
#include "xenia/ui/d3d12/d3d12_api.h"

namespace xe {
namespace ui {
namespace d3d12 {
class D3D12Provider;
}
}  // namespace ui

namespace gpu {
namespace d3d12 {

class DeferredCommandList;

// D3D12 occlusion query pool for ZPD reports. Queries live in ID3D12QueryHeap,
// results are copied to a persistent readback buffer via ResolveQueryData.
//
// D3D12 requires BeginQuery and EndQuery to be recorded in the same command
// list, so segments split at EndSubmission.
//
// FlushResolveBatch coalesces pending indices into contiguous ranges to cut
// down on ResolveQueryData call count.
//
// ROV queries use a separate path instead of normal D3D12 results. They write
// depth/stencil outcomes into a dedicated buffer, one slot per active query.
// QueueQueryResolve + ClearCounter are used instead of BeginQuery and EndQuery.
// Hybrid RTV queries use both, adding pre-test coverage to the slot's Total.
class D3D12ZPDQueryPool {
 public:
  D3D12ZPDQueryPool() = default;
  D3D12ZPDQueryPool(const D3D12ZPDQueryPool&) = delete;
  D3D12ZPDQueryPool& operator=(const D3D12ZPDQueryPool&) = delete;
  ~D3D12ZPDQueryPool() { Shutdown(); }

  bool EnsureInitialized(const ui::d3d12::D3D12Provider& provider,
                         uint32_t requested_capacity, bool can_recreate,
                         bool initialize_counter);
  void Shutdown();

  bool rtv_initialized() const {
    return query_heap_ && readback_buffer_ && readback_mapping_ != nullptr &&
           capacity_ != 0;
  }

  uint32_t capacity() const { return capacity_; }

  bool has_pending_resolve_batch() const {
    return !resolve_batch_indices_.empty() ||
           !counter_resolve_batch_indices_.empty();
  }

  bool counter_initialized() const {
    return counter_buffer_ && counter_readback_buffer_ &&
           counter_readback_mapping_ != nullptr && capacity_ != 0;
  }

  ID3D12Resource* counter_buffer() const { return counter_buffer_.Get(); }

  bool has_free_indices() const { return !free_indices_.empty(); }

  bool AcquireQueryIndex(uint32_t& query_index, uint32_t& query_generation);
  void ReleaseQueryIndex(uint32_t query_index, uint32_t query_generation);
  bool GenerationMatches(uint32_t query_index, uint32_t query_generation) const;

  void BeginQuery(DeferredCommandList& deferred_command_list,
                  uint32_t query_index) const;
  void EndQuery(DeferredCommandList& deferred_command_list,
                uint32_t query_index) const;
  void QueueQueryResolve(uint32_t query_index, bool counter);
  void ClearCounter(DeferredCommandList& deferred_command_list,
                    uint32_t query_index) const;

  void FlushResolveBatch(DeferredCommandList& deferred_command_list,
                         bool submission_open);

  XenosZPDReport GetQueryReadbackValue(uint32_t query_index, bool counter,
                                       bool hybrid) const;

 private:
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> query_heap_;

  // Persistently mapped. Results readable once the fence signals.
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer_;
  uint64_t* readback_mapping_ = nullptr;

  Microsoft::WRL::ComPtr<ID3D12Resource> counter_buffer_;
  Microsoft::WRL::ComPtr<ID3D12Resource> counter_readback_buffer_;
  uint32_t* counter_readback_mapping_ = nullptr;

  uint32_t capacity_ = 0;
  std::vector<uint32_t> free_indices_;

  // Bumped on each acquire so stale readbacks from a recycled slot get dropped.
  std::vector<uint32_t> index_generations_;

  std::vector<uint32_t> resolve_batch_indices_;
  std::vector<uint32_t> counter_resolve_batch_indices_;
  // Reusable scratch for coalesced contiguous ranges during flush.
  std::vector<ResolveRange> resolve_batch_ranges_;
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_D3D12_ZPD_QUERY_POOL_H_
