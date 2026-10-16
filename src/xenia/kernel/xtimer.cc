/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() { DisarmDpcTimer(); }

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

void XTimer::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  native_header_ = header;
  // TimerNotificationObject = 8, TimerSynchronizationObject = 9.
  Initialize(header->type == 9 ? 1 : 0);
}

static xe::chrono::WinSystemClock::time_point GuestDueTime(int64_t due_time) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;
  if (due_time < 0) {
    auto after = xe::chrono::hundrednanoseconds(-due_time);
    return date::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  }
  return date::clock_cast<WinSystemClock>(
      XSystemClock::from_file_time(due_time));
}

void XTimer::DisarmDpcTimer() {
  if (auto item = dpc_timer_.lock()) {
    item->Disarm();
  }
  dpc_timer_.reset();
}

bool XTimer::IsDpcTimerSet() const {
  return inserted_ &&
         (periodic_ || xe::chrono::WinSystemClock::now() < due_tp_);
}

bool XTimer::SetTimerDpc(int64_t due_time, uint32_t period_ms,
                         uint32_t dpc_ptr) {
  std::lock_guard<std::mutex> lock(timer_lock_);
  bool was_set = IsDpcTimerSet();
  DisarmDpcTimer();

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  auto due_tp = GuestDueTime(due_time);
  // The waitable timer gives waiters the timer's signal semantics; it has no
  // callback, so it queues nothing on the setting thread.
  bool result = period_ms ? timer_->SetRepeatingAt(
                                due_tp, std::chrono::milliseconds(period_ms))
                          : timer_->SetOnceAt(due_tp);

  auto steady_due =
      xe::threading::TimerQueueWaitItem::clock::now() +
      std::chrono::duration_cast<
          xe::threading::TimerQueueWaitItem::clock::duration>(
          due_tp - xe::chrono::WinSystemClock::now());
  bool periodic = period_ms != 0;
  auto callback = [ks = kernel_state(), dpc_ptr](void*) {
    OnDpcTimerExpired(ks, dpc_ptr);
  };
  dpc_timer_ = periodic ? xe::threading::QueueTimerRecurring(
                              std::move(callback), nullptr, steady_due,
                              std::chrono::milliseconds(period_ms))
                        : xe::threading::QueueTimerOnce(std::move(callback),
                                                        nullptr, steady_due);

  inserted_ = result;
  periodic_ = periodic;
  due_tp_ = due_tp;
  if (native_header_) {
    native_header_->inserted = result ? 1 : 0;
  }
  return was_set;
}

void XTimer::OnDpcTimerExpired(KernelState* kernel_state, uint32_t dpc_ptr) {
  // No timer state here: Disarm, called under timer_lock_, waits for a
  // running callback, so taking the lock would deadlock. IsDpcTimerSet tells
  // from the due time whether a one-shot timer has expired.
  if (dpc_ptr) {
    // As the kernel's timer expiry does: the system time, low then high.
    uint64_t time = xe::Clock::QueryGuestSystemTime();
    kernel_state->QueueDpc(dpc_ptr, static_cast<uint32_t>(time),
                           static_cast<uint32_t>(time >> 32));
  }
}

bool XTimer::CancelTimerDpc() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  bool was_set = IsDpcTimerSet();
  DisarmDpcTimer();
  timer_->Cancel();
  inserted_ = false;
  if (native_header_) {
    native_header_->inserted = 0;
  }
  return was_set;
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;
  // Caller is checking for STATUS_TIMER_RESUME_IGNORED.
  if (resume) {
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  std::lock_guard<std::mutex> lock(timer_lock_);

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = xe::chrono::hundrednanoseconds(-due_time);
    due_tp = date::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = date::clock_cast<WinSystemClock>(
        XSystemClock::from_file_time(due_time));
  }

  // Stash routine for callback.
  callback_thread_ = XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  // Capture values by value to avoid racing with a future SetTimer() call.
  std::function<void()> callback = nullptr;
  if (callback_routine_) {
    auto cb_thread = callback_thread_;
    auto cb_routine = callback_routine_;
    auto cb_routine_arg = callback_routine_arg_;
    callback = [cb_thread, cb_routine, cb_routine_arg]() {
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = xe::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      XELOGI(
          "XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
          cb_routine, cb_routine_arg, time_low, time_high);
      cb_thread->EnqueueApc(cb_routine, cb_routine_arg, time_low, time_high);
    };
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result = timer_->SetRepeatingAt(
        due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace kernel
}  // namespace xe
