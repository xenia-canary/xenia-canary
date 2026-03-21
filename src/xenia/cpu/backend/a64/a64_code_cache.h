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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "xenia/cpu/backend/code_cache_base.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

#if XE_ARCH_ARM64
#define XE_A64_INDIRECTION_64BIT 1
#else
#define XE_A64_INDIRECTION_64BIT 0
#endif

class A64CodeCache : public CodeCacheBase<A64CodeCache> {
 public:
  ~A64CodeCache() override;

  static std::unique_ptr<A64CodeCache> Create();

  virtual bool Initialize();

  void set_indirection_default(uint32_t default_value);
#if XE_A64_INDIRECTION_64BIT
  void set_indirection_default_64(uint64_t default_value);
#endif
  void AddIndirection(uint32_t guest_address, uint32_t host_address);
#if XE_A64_INDIRECTION_64BIT
  void AddIndirection64(uint32_t guest_address, uint64_t host_address);
#endif
  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high);
  void PlaceHostCode(uint32_t guest_address, void* machine_code,
                     const EmitFunctionInfo& func_info,
                     void*& code_execute_address_out,
                     void*& code_write_address_out);
  void PlaceGuestCode(uint32_t guest_address, void* machine_code,
                      const EmitFunctionInfo& func_info,
                      GuestFunction* function_info,
                      void*& code_execute_address_out,
                      void*& code_write_address_out);
  uint32_t PlaceData(const void* data, size_t length);

  uintptr_t execute_base_address() const override {
    return generated_code_execute_base_
               ? reinterpret_cast<uintptr_t>(generated_code_execute_base_)
               : CodeCacheBase<A64CodeCache>::execute_base_address();
  }

  uintptr_t indirection_table_base_address() const {
    return indirection_table_actual_base_;
  }
#if XE_A64_INDIRECTION_64BIT
  uintptr_t indirection_table_base_bias() const {
    return indirection_table_base_bias_;
  }
  uintptr_t external_indirection_table_base_address() const {
    return reinterpret_cast<uintptr_t>(external_indirection_targets_.get());
  }
#endif

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
  void EnsureCommitted(size_t high_mark);
  void PublishIndirection(uint32_t guest_address, uint64_t host_address);
  bool InitializeEncodedIndirectionTable();
  bool InitializeEncodedIndirectionTableState();
  uint32_t EncodeIndirectionTarget(uint64_t host_address);

#if XE_A64_INDIRECTION_64BIT
  static constexpr size_t kA64IndirectionTableSize = 0x20000000ull;

 public:
  static constexpr size_t kA64IndirectionEntrySize = sizeof(uint32_t);
  static constexpr uint32_t kIndirectionExternalTag = 0x80000000u;
  static constexpr uint32_t kIndirectionExternalIndexMask = 0x7FFFFFFFu;
  static constexpr uint32_t kIndirectionExternalCapacity = 0x00010000u;

 private:
  std::unique_ptr<uint64_t[]> external_indirection_targets_;
  std::atomic<uint32_t> external_indirection_target_count_ = {0};
  std::mutex external_indirection_mutex_;
#else
  static constexpr size_t kA64IndirectionTableSize = kIndirectionTableSize;
  static constexpr size_t kA64IndirectionEntrySize = sizeof(uint32_t);
#endif
  uintptr_t indirection_table_actual_base_ = 0;
#if XE_A64_INDIRECTION_64BIT
  uintptr_t indirection_table_base_bias_ = 0;
#endif
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_
