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

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

A64CodeCache::~A64CodeCache() {
  delete[] external_table_;
  external_table_ = nullptr;
}

bool A64CodeCache::Initialize() {
  // Try the fast path: fixed-address allocation.
  if (CodeCacheBase::Initialize()) {
    encoded_indirection_ = false;
    return true;
  }

  // Fixed allocation failed (e.g. macOS ARM64). Fall back to dynamic.
  XELOGI(
      "A64CodeCache: fixed-address allocation failed; using encoded "
      "indirection fallback");
  encoded_indirection_ = true;

  // Allocate indirection table at any available address.
  indirection_table_base_ = reinterpret_cast<uint8_t*>(xe::memory::AllocFixed(
      nullptr, kIndirectionTableSize, xe::memory::AllocationType::kReserve,
      xe::memory::PageAccess::kReadWrite));
  if (!indirection_table_base_) {
    XELOGE("A64CodeCache: unable to allocate indirection table (dynamic)");
    return false;
  }

  // Create file mapping for the code cache.
  file_name_ = fmt::format("xenia_code_cache_{}", Clock::QueryHostTickCount());
  mapping_ = xe::memory::CreateFileMappingHandle(
      file_name_, kGeneratedCodeSize, xe::memory::PageAccess::kExecuteReadWrite,
      false);
  if (mapping_ == xe::memory::kFileMappingHandleInvalid) {
    XELOGE("A64CodeCache: unable to create code cache file mapping (dynamic)");
    return false;
  }

  // Map execute and write views at OS-chosen addresses.
  if (xe::memory::IsWritableExecutableMemoryPreferred()) {
    generated_code_execute_base_ = reinterpret_cast<uint8_t*>(
        xe::memory::MapFileView(mapping_, nullptr, kGeneratedCodeSize,
                                xe::memory::PageAccess::kExecuteReadWrite, 0));
    generated_code_write_base_ = generated_code_execute_base_;
  } else {
    generated_code_execute_base_ = reinterpret_cast<uint8_t*>(
        xe::memory::MapFileView(mapping_, nullptr, kGeneratedCodeSize,
                                xe::memory::PageAccess::kExecuteReadOnly, 0));
    generated_code_write_base_ = reinterpret_cast<uint8_t*>(
        xe::memory::MapFileView(mapping_, nullptr, kGeneratedCodeSize,
                                xe::memory::PageAccess::kReadWrite, 0));
  }
  if (!generated_code_execute_base_ || !generated_code_write_base_) {
    XELOGE("A64CodeCache: unable to map code cache views (dynamic)");
    return false;
  }

  XELOGI("A64CodeCache: indirection table at {:016X}, code cache at {:016X}",
         reinterpret_cast<uint64_t>(indirection_table_base_),
         reinterpret_cast<uint64_t>(generated_code_execute_base_));

  // Allocate external table for trampoline addresses.
  external_table_ = new uint64_t[kMaxExternalEntries]();
  external_table_count_ = 0;

  generated_code_map_.reserve(kMaximumFunctionCount);
  return true;
}

void A64CodeCache::set_indirection_default(uint32_t default_value) {
  indirection_default_value_ = default_value;
}

void A64CodeCache::set_indirection_default_encoded(uint64_t default_value) {
  // The resolve thunk is in the code cache, encode as cache-relative offset.
  uint64_t code_base = reinterpret_cast<uint64_t>(generated_code_execute_base_);
  encoded_default_value_ = EncodeIndirectionTarget(default_value);
  // Also set the base class default for CommitExecutableRange to use.
  indirection_default_value_ = encoded_default_value_;
}

uint32_t A64CodeCache::EncodeIndirectionTarget(uint64_t host_address) {
  uint64_t code_base = reinterpret_cast<uint64_t>(generated_code_execute_base_);
  uint64_t code_end = code_base + kGeneratedCodeSize;
  if (host_address >= code_base && host_address < code_end) {
    uint32_t offset = static_cast<uint32_t>(host_address - code_base);
    assert_true((offset & 0x80000000) == 0);
    return offset;
  }
  return AllocateExternalSlot(host_address) | 0x80000000;
}

uint32_t A64CodeCache::AllocateExternalSlot(uint64_t host_address) {
  std::lock_guard<std::mutex> lock(external_table_mutex_);
  // Check if already registered.
  for (uint32_t i = 0; i < external_table_count_; i++) {
    if (external_table_[i] == host_address) {
      return i;
    }
  }
  assert_true(external_table_count_ < kMaxExternalEntries);
  uint32_t index = external_table_count_++;
  external_table_[index] = host_address;
  return index;
}

void A64CodeCache::AddIndirection(uint32_t guest_address,
                                  uint32_t host_address) {
  if (!encoded_indirection_) {
    CodeCacheBase::AddIndirection(guest_address, host_address);
    return;
  }
  AddIndirectionEncoded(guest_address, static_cast<uint64_t>(host_address));
}

void A64CodeCache::AddIndirectionEncoded(uint32_t guest_address,
                                         uint64_t host_address) {
  if (!indirection_table_base_) return;
  uint32_t* slot = reinterpret_cast<uint32_t*>(
      indirection_table_base_ + (guest_address - kIndirectionTableBase));
  *slot = EncodeIndirectionTarget(host_address);
}

void A64CodeCache::CommitExecutableRange(uint32_t guest_low,
                                         uint32_t guest_high) {
  if (!encoded_indirection_) {
    CodeCacheBase::CommitExecutableRange(guest_low, guest_high);
    return;
  }
  if (!indirection_table_base_) return;

  xe::memory::AllocFixed(
      indirection_table_base_ + (guest_low - kIndirectionTableBase),
      guest_high - guest_low, xe::memory::AllocationType::kCommit,
      xe::memory::PageAccess::kReadWrite);
  uint32_t* p = reinterpret_cast<uint32_t*>(indirection_table_base_);
  for (uint32_t address = guest_low; address < guest_high; address += 4) {
    p[(address - kIndirectionTableBase) / 4] = encoded_default_value_;
  }
}

void A64CodeCache::OnPlaceGuestCodeIndirection(uint32_t guest_address,
                                               void* code_execute_address) {
  if (!indirection_table_base_ || !guest_address) return;
  if (!encoded_indirection_) {
    uint32_t* slot = reinterpret_cast<uint32_t*>(
        indirection_table_base_ + (guest_address - kIndirectionTableBase));
    *slot = uint32_t(reinterpret_cast<uint64_t>(code_execute_address));
  } else {
    AddIndirectionEncoded(guest_address,
                          reinterpret_cast<uint64_t>(code_execute_address));
  }
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

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
