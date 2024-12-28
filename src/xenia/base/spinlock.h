/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_SPINLOCK_H_
#define XENIA_BASE_SPINLOCK_H_

#include <atomic>

namespace xe {
class spinlock {
 public:
  void lock() {
    while (locked.test_and_set(std::memory_order_acquire)) {
      ;
    }
  }
  void unlock() { locked.clear(std::memory_order_release); }

 private:
  std::atomic_flag locked = ATOMIC_FLAG_INIT;
};

}  // namespace xe
#endif