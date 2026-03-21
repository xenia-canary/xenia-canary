/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_code_cache.h"

#include <cstring>

#if XE_PLATFORM_MAC && XE_ARCH_ARM64
#include <pthread.h>
#endif

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
#include "xenia/base/assert.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

A64CodeCache::~A64CodeCache() {
#if XE_A64_INDIRECTION_64BIT
  if (indirection_table_base_) {
    xe::memory::DeallocFixed(indirection_table_base_, kA64IndirectionTableSize,
                             xe::memory::DeallocationType::kRelease);
    indirection_table_base_ = nullptr;
  }
#endif
#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  // The macOS ARM64 JIT path uses a direct MAP_JIT allocation instead of a
  // shared file mapping, so free it here before the generic base teardown.
  if (mapping_ == xe::memory::kFileMappingHandleInvalid &&
      generated_code_execute_base_) {
    xe::memory::DeallocFixed(generated_code_execute_base_, kGeneratedCodeSize,
                             xe::memory::DeallocationType::kRelease);
    generated_code_execute_base_ = nullptr;
    generated_code_write_base_ = nullptr;
  }
#endif
}

bool A64CodeCache::InitializeEncodedIndirectionTableState() {
#if XE_A64_INDIRECTION_64BIT
  if (!indirection_table_base_) {
    return false;
  }
  indirection_table_actual_base_ =
      reinterpret_cast<uintptr_t>(indirection_table_base_);
  indirection_table_base_bias_ = indirection_table_actual_base_ -
                                 static_cast<uintptr_t>(kIndirectionTableBase);
  external_indirection_targets_ = std::make_unique<uint64_t[]>(
      static_cast<size_t>(kIndirectionExternalCapacity));
  if (!external_indirection_targets_) {
    XELOGE("Unable to allocate ARM64 external indirection target table");
    return false;
  }
  external_indirection_target_count_.store(0, std::memory_order_relaxed);
#endif
  return true;
}

bool A64CodeCache::InitializeEncodedIndirectionTable() {
#if XE_A64_INDIRECTION_64BIT
  indirection_table_base_ = reinterpret_cast<uint8_t*>(xe::memory::AllocFixed(
      nullptr, kA64IndirectionTableSize, xe::memory::AllocationType::kReserve,
      xe::memory::PageAccess::kNoAccess));
  if (!indirection_table_base_) {
    XELOGE("Unable to reserve ARM64 indirection table");
    return false;
  }
  return InitializeEncodedIndirectionTableState();
#else
  return true;
#endif
}

bool A64CodeCache::Initialize() {
#if XE_A64_INDIRECTION_64BIT
#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  if (xe::memory::IsWritableExecutableMemoryPreferred()) {
    if (!InitializeEncodedIndirectionTable()) {
      return false;
    }

    generated_code_execute_base_ = reinterpret_cast<uint8_t*>(
        xe::memory::AllocFixed(nullptr, kGeneratedCodeSize,
                               xe::memory::AllocationType::kReserveCommit,
                               xe::memory::PageAccess::kExecuteReadWrite));
    generated_code_write_base_ = generated_code_execute_base_;
    if (!generated_code_execute_base_) {
      XELOGE("Unable to allocate macOS ARM64 JIT code cache");
      return false;
    }

    generated_code_commit_mark_ = kGeneratedCodeSize;
    generated_code_map_.reserve(kMaximumFunctionCount);
    return true;
  }
#endif

  if (!CodeCacheBase<A64CodeCache>::Initialize()) {
    return false;
  }
  return InitializeEncodedIndirectionTableState();
#else
  return CodeCacheBase<A64CodeCache>::Initialize();
#endif
}

void A64CodeCache::set_indirection_default(uint32_t default_value) {
  CodeCacheBase<A64CodeCache>::set_indirection_default(default_value);
}

#if XE_A64_INDIRECTION_64BIT
uint32_t A64CodeCache::EncodeIndirectionTarget(uint64_t host_address) {
  const uintptr_t code_base = execute_base_address();
  const uintptr_t code_end = code_base + kGeneratedCodeSize;
  if (host_address >= code_base && host_address < code_end) {
    return static_cast<uint32_t>(host_address - code_base);
  }

  if (!external_indirection_targets_) {
    XELOGE("ARM64 external indirection table is unavailable");
    return indirection_default_value_;
  }

  std::lock_guard<std::mutex> lock(external_indirection_mutex_);
  const uint32_t current_count =
      external_indirection_target_count_.load(std::memory_order_relaxed);
  if (current_count >= kIndirectionExternalCapacity) {
    XELOGE("ARM64 external indirection table overflow");
    return indirection_default_value_;
  }

  external_indirection_targets_[current_count] = host_address;
  external_indirection_target_count_.store(current_count + 1,
                                           std::memory_order_release);
  return kIndirectionExternalTag | current_count;
}

