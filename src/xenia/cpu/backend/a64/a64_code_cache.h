/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_
#define XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_

#include <cstdint>
#include <memory>
#include <mutex>

#include "xenia/cpu/backend/code_cache_base.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class A64CodeCache : public CodeCacheBase<A64CodeCache> {
 public:
  ~A64CodeCache() override;

  static std::unique_ptr<A64CodeCache> Create();

  virtual bool Initialize();

  // Whether the indirection table uses encoded (relative) entries rather
  // than direct absolute 32-bit host addresses.  True when fixed-address
  // allocation failed (e.g. macOS ARM64).
  bool encoded_indirection() const { return encoded_indirection_; }

  // Indirection table operations — shadow the base class to handle encoded
  // mode.  Callers go through A64CodeCache* so these are found first.
  void set_indirection_default(uint32_t default_value);
  void set_indirection_default_encoded(uint64_t default_value);
  void AddIndirection(uint32_t guest_address, uint32_t host_address);
  void AddIndirectionEncoded(uint32_t guest_address, uint64_t host_address);
  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high);

  // CRTP hook: called from CodeCacheBase::PlaceGuestCode to write the
  // indirection entry for a newly placed function.
  void OnPlaceGuestCodeIndirection(uint32_t guest_address,
                                   void* code_execute_address);

  uintptr_t execute_base_address() const override {
    return reinterpret_cast<uintptr_t>(generated_code_execute_base_);
  }

  uint64_t* external_table() const { return external_table_; }
  uint8_t* indirection_table_base() const { return indirection_table_base_; }
  uint8_t* generated_code_execute_base() const {
    return generated_code_execute_base_;
  }

  void* LookupUnwindInfo(uint64_t host_pc) override { return nullptr; }

  // CRTP hooks for CodeCacheBase.
  void FillCode(void* write_address, size_t size);
  void FlushCodeRange(void* address, size_t size);

  // Virtual for platform-specific overrides (_win.cc / _posix.cc).
  virtual UnwindReservation RequestUnwindReservation(uint8_t* entry_address) {
    return UnwindReservation();
  }
  virtual void PlaceCode(uint32_t guest_address, void* machine_code,
                         const EmitFunctionInfo& func_info,
                         void* code_execute_address,
                         UnwindReservation unwind_reservation) {}

 protected:
  A64CodeCache() = default;

 private:
  uint32_t EncodeIndirectionTarget(uint64_t host_address);
  uint32_t AllocateExternalSlot(uint64_t host_address);

  bool encoded_indirection_ = false;

  // External table for addresses outside the code cache (trampolines).
  static constexpr size_t kMaxExternalEntries = 32768;
  uint64_t* external_table_ = nullptr;
  uint32_t external_table_count_ = 0;
  std::mutex external_table_mutex_;

  // Encoded-mode default indirection value (code-cache-relative offset of
  // the resolve thunk).
  uint32_t encoded_default_value_ = 0;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_
