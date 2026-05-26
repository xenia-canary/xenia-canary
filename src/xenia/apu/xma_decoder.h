/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_XMA_DECODER_H_
#define XENIA_APU_XMA_DECODER_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

#include "xenia/apu/xma_context.h"
#include "xenia/apu/xma_register_file.h"
#include "xenia/base/bit_map.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {

struct XMA_CONTEXT_DATA;

class XmaDecoder {
 public:
  explicit XmaDecoder(cpu::Processor* processor);
  ~XmaDecoder();

  Memory* memory() const { return memory_; }
  cpu::Processor* processor() const { return processor_; }

  X_STATUS Setup(kernel::KernelState* kernel_state);
  void Shutdown();

  uint32_t context_array_ptr() const {
    return register_file_[XmaRegister::ContextArrayAddress];
  }

  uint32_t AllocateContext();
  void ReleaseContext(uint32_t guest_ptr);
  bool BlockOnContext(uint32_t guest_ptr, bool poll);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  bool is_paused() const { return paused_.load(std::memory_order_acquire); }
  void Pause();
  void Resume();

 protected:
  int GetContextId(uint32_t guest_ptr);

 private:
  void PoolWorkerMain(size_t group_index);

  static uint32_t MMIOReadRegisterThunk(void* ppc_context, XmaDecoder* as,
                                        uint32_t addr) {
    return as->ReadRegister(addr);
  }
  static void MMIOWriteRegisterThunk(void* ppc_context, XmaDecoder* as,
                                     uint32_t addr, uint32_t value) {
    as->WriteRegister(addr, value);
  }

 protected:
  Memory* memory_ = nullptr;
  cpu::Processor* processor_ = nullptr;

  std::atomic<bool> worker_running_ = {false};

  // Each worker is asigned to Context0Kick..Context9Kick.
  // Each worker owns up to 2 groups (64 contexts) and is woken
  // directly by the Kick handler that targets one of its groups.
  static constexpr size_t kKickGroupCount = 10;
  static constexpr size_t kKickGroupsPerWorker = 2;
  static constexpr size_t kWorkerPoolSize =
      kKickGroupCount / kKickGroupsPerWorker;
  struct PoolWorker {
    std::atomic<uint32_t> wake_signal{0};
    std::atomic<uint64_t> pending_mask{0};
  };
  std::array<PoolWorker, kWorkerPoolSize> worker_pool_;
  std::array<kernel::object_ref<kernel::XHostThread>, kWorkerPoolSize>
      pool_threads_;

  // Pause coordination across all pool workers. paused_ is the public-facing
  // flag; paused_count_ tallies how many workers are currently parked.
  std::atomic<bool> paused_ = false;
  std::mutex pause_mutex_;
  std::condition_variable pause_cv_;
  int paused_count_ = 0;

  XmaRegisterFile register_file_;

  static const uint32_t kContextCount = 320;
  static_assert(kContextCount == kWorkerPoolSize * kKickGroupsPerWorker * 32,
                "Pool partitioning must cover every context exactly once.");
  XmaContext* contexts_[kContextCount];
  BitMap context_bitmap_;

  uint32_t context_data_first_ptr_ = 0;
  uint32_t context_data_last_ptr_ = 0;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_XMA_DECODER_H_
