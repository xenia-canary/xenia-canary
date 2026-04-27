/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xevent.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/xthread_mac.h"

DECLARE_uint32(gpu_bootstrap_wait_object_watch);

namespace xe {
namespace kernel {

namespace {

bool IsWatchedEvent(XEvent* event) {
  uint32_t watch_object = cvars::gpu_bootstrap_wait_object_watch;
  return watch_object && event && event->guest_object() == watch_object;
}

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

void LogWatchedEventTransition(const char* operation, XEvent* event,
                               uint32_t before_type, uint32_t before_state,
                               uint32_t after_type, uint32_t after_state) {
  uint32_t watch_object = cvars::gpu_bootstrap_wait_object_watch;
  if (!watch_object || !event) {
    return;
  }
  uint32_t event_guest_ptr = event->guest_object();
  if (event_guest_ptr != watch_object) {
    return;
  }
  auto* thread = XThread::GetCurrentThread();
  XELOGW(
      "RING BUFFER: XEvent::{} WATCH_HIT obj_ptr={:08X} handle={:08X} "
      "before_type={:08X} before_state={:08X} after_type={:08X} "
      "after_state={:08X} thread_id={} pc={:08X} lr={:08X}",
      operation ? operation : "<unknown>", event_guest_ptr, event->handle(),
      before_type, before_state, after_type, after_state,
      thread ? thread->thread_id() : 0, GetCurrentThreadPcForDebug(),
      GetCurrentThreadLrForDebug());
}

}  // namespace

XEvent::XEvent(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() = default;

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  assert_false(event_);

  this->CreateNative<X_KEVENT>();

  if (manual_reset) {
    event_ = xe::threading::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = xe::threading::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  assert_false(event_);

  switch (header->type) {
    case 0x00:  // EventNotificationObject (manual reset)
      manual_reset_ = true;
      break;
    case 0x01:  // EventSynchronizationObject (auto reset)
      manual_reset_ = false;
      break;
    default:
      assert_always();
      return;
  }

  bool initial_state = header->signal_state ? true : false;
  if (manual_reset_) {
    event_ = xe::threading::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = xe::threading::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  if (!IsWatchedEvent(this)) {
    event_->Set();
    return 1;
  }
  uint32_t before_type = 0;
  uint32_t before_state = 0;
  Query(&before_type, &before_state);
  event_->Set();
  uint32_t after_type = 0;
  uint32_t after_state = 0;
  Query(&after_type, &after_state);
  LogWatchedEventTransition("Set", this, before_type, before_state, after_type,
                            after_state);
  return 1;
}

int32_t XEvent::Pulse(uint32_t priority_increment, bool wait) {
  if (!IsWatchedEvent(this)) {
    event_->Pulse();
    return 1;
  }
  uint32_t before_type = 0;
  uint32_t before_state = 0;
  Query(&before_type, &before_state);
  event_->Pulse();
  uint32_t after_type = 0;
  uint32_t after_state = 0;
  Query(&after_type, &after_state);
  LogWatchedEventTransition("Pulse", this, before_type, before_state,
                            after_type, after_state);
  return 1;
}

int32_t XEvent::Reset() {
  if (!IsWatchedEvent(this)) {
    event_->Reset();
    return 1;
  }
  uint32_t before_type = 0;
  uint32_t before_state = 0;
  Query(&before_type, &before_state);
  event_->Reset();
  uint32_t after_type = 0;
  uint32_t after_state = 0;
  Query(&after_type, &after_state);
  LogWatchedEventTransition("Reset", this, before_type, before_state,
                            after_type, after_state);
  return 1;
}
void XEvent::Query(uint32_t* out_type, uint32_t* out_state) {
  auto [type, state] = event_->Query();

  *out_type = type;
  *out_state = state;
}
void XEvent::Clear() { event_->Reset(); }

bool XEvent::Save(ByteStream* stream) {
  XELOGD("XEvent {:08X} ({})", handle(), manual_reset_ ? "manual" : "auto");
  SaveObject(stream);

  bool signaled = true;
  auto result =
      xe::threading::Wait(event_.get(), false, std::chrono::milliseconds(0));
  if (result == xe::threading::WaitResult::kSuccess) {
    signaled = true;
  } else if (result == xe::threading::WaitResult::kTimeout) {
    signaled = false;
  } else {
    assert_always();
  }

  if (signaled) {
    // Reset the event in-case it's an auto-reset.
    event_->Set();
  }

  stream->Write<bool>(signaled);
  stream->Write<bool>(manual_reset_);

  return true;
}

object_ref<XEvent> XEvent::Restore(KernelState* kernel_state,
                                   ByteStream* stream) {
  auto evt = new XEvent(nullptr);
  evt->kernel_state_ = kernel_state;

  evt->RestoreObject(stream);
  bool signaled = stream->Read<bool>();
  evt->manual_reset_ = stream->Read<bool>();

  if (evt->manual_reset_) {
    evt->event_ = xe::threading::Event::CreateManualResetEvent(false);
  } else {
    evt->event_ = xe::threading::Event::CreateAutoResetEvent(false);
  }
  assert_not_null(evt->event_);

  if (signaled) {
    evt->event_->Set();
  }

  return object_ref<XEvent>(evt);
}

}  // namespace kernel
}  // namespace xe
