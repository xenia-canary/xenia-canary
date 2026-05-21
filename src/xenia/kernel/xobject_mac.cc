/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xobject_mac.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread_mac.h"
#include "xenia/xbox.h"

DEFINE_uint32(main_thread_wait_fallback_ms, 0,
              "Force main thread waits to time out after N ms when otherwise infinite or longer than N (0 disables).",
              "Debug");
DEFINE_bool(main_thread_wait_force_success, false,
            "When a main thread wait times out due to fallback, return success instead of timeout (dangerous).",
            "Debug");
DECLARE_uint32(gpu_bootstrap_wait_object_watch);

namespace xe {
namespace kernel {

namespace {

uint32_t GetCurrentThreadPcForDebug() {
  auto* thread = XThread::GetCurrentThread();
  if (!thread || !thread->thread_state() || !thread->thread_state()->context()) {
    return 0;
  }
  return thread->thread_state()->context()->current_pc;
}

uint32_t GetCurrentThreadLrForDebug() {
  auto* thread = XThread::GetCurrentThread();
  if (!thread || !thread->thread_state() || !thread->thread_state()->context()) {
    return 0;
  }
  return static_cast<uint32_t>(thread->thread_state()->context()->lr);
}

void QueryEventStateForWatchLog(XObject* object, bool* out_valid,
                                uint32_t* out_event_type,
                                uint32_t* out_event_state) {
  bool valid = false;
  uint32_t type = 0;
  uint32_t state = 0;
  if (object && object->type() == XObject::Type::Event) {
    auto* event = static_cast<XEvent*>(object);
    event->Query(&type, &state);
    valid = true;
  }
  if (out_valid) {
    *out_valid = valid;
  }
  if (out_event_type) {
    *out_event_type = type;
  }
  if (out_event_state) {
    *out_event_state = state;
  }
}

}  // namespace

// Implementation of XObjectErrorHandler static members
std::mutex XObjectErrorHandler::error_mutex_;
std::vector<XObjectErrorHandler::ErrorReport> XObjectErrorHandler::error_history_;
std::function<void(const XObjectErrorHandler::ErrorReport&)> XObjectErrorHandler::error_callback_;

// Implementation of MemoryCorruptionDetector static members
std::mutex MemoryCorruptionDetector::guard_mutex_;
std::unordered_map<uint32_t, MemoryCorruptionDetector::MemoryGuard> MemoryCorruptionDetector::memory_guards_;

XObject::XObject(Type type)
    : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type, bool host_object)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false),
      host_object_(host_object) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  assert_true(handles_.empty());
  assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

Emulator* XObject::emulator() const { return kernel_state_->emulator_; }
KernelState* XObject::kernel_state() const { return kernel_state_; }
Memory* XObject::memory() const { return kernel_state_->memory(); }

XObject::Type XObject::type() const { return type_; }

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) ==
         X_STATUS_SUCCESS;
}

