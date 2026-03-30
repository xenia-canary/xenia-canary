/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_X64_CODE_CACHE_H_
#define XENIA_CPU_BACKEND_X64_X64_CODE_CACHE_H_

#include <memory>

#include "xenia/cpu/backend/code_cache_base.h"

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

class X64CodeCache : public CodeCacheBase<X64CodeCache> {
 public:
  ~X64CodeCache() override = default;

  static std::unique_ptr<X64CodeCache> Create();

  virtual bool Initialize();

  void* LookupUnwindInfo(uint64_t host_pc) override { return nullptr; }

  // CRTP hooks for CodeCacheBase.
  void FillCode(void* write_address, size_t size);
  void FlushCodeRange(void* address, size_t size);
  void BeginCodeWrite() {}
  void EndCodeWrite() {}
  void OnCodePlaced(uint32_t guest_address, GuestFunction* function_info,
                    void* code_execute_address, size_t code_size);

  // Virtual for platform-specific overrides (_win.cc / _posix.cc).
  virtual UnwindReservation RequestUnwindReservation(uint8_t* entry_address) {
    return UnwindReservation();
  }
  virtual void PlaceCode(uint32_t guest_address, void* machine_code,
                         const EmitFunctionInfo& func_info,
                         void* code_execute_address,
                         UnwindReservation unwind_reservation) {}

 protected:
  X64CodeCache() = default;
};

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_X64_X64_CODE_CACHE_H_