void A64CodeCache::set_indirection_default_64(uint64_t default_value) {
  CodeCacheBase<A64CodeCache>::set_indirection_default(
      EncodeIndirectionTarget(default_value));
}
#endif

void A64CodeCache::AddIndirection(uint32_t guest_address,
                                  uint32_t host_address) {
#if XE_A64_INDIRECTION_64BIT
  AddIndirection64(guest_address, host_address);
#else
  PublishIndirection(guest_address, host_address);
#endif
}

#if XE_A64_INDIRECTION_64BIT
void A64CodeCache::AddIndirection64(uint32_t guest_address,
                                    uint64_t host_address) {
  PublishIndirection(guest_address, host_address);
}
#endif

void A64CodeCache::CommitExecutableRange(uint32_t guest_low,
                                         uint32_t guest_high) {
  if (!indirection_table_base_) {
    return;
  }

#if XE_A64_INDIRECTION_64BIT
  if (guest_low < kIndirectionTableBase || guest_high < guest_low) {
    return;
  }
  const uint64_t start_offset =
      uint64_t(guest_low) - uint64_t(kIndirectionTableBase);
  const uint64_t size = uint64_t(guest_high) - uint64_t(guest_low);
  if (start_offset + size > kA64IndirectionTableSize) {
    return;
  }
  if (!xe::memory::AllocFixed(indirection_table_base_ + start_offset, size,
                              xe::memory::AllocationType::kCommit,
                              xe::memory::PageAccess::kReadWrite)) {
    return;
  }
  auto* slots =
      reinterpret_cast<uint32_t*>(indirection_table_base_ + start_offset);
  const uint64_t entry_count = size / kA64IndirectionEntrySize;
  for (uint64_t i = 0; i < entry_count; ++i) {
    slots[i] = indirection_default_value_;
  }
#else
  xe::memory::AllocFixed(
      indirection_table_base_ + (guest_low - kIndirectionTableBase),
      guest_high - guest_low, xe::memory::AllocationType::kCommit,
      xe::memory::PageAccess::kReadWrite);
  uint32_t* slots = reinterpret_cast<uint32_t*>(indirection_table_base_);
  for (uint32_t address = guest_low; address < guest_high; address += 4) {
    slots[(address - kIndirectionTableBase) / 4] = indirection_default_value_;
  }
#endif
}

void A64CodeCache::PlaceHostCode(uint32_t guest_address, void* machine_code,
                                 const EmitFunctionInfo& func_info,
                                 void*& code_execute_address_out,
                                 void*& code_write_address_out) {
  PlaceGuestCode(guest_address, machine_code, func_info, nullptr,
                 code_execute_address_out, code_write_address_out);
}

void A64CodeCache::PlaceGuestCode(uint32_t guest_address, void* machine_code,
                                  const EmitFunctionInfo& func_info,
                                  GuestFunction* function_info,
                                  void*& code_execute_address_out,
                                  void*& code_write_address_out) {
  size_t high_mark = 0;
  uint8_t* code_execute_address = nullptr;
  uint8_t* tail_execute_address = nullptr;
  uint8_t* end_execute_address = nullptr;
  UnwindReservation unwind_reservation;
  {
    auto global_lock = global_critical_region_.Acquire();

    code_execute_address =
        generated_code_execute_base_ + generated_code_offset_;
    code_execute_address_out = code_execute_address;
    uint8_t* code_write_address =
        generated_code_write_base_ + generated_code_offset_;
    code_write_address_out = code_write_address;
    generated_code_offset_ += xe::round_up(func_info.code_size.total, 16);

    uint8_t* tail_write_address =
        generated_code_write_base_ + generated_code_offset_;
    tail_execute_address =
        generated_code_execute_base_ + generated_code_offset_;

    unwind_reservation = RequestUnwindReservation(generated_code_write_base_ +
                                                  generated_code_offset_);
    generated_code_offset_ += xe::round_up(unwind_reservation.data_size, 16);

    uint8_t* end_write_address =
        generated_code_write_base_ + generated_code_offset_;
    end_execute_address = generated_code_execute_base_ + generated_code_offset_;
    high_mark = generated_code_offset_;

    generated_code_map_.emplace_back(
        (uint64_t(code_execute_address - generated_code_execute_base_) << 32) |
            generated_code_offset_,
        function_info);

    EnsureCommitted(high_mark);

#if XE_PLATFORM_MAC && XE_ARCH_ARM64
    const bool jit_write =
        generated_code_execute_base_ == generated_code_write_base_;
    if (jit_write) {
      pthread_jit_write_protect_np(0);
    }
#endif

    std::memcpy(code_write_address, machine_code, func_info.code_size.total);
    if (end_write_address > tail_write_address) {
      FillCode(tail_write_address,
               static_cast<size_t>(end_write_address - tail_write_address));
    }
    PlaceCode(guest_address, machine_code, func_info, code_execute_address,
              unwind_reservation);

#if XE_PLATFORM_MAC && XE_ARCH_ARM64
    if (jit_write) {
      pthread_jit_write_protect_np(1);
    }
#endif
  }

  FlushCodeRange(code_execute_address, func_info.code_size.total);
  if (tail_execute_address < end_execute_address) {
    FlushCodeRange(
        tail_execute_address,
        static_cast<size_t>(end_execute_address - tail_execute_address));
  }

  if (guest_address && indirection_table_base_) {
    PublishIndirection(guest_address,
                       reinterpret_cast<uint64_t>(code_execute_address));
  }
}

