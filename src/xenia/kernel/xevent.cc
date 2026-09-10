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
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {

XEvent::XEvent(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() = default;

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  assert_false(event_);

  manual_reset_ = manual_reset;
  CreateNative<X_KEVENT>();
  auto* kevent = memory()->TranslateVirtual<X_KEVENT*>(guest_object());
  // Don't touch header.wait_list: SetNativePointer stashes the handle there.
  kevent->header.type = manual_reset
                            ? X_OBJECT_TYPES::EventNotificationObject
                            : X_OBJECT_TYPES::EventSynchronizationObject;
  kevent->header.signal_state = initial_state ? 1 : 0;
  host_signaled_ = initial_state;

  if (manual_reset) {
    event_ = xe::threading::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = xe::threading::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::InitializeNative(void* native_ptr,
                              const X_DISPATCH_HEADER* header) {
  assert_false(event_);

  switch (header->type) {
    case X_OBJECT_TYPES::EventNotificationObject:
      manual_reset_ = true;
      break;
    case X_OBJECT_TYPES::EventSynchronizationObject:
      manual_reset_ = false;
      break;
    default:
      assert_always();
      return;
  }

  bool initial_state = header->signal_state ? true : false;
  host_signaled_ = initial_state;
  if (manual_reset_) {
    event_ = xe::threading::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = xe::threading::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
  SetNativePointer(memory()->HostToGuestVirtual(native_ptr), true);
}

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  set_priority_increment(priority_increment);
  {
    std::lock_guard<std::mutex> lock(state_lock_);
    memory()->TranslateVirtual<X_KEVENT*>(guest_object())->header.signal_state =
        1;
    host_signaled_ = true;
    event_->Set();
  }
  return 1;
}

int32_t XEvent::Pulse(uint32_t priority_increment, bool wait) {
  set_priority_increment(priority_increment);
  auto* kevent = memory()->TranslateVirtual<X_KEVENT*>(guest_object());
  // KePulseEvent returns the pre-pulse signal state.
  std::lock_guard<std::mutex> lock(state_lock_);
  const int32_t old_signal_state = kevent->header.signal_state;
  event_->Pulse();
  // Pulse leaves the event reset after releasing waiters.
  kevent->header.signal_state = 0;
  host_signaled_ = false;
  return old_signal_state;
}

int32_t XEvent::Reset() {
  std::lock_guard<std::mutex> lock(state_lock_);
  memory()->TranslateVirtual<X_KEVENT*>(guest_object())->header.signal_state =
      0;
  host_signaled_ = false;
  event_->Reset();
  return 1;
}

void XEvent::WaitCallback() {
  // Auto-reset events atomically clear on successful wait; manual stay set.
  if (!manual_reset_) {
    std::lock_guard<std::mutex> lock(state_lock_);
    memory()->TranslateVirtual<X_KEVENT*>(guest_object())->header.signal_state =
        0;
    host_signaled_ = false;
  }
}

void XEvent::SyncFromGuest() {
  if (!guest_object()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(state_lock_);
    auto* kevent = memory()->TranslateVirtual<X_KEVENT*>(guest_object());
    bool guest_signaled = kevent->header.signal_state ? true : false;
    if (guest_signaled == host_signaled_) {
      return;
    }
    host_signaled_ = guest_signaled;
    if (guest_signaled) {
      event_->Set();
    } else {
      event_->Reset();
    }
  }
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
