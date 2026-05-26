/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/threading.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <time.h>

#include <cerrno>

namespace xe {
namespace threading {

uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_id() {
  mach_port_t tid = pthread_mach_thread_np(pthread_self());
  return static_cast<uint32_t>(tid);
}

void set_name(const std::string& name) { pthread_setname_np(name.c_str()); }

void MaybeYield() { pthread_yield_np(); }

void Sleep(std::chrono::microseconds duration) {
  timespec rqtp = {duration.count() / 1000000, duration.count() % 1000};
  nanosleep(&rqtp, nullptr);
  // TODO(benvanik): spin while rmtp >0?
}

// Requires macOS 14.4 / iOS 17.4
bool WaitOnAddress32(std::atomic<uint32_t>* addr, uint32_t expected,
                     std::chrono::milliseconds timeout) {
  int rc;
  if (timeout.count() < 0) {
    rc =
        os_sync_wait_on_address(addr, static_cast<uint64_t>(expected),
                                sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_NONE);
  } else {
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
    rc = os_sync_wait_on_address_with_timeout(
        addr, static_cast<uint64_t>(expected), sizeof(uint32_t),
        OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
        static_cast<uint64_t>(ns));
  }
  // Returns >=0 on wake (remaining waiter count), -1 on error.
  if (rc >= 0) {
    return true;
  }
  return errno != ETIMEDOUT;
}

void WakeOneByAddress32(std::atomic<uint32_t>* addr) {
  os_sync_wake_by_address_any(addr, sizeof(uint32_t),
                              OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

}  // namespace threading
}  // namespace xe
