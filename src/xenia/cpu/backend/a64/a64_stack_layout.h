/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_STACK_LAYOUT_H_
#define XENIA_CPU_BACKEND_A64_A64_STACK_LAYOUT_H_

#include "xenia/base/vec128.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class StackLayout {
 public:
  /**
   * ARM64 Thunk Stack Layout
   * NOTE: stack must always be 16-byte aligned.
   *
   * Thunk (HostToGuest/GuestToHost):
   *  +------------------+
   *  | x19              | sp + 0x000
   *  | x20 (context)    | sp + 0x008
   *  | x21 (membase)    | sp + 0x010
   *  | x22              | sp + 0x018
   *  | x23              | sp + 0x020
   *  | x24              | sp + 0x028
   *  | x25              | sp + 0x030
   *  | x26              | sp + 0x038
   *  | x27              | sp + 0x040
   *  | x28              | sp + 0x048
   *  | x29 (fp)         | sp + 0x050
   *  | x30 (lr)         | sp + 0x058
   *  | d8               | sp + 0x060
   *  | d9               | sp + 0x068
   *  | d10              | sp + 0x070
   *  | d11              | sp + 0x078
   *  | d12              | sp + 0x080
   *  | d13              | sp + 0x088
   *  | d14              | sp + 0x090
   *  | d15              | sp + 0x098
   *  +------------------+
   *  Total: 0xA0 = 160 bytes (already 16-byte aligned)
   */
  static constexpr size_t THUNK_STACK_SIZE = 160;

  /**
   * ARM64 Guest Stack Layout
   *  +------------------+
   *  | scratch, 48b     | sp + 0x000  (3 x Q for VMX FP scratch)
   *  | guest ret addr   | sp + 0x030  (guest PPC return address)
   *  | call ret addr    | sp + 0x038  (next call's guest PPC return addr)
   *  | host ret addr    | sp + 0x040  (host x30/LR, for ret instruction)
   *  | guest saved r1   | sp + 0x048  (guest r1 at function entry, for
   *  |                  |              longjmp detection)
   *  |  ... locals ...  |
   *  +------------------+
   *
   * Minimum size: 80 bytes (aligned to 16).
   *
   * Convention: at guest function entry, x0 holds the guest PPC return
   * address. The prolog stores it to GUEST_RET_ADDR and saves x30 (host
   * LR) to HOST_RET_ADDR.
   */
  static constexpr size_t GUEST_STACK_SIZE = 80;  // 16-byte aligned
  static constexpr size_t GUEST_SCRATCH = 0;      // 48 bytes (3 x Q)
  static constexpr size_t GUEST_RET_ADDR = 48;
  static constexpr size_t GUEST_CALL_RET_ADDR = 56;
  static constexpr size_t HOST_RET_ADDR = 64;
  // Stackpoint depth after PushStackpoint in prolog, for longjmp detection.
  static constexpr size_t GUEST_SAVED_STACKPOINT_DEPTH = 72;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_STACK_LAYOUT_H_