void XObject::Retain() { ++pointer_ref_count_; }

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
    case Type::Device:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(), xe::load_and_swap<uint32_t>(
                    memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // NetDll_WSAWaitForMultipleEvents provides timeout in form of MS.
    return (uint32_t)timeout_ticks;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode,
                       uint32_t alertable, uint64_t* opt_timeout) {
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  // Debug: log long waits from the main thread to diagnose stalls.
  XThread* current_thread = XThread::GetCurrentThread();
  bool is_main_thread = current_thread && current_thread->main_thread();
  uint32_t thread_handle = current_thread ? current_thread->handle() : 0;
  uint32_t thread_id = current_thread ? current_thread->thread_id() : 0;
  uint32_t watch_object_ptr = cvars::gpu_bootstrap_wait_object_watch;
  bool watch_match =
      watch_object_ptr != 0 && guest_object() == watch_object_ptr;
  if (watch_match) {
    bool event_state_valid = false;
    uint32_t event_type = 0;
    uint32_t event_state = 0;
    QueryEventStateForWatchLog(this, &event_state_valid, &event_type,
                               &event_state);
    XELOGW(
        "RING BUFFER: XObject::Wait WATCH_HIT pre obj_ptr={:08X} "
        "watch_obj={:08X} type={} handle={:08X} thread_id={} "
        "thread_handle={:08X} wait_reason={} processor_mode={} alertable={} "
        "event_state_valid={} event_type={:08X} event_state={:08X} "
        "timeout_ptr={} pc={:08X} lr={:08X}",
        guest_object(), watch_object_ptr, static_cast<uint32_t>(type_),
        handle(), thread_id, thread_handle, wait_reason, processor_mode,
        alertable, event_state_valid ? "YES" : "NO", event_type, event_state,
        opt_timeout ? "NON_NULL" : "NULL", GetCurrentThreadPcForDebug(),
        GetCurrentThreadLrForDebug());
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();
  bool forced_timeout = false;
  if (is_main_thread && cvars::main_thread_wait_fallback_ms > 0) {
    auto forced_ms =
        std::chrono::milliseconds(cvars::main_thread_wait_fallback_ms);
    if (timeout_ms == std::chrono::milliseconds::max() ||
        timeout_ms > forced_ms) {
      XELOGW(
          "DEBUG: MainThread wait fallback: overriding timeout {} -> {} ms",
          timeout_ms == std::chrono::milliseconds::max()
              ? -1
              : static_cast<int64_t>(timeout_ms.count()),
          static_cast<int64_t>(forced_ms.count()));
      timeout_ms = forced_ms;
      forced_timeout = true;
    }
  }

  if (is_main_thread) {
    static uint64_t last_main_wait_log_ms = 0;
    uint64_t now_ms = Clock::QueryHostUptimeMillis();
    if (now_ms - last_main_wait_log_ms > 1000) {
      XELOGI(
          "DEBUG: MainThread Wait: obj_type={} handle={:08X} thread_id={} wait_reason={} alertable={} timeout_ms={}",
          static_cast<uint32_t>(type_), handle(), thread_id, wait_reason,
          alertable ? 1 : 0,
          timeout_ms == std::chrono::milliseconds::max()
              ? -1
              : static_cast<int64_t>(timeout_ms.count()));
      last_main_wait_log_ms = now_ms;
    }
  }

  auto result =
      xe::threading::Wait(wait_handle, alertable ? true : false, timeout_ms);
  switch (result) {
    case xe::threading::WaitResult::kSuccess:
      if (watch_match) {
        bool event_state_valid = false;
        uint32_t event_type = 0;
        uint32_t event_state = 0;
        QueryEventStateForWatchLog(this, &event_state_valid, &event_type,
                                   &event_state);
        XELOGW(
            "RING BUFFER: XObject::Wait WATCH_HIT result=SUCCESS "
            "obj_ptr={:08X} event_state_valid={} event_type={:08X} "
            "event_state={:08X}",
            guest_object(), event_state_valid ? "YES" : "NO", event_type,
            event_state);
      }
      WaitCallback();
      return X_STATUS_SUCCESS;
    case xe::threading::WaitResult::kUserCallback:
      if (watch_match) {
        XELOGW(
            "RING BUFFER: XObject::Wait WATCH_HIT result=USER_CALLBACK "
            "obj_ptr={:08X}",
            guest_object());
      }
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case xe::threading::WaitResult::kTimeout:
      if (watch_match) {
        bool event_state_valid = false;
        uint32_t event_type = 0;
        uint32_t event_state = 0;
        QueryEventStateForWatchLog(this, &event_state_valid, &event_type,
                                   &event_state);
        XELOGW(
            "RING BUFFER: XObject::Wait WATCH_HIT result=TIMEOUT "
            "obj_ptr={:08X} event_state_valid={} event_type={:08X} "
            "event_state={:08X}",
            guest_object(), event_state_valid ? "YES" : "NO", event_type,
            event_state);
      }
      if (is_main_thread) {
        XELOGW(
            "DEBUG: MainThread Wait timeout: obj_type={} handle={:08X} thread_id={} wait_reason={} alertable={} forced_timeout={}",
            static_cast<uint32_t>(type_), handle(), thread_id, wait_reason,
            alertable ? 1 : 0, forced_timeout ? "YES" : "NO");
      }
      xe::threading::MaybeYield();
      if (forced_timeout && cvars::main_thread_wait_force_success) {
        XELOGW("DEBUG: MainThread wait fallback: returning SUCCESS");
        return X_STATUS_SUCCESS;
      }
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      if (watch_match) {
        XELOGW(
            "RING BUFFER: XObject::Wait WATCH_HIT result=FAILED_OR_ABANDONED "
            "obj_ptr={:08X}",
            guest_object());
      }
      if (is_main_thread) {
        XELOGE(
            "DEBUG: MainThread Wait failed/abandoned: obj_type={} handle={:08X} thread_id={} wait_reason={} alertable={}",
            static_cast<uint32_t>(type_), handle(), thread_id, wait_reason,
            alertable ? 1 : 0);
      }
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object,
                                uint32_t wait_reason, uint32_t processor_mode,
                                uint32_t alertable, uint64_t* opt_timeout) {
  XThread* current_thread = XThread::GetCurrentThread();
  bool is_main_thread = current_thread && current_thread->main_thread();
  uint32_t watch_object_ptr = cvars::gpu_bootstrap_wait_object_watch;
  bool watch_match_signal =
      watch_object_ptr != 0 && signal_object &&
      signal_object->guest_object() == watch_object_ptr;
  bool watch_match_wait = watch_object_ptr != 0 && wait_object &&
                          wait_object->guest_object() == watch_object_ptr;
  if (watch_match_signal || watch_match_wait) {
    bool signal_event_state_valid = false;
    uint32_t signal_event_type = 0;
    uint32_t signal_event_state = 0;
    QueryEventStateForWatchLog(signal_object, &signal_event_state_valid,
                               &signal_event_type, &signal_event_state);
    bool wait_event_state_valid = false;
    uint32_t wait_event_type = 0;
    uint32_t wait_event_state = 0;
    QueryEventStateForWatchLog(wait_object, &wait_event_state_valid,
                               &wait_event_type, &wait_event_state);
    XELOGW(
        "RING BUFFER: XObject::SignalAndWait WATCH_HIT pre watch_obj={:08X} "
        "signal_obj={:08X} signal_type={} signal_handle={:08X} "
        "signal_event_state_valid={} signal_event_type={:08X} "
        "signal_event_state={:08X} wait_obj={:08X} wait_type={} "
        "wait_handle={:08X} wait_event_state_valid={} wait_event_type={:08X} "
        "wait_event_state={:08X} wait_reason={} processor_mode={} "
        "alertable={} timeout_ptr={} thread_id={} pc={:08X} lr={:08X}",
        watch_object_ptr, signal_object ? signal_object->guest_object() : 0,
        signal_object ? static_cast<uint32_t>(signal_object->type()) : 0,
        signal_object ? signal_object->handle() : 0,
        signal_event_state_valid ? "YES" : "NO", signal_event_type,
        signal_event_state, wait_object ? wait_object->guest_object() : 0,
        wait_object ? static_cast<uint32_t>(wait_object->type()) : 0,
        wait_object ? wait_object->handle() : 0,
        wait_event_state_valid ? "YES" : "NO", wait_event_type,
        wait_event_state, wait_reason, processor_mode, alertable,
        opt_timeout ? "NON_NULL" : "NULL",
        current_thread ? current_thread->thread_id() : 0,
        GetCurrentThreadPcForDebug(), GetCurrentThreadLrForDebug());
  }
  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();
  bool forced_timeout = false;
  if (is_main_thread && cvars::main_thread_wait_fallback_ms > 0) {
    auto forced_ms =
        std::chrono::milliseconds(cvars::main_thread_wait_fallback_ms);
    if (timeout_ms == std::chrono::milliseconds::max() ||
        timeout_ms > forced_ms) {
      XELOGW(
          "DEBUG: MainThread wait fallback (SignalAndWait): overriding timeout {} -> {} ms",
          timeout_ms == std::chrono::milliseconds::max()
              ? -1
              : static_cast<int64_t>(timeout_ms.count()),
          static_cast<int64_t>(forced_ms.count()));
      timeout_ms = forced_ms;
      forced_timeout = true;
    }
  }

  auto result = xe::threading::SignalAndWait(
      signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
      alertable ? true : false, timeout_ms);
  switch (result) {
    case xe::threading::WaitResult::kSuccess:
      if (watch_match_signal || watch_match_wait) {
        bool signal_event_state_valid = false;
        uint32_t signal_event_type = 0;
        uint32_t signal_event_state = 0;
        QueryEventStateForWatchLog(signal_object, &signal_event_state_valid,
                                   &signal_event_type, &signal_event_state);
        bool wait_event_state_valid = false;
        uint32_t wait_event_type = 0;
        uint32_t wait_event_state = 0;
        QueryEventStateForWatchLog(wait_object, &wait_event_state_valid,
                                   &wait_event_type, &wait_event_state);
        XELOGW(
            "RING BUFFER: XObject::SignalAndWait WATCH_HIT result=SUCCESS "
            "signal_event_state_valid={} signal_event_type={:08X} "
            "signal_event_state={:08X} wait_event_state_valid={} "
            "wait_event_type={:08X} wait_event_state={:08X}",
            signal_event_state_valid ? "YES" : "NO", signal_event_type,
            signal_event_state, wait_event_state_valid ? "YES" : "NO",
            wait_event_type, wait_event_state);
      }
      wait_object->WaitCallback();
      return X_STATUS_SUCCESS;
    case xe::threading::WaitResult::kUserCallback:
      if (watch_match_signal || watch_match_wait) {
        XELOGW(
            "RING BUFFER: XObject::SignalAndWait WATCH_HIT "
            "result=USER_CALLBACK");
      }
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case xe::threading::WaitResult::kTimeout:
      if (watch_match_signal || watch_match_wait) {
        XELOGW(
            "RING BUFFER: XObject::SignalAndWait WATCH_HIT result=TIMEOUT");
      }
      xe::threading::MaybeYield();
      if (forced_timeout && cvars::main_thread_wait_force_success) {
        XELOGW("DEBUG: MainThread wait fallback (SignalAndWait): returning SUCCESS");
        return X_STATUS_SUCCESS;
      }
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      if (watch_match_signal || watch_match_wait) {
        XELOGW(
            "RING BUFFER: XObject::SignalAndWait WATCH_HIT "
            "result=FAILED_OR_ABANDONED");
      }
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects,
                               uint32_t wait_type, uint32_t wait_reason,
                               uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  XThread* current_thread = XThread::GetCurrentThread();
  bool is_main_thread = current_thread && current_thread->main_thread();
  uint32_t watch_object_ptr = cvars::gpu_bootstrap_wait_object_watch;
  bool watch_match = false;
  xe::threading::WaitHandle* wait_handles[64];

  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
    if (watch_object_ptr != 0 && objects[i] &&
        objects[i]->guest_object() == watch_object_ptr) {
      watch_match = true;
    }
  }
  if (watch_match) {
    XELOGW(
        "RING BUFFER: XObject::WaitMultiple WATCH_HIT pre watch_obj={:08X} "
        "count={} wait_type={} wait_reason={} processor_mode={} alertable={} "
        "timeout_ptr={} thread_id={} pc={:08X} lr={:08X}",
        watch_object_ptr, count, wait_type, wait_reason, processor_mode,
        alertable, opt_timeout ? "NON_NULL" : "NULL",
        current_thread ? current_thread->thread_id() : 0,
        GetCurrentThreadPcForDebug(), GetCurrentThreadLrForDebug());
    for (size_t i = 0; i < count; ++i) {
      bool event_state_valid = false;
      uint32_t event_type = 0;
      uint32_t event_state = 0;
      QueryEventStateForWatchLog(objects[i], &event_state_valid, &event_type,
                                 &event_state);
      XELOGW(
          "RING BUFFER: XObject::WaitMultiple WATCH_HIT obj[{}] ptr={:08X} "
          "type={} handle={:08X} event_state_valid={} event_type={:08X} "
          "event_state={:08X} watch_match={}",
          i, objects[i] ? objects[i]->guest_object() : 0,
          objects[i] ? static_cast<uint32_t>(objects[i]->type()) : 0,
          objects[i] ? objects[i]->handle() : 0,
          event_state_valid ? "YES" : "NO", event_type, event_state,
          (objects[i] && objects[i]->guest_object() == watch_object_ptr)
              ? "YES"
              : "NO");
    }
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();
  bool forced_timeout = false;
  if (is_main_thread && cvars::main_thread_wait_fallback_ms > 0) {
    auto forced_ms =
        std::chrono::milliseconds(cvars::main_thread_wait_fallback_ms);
    if (timeout_ms == std::chrono::milliseconds::max() ||
        timeout_ms > forced_ms) {
      XELOGW(
          "DEBUG: MainThread wait fallback (WaitMultiple): overriding timeout {} -> {} ms",
          timeout_ms == std::chrono::milliseconds::max()
              ? -1
              : static_cast<int64_t>(timeout_ms.count()),
          static_cast<int64_t>(forced_ms.count()));
      timeout_ms = forced_ms;
      forced_timeout = true;
    }
  }

  if (wait_type) {
    auto result = xe::threading::WaitAny(wait_handles, count,
                                         alertable ? true : false, timeout_ms);
    switch (result.first) {
      case xe::threading::WaitResult::kSuccess:
        if (watch_match) {
          XELOGW(
              "RING BUFFER: XObject::WaitMultiple WATCH_HIT result=SUCCESS "
              "mode=WAIT_ANY index={}",
              result.second);
        }
        objects[result.second]->WaitCallback();

        return X_STATUS(result.second);
      case xe::threading::WaitResult::kUserCallback:
        if (watch_match) {
          XELOGW(
              "RING BUFFER: XObject::WaitMultiple WATCH_HIT "
              "result=USER_CALLBACK mode=WAIT_ANY");
        }
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case xe::threading::WaitResult::kTimeout:
        if (watch_match) {
          XELOGW(
              "RING BUFFER: XObject::WaitMultiple WATCH_HIT "
              "result=TIMEOUT mode=WAIT_ANY");
        }
        xe::threading::MaybeYield();
        if (forced_timeout && cvars::main_thread_wait_force_success) {
          XELOGW(
              "DEBUG: MainThread wait fallback (WaitAny): returning SUCCESS index 0");
          return X_STATUS_SUCCESS;
        }
        return X_STATUS_TIMEOUT;
      default:
      case xe::threading::WaitResult::kAbandoned:
        if (watch_match) {
          XELOGW(
              "RING BUFFER: XObject::WaitMultiple WATCH_HIT "
              "result=ABANDONED mode=WAIT_ANY index={}",
              result.second);
        }
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
      case xe::threading::WaitResult::kFailed:
        if (watch_match) {
          XELOGW(
              "RING BUFFER: XObject::WaitMultiple WATCH_HIT "
              "result=FAILED mode=WAIT_ANY");
        }
        return X_STATUS_UNSUCCESSFUL;
    }
  } else {
    auto result = xe::threading::WaitAll(wait_handles, count,
                                         alertable ? true : false, timeout_ms);
    switch (result) {
      case xe::threading::WaitResult::kSuccess:
        if (watch_match) {
          XELOGW(
              "RING BUFFER: XObject::WaitMultiple WATCH_HIT result=SUCCESS "
              "mode=WAIT_ALL");
        }
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
        }

        return X_STATUS_SUCCESS;
      case xe::threading::WaitResult::kUserCallback:
        if (watch_match) {
          XELOGW(
              "RING BUFFER: XObject::WaitMultiple WATCH_HIT "
              "result=USER_CALLBACK mode=WAIT_ALL");
        }
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case xe::threading::WaitResult::kTimeout:
        if (watch_match) {
          XELOGW(
              "RING BUFFER: XObject::WaitMultiple WATCH_HIT "
              "result=TIMEOUT mode=WAIT_ALL");
        }
        xe::threading::MaybeYield();
        if (forced_timeout && cvars::main_thread_wait_force_success) {
          XELOGW(
              "DEBUG: MainThread wait fallback (WaitAll): returning SUCCESS");
          return X_STATUS_SUCCESS;
        }
        return X_STATUS_TIMEOUT;
      default:
      case xe::threading::WaitResult::kAbandoned:
      case xe::threading::WaitResult::kFailed:
        if (watch_match) {
          XELOGW(
              "RING BUFFER: XObject::WaitMultiple WATCH_HIT "
              "result=FAILED_OR_ABANDONED mode=WAIT_ALL");
        }
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);
  
  // Enhanced debugging - track CreateNative calls for host objects
  if (host_object_) {
    XELOGI("DEBUG: CreateNative called for HOST object:");
    XELOGI("   Object type: {}", static_cast<uint32_t>(type_));
    XELOGI("   Requested size: {} bytes", size);
    XELOGI("   Total size with header: {} bytes", total_size);
    XELOGI("   Handle: {:08X}", handles_.empty() ? 0 : handles_[0]);
    XELOGI("   Thread ID: {}", std::this_thread::get_id());
    
    // Check current thread context for debugging
    // CRITICAL: Removed GetCurrentThread() call here because it takes tls_global_mutex_ 
    // while we hold the global_critical_region, causing a lock inversion deadlock on macOS.
    XELOGI("   Thread ID: {}", std::this_thread::get_id());
  }

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    if (host_object_) {
      XELOGI("DEBUG: SystemHeapAlloc FAILED - out of memory");
    }
    // Out of memory!
    return nullptr;
  }
  
  if (host_object_) {
    XELOGI("DEBUG: SystemHeapAlloc SUCCESS - allocated at: {:08X}", mem);
    XELOGI("   Memory region: {:08X} - {:08X}", mem, mem + total_size);
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  
  if (host_object_) {
    XELOGI("   Memory zeroed successfully");
    XELOGI("   About to call SetNativePointer at: {:08X}", mem + sizeof(X_OBJECT_HEADER));
  }
  
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  if (host_object_) {
    XELOGI("DEBUG: SetNativePointer completed successfully");
    XELOGI("   About to call TranslateVirtual<X_OBJECT_HEADER*> at: {:08X}", mem);
  }

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);
  
  if (host_object_) {
    if (header) {
      XELOGI("DEBUG: TranslateVirtual<X_OBJECT_HEADER*> SUCCESS");
      XELOGI("   Header pointer: {:08X}", mem);
      XELOGI("   Translated header: {:08X}", reinterpret_cast<uintptr_t>(header));
    } else {
      XELOGI("DEBUG: TranslateVirtual<X_OBJECT_HEADER*> FAILED - returned nullptr");
    }
  }

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (host_object_) {
    if (object_type) {
      XELOGI("DEBUG: Object type allocation SUCCESS: {:08X}", object_type);
    } else {
      XELOGI("DEBUG: Object type allocation FAILED");
    }
  }
  
  if (object_type) {
    // Set it up in header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
    
  if (host_object_) {
    XELOGI("   About to call SetNativePointer at: {:08X}", mem + sizeof(X_OBJECT_HEADER));
  }
  
  printf("DEBUG: BREADCRUMB CreateNative: About to call SetNativePointer(%08X, true)\n", mem + sizeof(X_OBJECT_HEADER));
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);
  printf("DEBUG: BREADCRUMB CreateNative: SetNativePointer returned\n");

  if (host_object_) {
    XELOGI("DEBUG: SetNativePointer completed successfully");
    XELOGI("   About to call TranslateVirtual<X_OBJECT_HEADER*> at: {:08X}", mem);
    printf("DEBUG: BREADCRUMB CreateNative: About to call TranslateVirtual<X_OBJECT_HEADER*>(%08X)\n", mem);
  }
  }

  auto result = memory()->TranslateVirtual(guest_object_ptr_);
  if (host_object_) {
    if (result) {
      XELOGI("DEBUG: CreateNative FINAL SUCCESS - result: {:08X}", reinterpret_cast<uintptr_t>(result));
    } else {
      XELOGI("DEBUG: CreateNative FINAL FAILURE - TranslateVirtual returned nullptr");
    }
  }
  
  return result;
}

void xe::kernel::XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  // CRITICAL FIX: Prevent assertion failure from duplicate SetNativePointer calls
  if (guest_object_ptr_ != 0) {
    printf("DEBUG: BREADCRUMB: SetNativePointer called but guest_object_ptr_ already set: %08X\n", guest_object_ptr_);
    printf("DEBUG: BREADCRUMB: Skipping duplicate SetNativePointer call to prevent assertion failure\n");
    XELOGI("DEBUG: SetNativePointer already called - skipping duplicate call");
    return;
  }

  if (host_object_) {
    XELOGI("DEBUG: SetNativePointer called for HOST object:");
    XELOGI("   Object type: {}", static_cast<uint32_t>(type_));
    XELOGI("   Native ptr: {:08X}", native_ptr);
    XELOGI("   Handle: {:08X}", handles_.empty() ? 0 : handles_[0]);
    XELOGI("   Thread ID: {}", std::this_thread::get_id());
    XELOGI("   Uninitialized: {}", uninitialized ? "YES" : "NO");
  }

  printf("DEBUG: BREADCRUMB: About to call TranslateVirtual for native_ptr: %08X\n", native_ptr);
  auto header =
      kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);
  printf("DEBUG: BREADCRUMB: TranslateVirtual returned header: %p\n",
         static_cast<void*>(header));

  if (host_object_) {
    if (header) {
      XELOGI("DEBUG: TranslateVirtual<X_DISPATCH_HEADER*> SUCCESS");
      XELOGI("   Header pointer: {:08X}", reinterpret_cast<uintptr_t>(header));
      printf("DEBUG: BREADCRUMB: Header is valid, about to access wait_list\n");
    } else {
      XELOGI("DEBUG: TranslateVirtual<X_DISPATCH_HEADER*> FAILED - returned nullptr");
      XELOGI("   This is likely the bus error crash point!");
      printf("DEBUG: BREADCRUMB: ERROR - TranslateVirtual returned nullptr\n");
      return;  // Early exit to prevent crash
    }
  }

  printf("DEBUG: BREADCRUMB: About to check uninitialized flag: %s\n", uninitialized ? "YES" : "NO");
  if (!uninitialized) {
    printf("DEBUG: BREADCRUMB: About to access header->wait_list.blink_ptr\n");
    assert_true(!(header->wait_list.blink_ptr & 0x1));
    printf("DEBUG: BREADCRUMB: header->wait_list.blink_ptr accessed successfully\n");
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  if (host_object_) {
    XELOGI("   About to call StashHandle with header: {:08X}, handle: {:08X}", 
              reinterpret_cast<uintptr_t>(header), handles_.empty() ? 0 : handles_[0]);
    printf("DEBUG: BREADCRUMB: About to call StashHandle\n");
  }
  
  printf("DEBUG: BREADCRUMB: Calling StashHandle(header, handle=%08X)\n", handles_.empty() ? 0 : handles_[0]);
  StashHandle(header, handle());
  printf("DEBUG: BREADCRUMB: StashHandle completed successfully\n");

  guest_object_ptr_ = native_ptr;

  if (host_object_) {
    XELOGI("DEBUG: SetNativePointer completed successfully");
    XELOGI("   guest_object_ptr_ set to: {:08X}", native_ptr);
  }
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state,
                                              void* native_ptr, int32_t as_type,
                                              bool already_locked) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list.flink_ptr to a magic value. When set,
  // wait_list.blink_ptr will hold a handle to our object.
  if (!already_locked) {
    global_critical_region::mutex().lock();
  }

  XObject* result;

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list.flink_ptr == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list.blink_ptr;
    result = kernel_state->object_table()
                 ->LookupObject<XObject>(handle, true)
                 .release();
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        result = nullptr;
    }
    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    if (object) {
      StashHandle(header, object->handle());
    }
    result = object;
  }

  if (!already_locked) {
    global_critical_region::mutex().unlock();
  }
  return xe::kernel::object_ref<XObject>(result);
}

}  // namespace kernel
}  // namespace xe
