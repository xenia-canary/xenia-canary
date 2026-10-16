/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XTIMER_H_
#define XENIA_KERNEL_XTIMER_H_

#include <memory>
#include <mutex>

#include "xenia/base/chrono.h"
#include "xenia/base/threading.h"
#include "xenia/base/threading_timer_queue.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class XThread;

class XTimer : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Timer;

  explicit XTimer(KernelState* kernel_state);
  ~XTimer() override;

  void Initialize(uint32_t timer_type);
  // A KTIMER in guest memory (header type 8 or 9), set with KeSetTimerEx.
  void InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header);

  X_STATUS SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                    uint32_t routine_arg, bool resume);
  X_STATUS Cancel();

  // KeSetTimerEx: (re)arm the timer, queueing dpc_ptr (if any) each time it
  // expires. Returns whether the timer was already set.
  bool SetTimerDpc(int64_t due_time, uint32_t period_ms, uint32_t dpc_ptr);
  // KeCancelTimer: returns whether the timer was set.
  bool CancelTimerDpc();

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override { return timer_.get(); }

 private:
  std::unique_ptr<xe::threading::Timer> timer_;
  std::mutex timer_lock_;

  // Expiry of a KeSetTimerEx timer: queues its DPC. Runs on the timer queue's
  // thread, not a guest thread, so the DPC does not wait for the setting
  // thread to make an alertable wait.
  static void OnDpcTimerExpired(KernelState* kernel_state, uint32_t dpc_ptr);
  void DisarmDpcTimer();
  // Whether a KeSetTimerEx timer is still set: a one-shot timer stops being
  // set at its due time. Tracked from the time rather than by the expiry
  // callback, which can run after the waitable timer has released a waiter.
  bool IsDpcTimerSet() const;

  X_DISPATCH_HEADER* native_header_ = nullptr;
  std::weak_ptr<xe::threading::TimerQueueWaitItem> dpc_timer_;
  // Guarded by timer_lock_.
  bool inserted_ = false;
  bool periodic_ = false;
  xe::chrono::WinSystemClock::time_point due_tp_;

  XThread* callback_thread_ = nullptr;
  uint32_t callback_routine_ = 0;
  uint32_t callback_routine_arg_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTIMER_H_
