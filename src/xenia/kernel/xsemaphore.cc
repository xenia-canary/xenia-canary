/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsemaphore.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {

XSemaphore::XSemaphore(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XSemaphore::~XSemaphore() = default;

bool XSemaphore::Initialize(int32_t initial_count, int32_t maximum_count) {
  assert_false(semaphore_);

  CreateNative(sizeof(X_KSEMAPHORE));
  auto* semaphore = memory()->TranslateVirtual<X_KSEMAPHORE*>(guest_object());
  semaphore->header.type = X_OBJECT_TYPES::SemaphoreObject;
  semaphore->header.signal_state = initial_count;
  semaphore->limit = maximum_count;

  maximum_count_ = maximum_count;
  host_count_ = initial_count;
  semaphore_ = xe::threading::Semaphore::Create(initial_count, maximum_count);
  return !!semaphore_;
}

bool XSemaphore::InitializeNative(void* native_ptr,
                                  const X_DISPATCH_HEADER* header) {
  assert_false(semaphore_);
  SetNativePointer(memory()->HostToGuestVirtual(native_ptr));
  auto semaphore = reinterpret_cast<X_KSEMAPHORE*>(native_ptr);
  maximum_count_ = semaphore->limit;
  host_count_ = static_cast<int32_t>(semaphore->header.signal_state);
  semaphore_ = xe::threading::Semaphore::Create(semaphore->header.signal_state,
                                                semaphore->limit);
  return !!semaphore_;
}

bool XSemaphore::ReleaseSemaphore(int32_t release_count,
                                  int32_t* out_previous_count) {
  int32_t previous_count = 0;

  bool success = false;
  {
    std::lock_guard<std::mutex> lock(count_lock_);
    success = semaphore_->Release(release_count, &previous_count);
    if (success) {
      host_count_ += release_count;
      memory()
          ->TranslateVirtual<X_KSEMAPHORE*>(guest_object())
          ->header.signal_state = host_count_;
    }
  }

  if (out_previous_count) {
    *out_previous_count = previous_count;
  }
  return success;
}

bool XSemaphore::Save(ByteStream* stream) {
  if (!SaveObject(stream)) {
    return false;
  }

  // Get the free number of slots from the semaphore.
  uint32_t free_count = 0;
  while (
      threading::Wait(semaphore_.get(), false, std::chrono::milliseconds(0)) ==
      threading::WaitResult::kSuccess) {
    free_count++;
  }

  XELOGD("XSemaphore {:08X} (count {}/{})", handle(), free_count,
         maximum_count_);

  // Restore the semaphore back to its previous count.
  semaphore_->Release(free_count, nullptr);

  stream->Write(maximum_count_);
  stream->Write(free_count);

  return true;
}

void XSemaphore::WaitCallback() {
  std::lock_guard<std::mutex> lock(count_lock_);
  host_count_ -= 1;
  memory()
      ->TranslateVirtual<X_KSEMAPHORE*>(guest_object())
      ->header.signal_state = host_count_;
}

void XSemaphore::SyncFromGuest() {
  if (!guest_object()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(count_lock_);
    auto& signal_state = memory()
                             ->TranslateVirtual<X_KSEMAPHORE*>(guest_object())
                             ->header.signal_state;
    int32_t guest_count = static_cast<int32_t>(signal_state);
    if (guest_count == host_count_) {
      return;
    }
    // The header only holds what this kernel last put there, so anything else
    // came from the guest, such as an in-place re-initialize.
    if (guest_count > host_count_) {
      int32_t delta = guest_count - host_count_;
      if (!semaphore_->Release(delta, nullptr)) {
        return;  // over the limit, leave the host count alone
      }
      host_count_ += delta;
    } else {
      // Take back permits the guest dropped, never blocking for one a waiter
      // already claimed.
      int32_t delta = host_count_ - guest_count;
      int32_t drained = 0;
      while (drained < delta &&
             xe::threading::Wait(semaphore_.get(), false,
                                 std::chrono::milliseconds(0)) ==
                 xe::threading::WaitResult::kSuccess) {
        ++drained;
      }
      host_count_ -= drained;
    }
    signal_state = host_count_;
  }
}

object_ref<XSemaphore> XSemaphore::Restore(KernelState* kernel_state,
                                           ByteStream* stream) {
  auto sem = new XSemaphore(nullptr);
  sem->kernel_state_ = kernel_state;

  if (!sem->RestoreObject(stream)) {
    return nullptr;
  }

  sem->maximum_count_ = stream->Read<uint32_t>();
  auto free_count = stream->Read<uint32_t>();
  XELOGD("XSemaphore {:08X} (count {}/{})", sem->handle(), free_count,
         sem->maximum_count_);

  sem->semaphore_ =
      threading::Semaphore::Create(free_count, sem->maximum_count_);
  assert_not_null(sem->semaphore_);
  sem->host_count_ = static_cast<int32_t>(free_count);

  return object_ref<XSemaphore>(sem);
}

}  // namespace kernel
}  // namespace xe