uint32_t A64CodeCache::PlaceData(const void* data, size_t length) {
  size_t high_mark = 0;
  uint8_t* data_execute_address = nullptr;
  uint8_t* data_write_address = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    data_execute_address =
        generated_code_execute_base_ + generated_code_offset_;
    data_write_address = generated_code_write_base_ + generated_code_offset_;
    generated_code_offset_ += xe::round_up(length, 16);
    high_mark = generated_code_offset_;
  }

  EnsureCommitted(high_mark);

#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  const bool jit_write =
      generated_code_execute_base_ == generated_code_write_base_;
  if (jit_write) {
    pthread_jit_write_protect_np(0);
  }
#endif
  std::memcpy(data_write_address, data, length);
#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  if (jit_write) {
    pthread_jit_write_protect_np(1);
  }
#endif

  FlushCodeRange(data_execute_address, length);
  return static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(data_execute_address));
}

void A64CodeCache::FillCode(void* write_address, size_t size) {
  // Fill with BRK #0 (0xD4200000), 4-byte aligned.
  constexpr uint32_t kBrk0 = 0xD4200000;
  auto* p = reinterpret_cast<uint32_t*>(write_address);
  auto* end =
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(write_address) + size);
  for (; p < end; ++p) {
    *p = kBrk0;
  }
}

void A64CodeCache::FlushCodeRange(void* address, size_t size) {
#if XE_PLATFORM_WIN32
  FlushInstructionCache(GetCurrentProcess(), address, size);
#else
  __builtin___clear_cache(
      reinterpret_cast<char*>(address),
      reinterpret_cast<char*>(static_cast<uint8_t*>(address) + size));
#endif
}

void A64CodeCache::EnsureCommitted(size_t high_mark) {
  using namespace xe::literals;
  size_t old_commit_mark = 0;
  size_t new_commit_mark = 0;
  do {
    old_commit_mark = generated_code_commit_mark_;
    if (high_mark <= old_commit_mark) {
      break;
    }
    new_commit_mark = old_commit_mark + 16_MiB;
    if (generated_code_execute_base_ == generated_code_write_base_) {
      xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                             xe::memory::AllocationType::kCommit,
                             xe::memory::PageAccess::kExecuteReadWrite);
    } else {
      xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                             xe::memory::AllocationType::kCommit,
                             xe::memory::PageAccess::kExecuteReadOnly);
      xe::memory::AllocFixed(generated_code_write_base_, new_commit_mark,
                             xe::memory::AllocationType::kCommit,
                             xe::memory::PageAccess::kReadWrite);
    }
  } while (generated_code_commit_mark_.compare_exchange_weak(old_commit_mark,
                                                             new_commit_mark));
}

void A64CodeCache::PublishIndirection(uint32_t guest_address,
                                      uint64_t host_address) {
  if (!indirection_table_base_ || guest_address < kIndirectionTableBase) {
    return;
  }

#if XE_A64_INDIRECTION_64BIT
  const uint64_t guest_offset =
      uint64_t(guest_address) - uint64_t(kIndirectionTableBase);
  if (guest_offset + kA64IndirectionEntrySize > kA64IndirectionTableSize) {
    return;
  }
  auto* slot =
      reinterpret_cast<uint32_t*>(indirection_table_base_ + guest_offset);
  *slot = EncodeIndirectionTarget(host_address);
#else
  auto* slot = reinterpret_cast<uint32_t*>(
      indirection_table_base_ + (guest_address - kIndirectionTableBase));
  *slot = static_cast<uint32_t>(host_address);
#endif
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
