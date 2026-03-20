/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_code_cache.h"

#include <cstdlib>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform_win.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/function.h"

// Function pointer definitions for growable function tables.
using FnRtlAddGrowableFunctionTable = decltype(&RtlAddGrowableFunctionTable);
using FnRtlGrowFunctionTable = decltype(&RtlGrowFunctionTable);
using FnRtlDeleteGrowableFunctionTable =
    decltype(&RtlDeleteGrowableFunctionTable);

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

// ARM64 .xdata unwind codes.
// See: https://learn.microsoft.com/en-us/cpp/build/arm64-exception-handling
//
// Codes are stored as a byte array (big-endian for multi-byte codes), listed
// in reverse prolog order (last prolog instruction's code first).
//
// For the thunk prolog:
//   sub sp, sp, #0xA0        (alloc_s or alloc_m)
//   stp x19,x20, [sp, #0x00] (save_regp)
//   stp x21,x22, [sp, #0x10] (save_regp)
//   stp x23,x24, [sp, #0x20] (save_regp)
//   stp x25,x26, [sp, #0x30] (save_regp)
//   stp x27,x28, [sp, #0x40] (save_regp)
//   stp x29,x30, [sp, #0x50] (save_fplr)
//   stp d8, d9,  [sp, #0x60] (save_fregp)
//   stp d10,d11, [sp, #0x70] (save_fregp)
//   stp d12,d13, [sp, #0x80] (save_fregp)
//   stp d14,d15, [sp, #0x90] (save_fregp)

// ARM64 unwind code builders.
// alloc_s: 000XXXXX, allocate X*16 bytes (0..496).
static void EmitAllocS(uint8_t* buf, size_t& off, uint32_t size_bytes) {
  assert_true(size_bytes <= 496 && (size_bytes % 16) == 0);
  buf[off++] = static_cast<uint8_t>(size_bytes / 16);
}

// alloc_m: 11000XXX XXXXXXXX, allocate X*16 bytes (0..32752).
static void EmitAllocM(uint8_t* buf, size_t& off, uint32_t size_bytes) {
  assert_true(size_bytes <= 32752 && (size_bytes % 16) == 0);
  uint16_t val = static_cast<uint16_t>(size_bytes / 16);
  buf[off++] = static_cast<uint8_t>(0xC0 | ((val >> 8) & 0x07));
  buf[off++] = static_cast<uint8_t>(val & 0xFF);
}

// save_regp: 110010XX XXzzzzzz
// Save x(19+X), x(20+X) pair at [sp + Z*8].
// X is the register offset from x19 (not a pair ordinal).
// e.g., x21,x22 -> X=2, x27,x28 -> X=8.
static void EmitSaveRegp(uint8_t* buf, size_t& off, uint32_t reg_offset,
                         uint32_t sp_offset) {
  assert_true(reg_offset <= 10);
  assert_true((sp_offset % 8) == 0 && sp_offset / 8 <= 63);
  uint32_t z = sp_offset / 8;
  buf[off++] = static_cast<uint8_t>(0xC8 | ((reg_offset >> 2) & 0x03));
  buf[off++] = static_cast<uint8_t>(((reg_offset & 0x03) << 6) | (z & 0x3F));
}

// save_fplr: 01zzzzzz
// Save <x29, lr> at [sp + Z*8].
static void EmitSaveFplr(uint8_t* buf, size_t& off, uint32_t sp_offset) {
  assert_true((sp_offset % 8) == 0 && sp_offset / 8 <= 63);
  buf[off++] = static_cast<uint8_t>(0x40 | (sp_offset / 8));
}

// save_fregp: 1101100X XXzzzzzz
// Save d(8+X), d(9+X) pair at [sp + Z*8].
// X is the register offset from d8 (not a pair ordinal).
// e.g., d10,d11 -> X=2, d14,d15 -> X=6.
static void EmitSaveFregp(uint8_t* buf, size_t& off, uint32_t reg_offset,
                          uint32_t sp_offset) {
  assert_true(reg_offset <= 7);
  assert_true((sp_offset % 8) == 0 && sp_offset / 8 <= 63);
  uint32_t z = sp_offset / 8;
  buf[off++] = static_cast<uint8_t>(0xD8 | ((reg_offset >> 2) & 0x01));
  buf[off++] = static_cast<uint8_t>(((reg_offset & 0x03) << 6) | (z & 0x3F));
}

// end: 0xE4
static void EmitEnd(uint8_t* buf, size_t& off) { buf[off++] = 0xE4; }

// Build the .xdata unwind codes for a thunk prolog that saves callee-saved
// registers at known offsets (see StackLayout in a64_stack_layout.h).
// Returns the number of bytes written.
static size_t BuildThunkUnwindCodes(uint8_t* buf) {
  size_t off = 0;
  // Codes listed in reverse prolog order (last prolog instruction first).
  // stp d14, d15, [sp, #0x90]  — d14 = d(8+6)
  EmitSaveFregp(buf, off, 6, 0x90);
  // stp d12, d13, [sp, #0x80]  — d12 = d(8+4)
  EmitSaveFregp(buf, off, 4, 0x80);
  // stp d10, d11, [sp, #0x70]  — d10 = d(8+2)
  EmitSaveFregp(buf, off, 2, 0x70);
  // stp d8,  d9,  [sp, #0x60]  — d8 = d(8+0)
  EmitSaveFregp(buf, off, 0, 0x60);
  // stp x29, x30, [sp, #0x50]
  EmitSaveFplr(buf, off, 0x50);
  // stp x27, x28, [sp, #0x40]  — x27 = x(19+8)
  EmitSaveRegp(buf, off, 8, 0x40);
  // stp x25, x26, [sp, #0x30]  — x25 = x(19+6)
  EmitSaveRegp(buf, off, 6, 0x30);
  // stp x23, x24, [sp, #0x20]  — x23 = x(19+4)
  EmitSaveRegp(buf, off, 4, 0x20);
  // stp x21, x22, [sp, #0x10]  — x21 = x(19+2)
  EmitSaveRegp(buf, off, 2, 0x10);
  // stp x19, x20, [sp, #0x00]  — x19 = x(19+0)
  EmitSaveRegp(buf, off, 0, 0x00);
  // sub sp, sp, #0xA0 (160 bytes)
  EmitAllocS(buf, off, StackLayout::THUNK_STACK_SIZE);
  EmitEnd(buf, off);
  return off;
}

// Build minimal unwind codes for a guest function prolog.
// Guest functions only do: sub sp, sp, #N; str x30, [sp, #64]
// The callee-saved registers were already saved by the thunk.
static size_t BuildGuestUnwindCodes(uint8_t* buf, uint32_t stack_size) {
  size_t off = 0;
  // The guest function stores x30 at [sp + HOST_RET_ADDR] via STR, not STP.
  // Windows unwinder needs to know where LR is to unwind. We encode this as
  // save_lrpair — but there's no single-register LR save opcode on ARM64.
  // Instead we describe the stack allocation only. The host return address
  // is stored by the JIT but is not a callee-save operation (it's the thunk's
  // LR, not the guest function's). The unwinder will walk up to the thunk
  // frame which has the full unwind info.
  if (stack_size <= 496) {
    EmitAllocS(buf, off, stack_size);
  } else {
    EmitAllocM(buf, off, stack_size);
  }
  EmitEnd(buf, off);
  return off;
}

// Size of .xdata record for a thunk (header + codes + padding).
// Thunk codes: 5x save_regp(2B) + 1x save_fplr(1B) + 4x save_fregp(2B) +
//              1x alloc_s(1B) + end(1B) = 21 bytes -> 24 bytes padded -> 6
//              code words. Header is 1 word. Total: 7 words = 28 bytes.
static constexpr uint32_t kThunkXdataSize = 28;

// Size of .xdata record for a guest function (header + codes + padding).
// Guest codes: alloc_s(1B) or alloc_m(2B) + end(1B) = 2-3 bytes -> 4 bytes
//              padded -> 1 code word. Header is 1 word. Total: 2 words = 8
//              bytes. We reserve the larger case.
static constexpr uint32_t kGuestXdataSize = 12;

// Compute the maximum unwind data size for any function.
static constexpr uint32_t kMaxUnwindSize = kThunkXdataSize;

// Build a complete .xdata record. Returns the total size written.
static size_t BuildXdataRecord(uint8_t* xdata, uint32_t func_length_bytes,
                               const uint8_t* codes, size_t codes_length) {
  // Pad codes to 4-byte boundary.
  size_t codes_padded = xe::round_up(codes_length, size_t{4});
  uint32_t code_words = static_cast<uint32_t>(codes_padded / 4);
  assert_true(code_words <= 31);
  assert_true(func_length_bytes % 4 == 0);

  uint32_t func_len_div4 = func_length_bytes / 4;
  assert_true(func_len_div4 <= 0x3FFFF);

  // Header word:
  //   bits 0-17:  Function Length / 4
  //   bits 18-19: Version = 0
  //   bit  20:    X = 0 (no exception handler)
  //   bit  21:    E = 1 (single epilog, packed in header)
  //   bits 22-26: Epilog start index (0 = epilog uses same codes from start)
  //   bits 27-31: Code Words
  uint32_t header = (func_len_div4 & 0x3FFFF) | (0u << 18)  // Vers = 0
                    | (0u << 20)                            // X = 0
                    | (1u << 21)                            // E = 1
                    | (0u << 22)  // Epilog start index = 0
                    | (code_words << 27);

  std::memcpy(xdata, &header, 4);

  // Write codes, zero-padded to code_words * 4 bytes.
  std::memset(xdata + 4, 0, codes_padded);
  std::memcpy(xdata + 4, codes, codes_length);

  return 4 + codes_padded;
}

class Win32A64CodeCache : public A64CodeCache {
 public:
  Win32A64CodeCache();
  ~Win32A64CodeCache() override;

  bool Initialize() override;

  void* LookupUnwindInfo(uint64_t host_pc) override;

 private:
  UnwindReservation RequestUnwindReservation(uint8_t* entry_address) override;
  void PlaceCode(uint32_t guest_address, void* machine_code,
                 const EmitFunctionInfo& func_info, void* code_execute_address,
                 UnwindReservation unwind_reservation) override;

  void InitializeUnwindEntry(uint8_t* unwind_entry_address,
                             size_t unwind_table_slot,
                             void* code_execute_address,
                             const EmitFunctionInfo& func_info);

  // Growable function table system handle.
  void* unwind_table_handle_ = nullptr;
  // Actual unwind table entries.
  std::vector<RUNTIME_FUNCTION> unwind_table_;
  // End addresses for each entry (ARM64 RUNTIME_FUNCTION lacks EndAddress).
  std::vector<DWORD> unwind_table_end_address_;
  // Current number of entries in the table.
  std::atomic<uint32_t> unwind_table_count_ = {0};
  // Does this version of Windows support growable function tables?
  bool supports_growable_table_ = false;

  FnRtlAddGrowableFunctionTable add_growable_table_ = nullptr;
  FnRtlDeleteGrowableFunctionTable delete_growable_table_ = nullptr;
  FnRtlGrowFunctionTable grow_table_ = nullptr;
};

std::unique_ptr<A64CodeCache> A64CodeCache::Create() {
  return std::make_unique<Win32A64CodeCache>();
}

Win32A64CodeCache::Win32A64CodeCache() = default;

Win32A64CodeCache::~Win32A64CodeCache() {
  if (supports_growable_table_) {
    if (unwind_table_handle_) {
      delete_growable_table_(unwind_table_handle_);
    }
  } else {
    if (generated_code_execute_base_) {
      RtlDeleteFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(
          reinterpret_cast<DWORD64>(generated_code_execute_base_) | 0x3));
    }
  }
}

bool Win32A64CodeCache::Initialize() {
  if (!A64CodeCache::Initialize()) {
    return false;
  }

  // Allocate unwind table with maximum entry count.
  unwind_table_.resize(kMaximumFunctionCount);
  unwind_table_end_address_.resize(kMaximumFunctionCount);

  // Check if this version of Windows supports growable function tables.
  auto ntdll_handle = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll_handle) {
    add_growable_table_ = nullptr;
    delete_growable_table_ = nullptr;
    grow_table_ = nullptr;
  } else {
    add_growable_table_ = (FnRtlAddGrowableFunctionTable)GetProcAddress(
        ntdll_handle, "RtlAddGrowableFunctionTable");
    delete_growable_table_ = (FnRtlDeleteGrowableFunctionTable)GetProcAddress(
        ntdll_handle, "RtlDeleteGrowableFunctionTable");
    grow_table_ = (FnRtlGrowFunctionTable)GetProcAddress(
        ntdll_handle, "RtlGrowFunctionTable");
  }
  supports_growable_table_ =
      add_growable_table_ && delete_growable_table_ && grow_table_;

  if (supports_growable_table_) {
    if (add_growable_table_(
            &unwind_table_handle_, unwind_table_.data(), unwind_table_count_,
            DWORD(unwind_table_.size()),
            reinterpret_cast<ULONG_PTR>(generated_code_execute_base_),
            reinterpret_cast<ULONG_PTR>(generated_code_execute_base_ +
                                        kGeneratedCodeSize))) {
      XELOGE("Unable to create unwind function table");
      return false;
    }
  } else {
    if (!RtlInstallFunctionTableCallback(
            reinterpret_cast<DWORD64>(generated_code_execute_base_) | 0x3,
            reinterpret_cast<DWORD64>(generated_code_execute_base_),
            kGeneratedCodeSize,
            [](DWORD64 control_pc, PVOID context) {
              auto code_cache = reinterpret_cast<Win32A64CodeCache*>(context);
              return reinterpret_cast<PRUNTIME_FUNCTION>(
                  code_cache->LookupUnwindInfo(control_pc));
            },
            this, nullptr)) {
      XELOGE("Unable to install function table callback");
      return false;
    }
  }

  return true;
}

Win32A64CodeCache::UnwindReservation
Win32A64CodeCache::RequestUnwindReservation(uint8_t* entry_address) {
#if defined(NDEBUG)
  if (unwind_table_count_ >= kMaximumFunctionCount) {
    xe::FatalError(
        "Unwind table count exceeded maximum! Please report this to "
        "Xenia/Canary developers");
  }
#else
  assert_false(unwind_table_count_ >= kMaximumFunctionCount);
#endif
  UnwindReservation unwind_reservation;
  unwind_reservation.data_size = xe::round_up(kMaxUnwindSize, size_t{16});
  unwind_reservation.table_slot = unwind_table_count_++;
  unwind_reservation.entry_address = entry_address;
  return unwind_reservation;
}

void Win32A64CodeCache::PlaceCode(uint32_t guest_address, void* machine_code,
                                  const EmitFunctionInfo& func_info,
                                  void* code_execute_address,
                                  UnwindReservation unwind_reservation) {
  // Add unwind info.
  InitializeUnwindEntry(unwind_reservation.entry_address,
                        unwind_reservation.table_slot, code_execute_address,
                        func_info);

  if (supports_growable_table_) {
    grow_table_(unwind_table_handle_, unwind_table_count_);
  }

  FlushInstructionCache(GetCurrentProcess(), code_execute_address,
                        func_info.code_size.total);
}

void Win32A64CodeCache::InitializeUnwindEntry(
    uint8_t* unwind_entry_address, size_t unwind_table_slot,
    void* code_execute_address, const EmitFunctionInfo& func_info) {
  uint8_t codes[32];
  size_t codes_length;

  // Guest function prologs are exactly 4 instructions (16 bytes):
  //   sub sp, sp, #N; str x30, [sp, #64]; str x0, [sp, #48]; str xzr, [sp, #56]
  // The HostToGuest thunk has a much larger prolog that saves all
  // callee-saved registers. We detect it by stack size.
  // Other thunks (GuestToHost, ResolveFunction) have different layouts
  // but are only called from within JIT'd code; stack-alloc-only unwind
  // info is sufficient for them since the unwinder will walk up to the
  // HostToGuest frame which has full unwind info.
  bool is_host_to_guest_thunk =
      (func_info.stack_size == StackLayout::THUNK_STACK_SIZE &&
       func_info.code_size.prolog > 16);

  if (is_host_to_guest_thunk) {
    codes_length = BuildThunkUnwindCodes(codes);
  } else if (func_info.stack_size > 0) {
    codes_length = BuildGuestUnwindCodes(
        codes, static_cast<uint32_t>(func_info.stack_size));
  } else {
    // Thunks with no stack allocation (e.g. GuestToHost, ResolveFunction)
    // still need minimal unwind info — emit empty unwind codes.
    codes_length = 0;
  }

  size_t xdata_size = BuildXdataRecord(
      unwind_entry_address, static_cast<uint32_t>(func_info.code_size.total),
      codes, codes_length);

  // Add RUNTIME_FUNCTION entry.
  // ARM64 RUNTIME_FUNCTION has BeginAddress and UnwindData but no EndAddress
  // (the function length is encoded in the .xdata header).
  auto& fn_entry = unwind_table_[unwind_table_slot];
  fn_entry.BeginAddress =
      DWORD(reinterpret_cast<uint8_t*>(code_execute_address) -
            generated_code_execute_base_);
  fn_entry.UnwindData =
      DWORD(unwind_entry_address - generated_code_execute_base_);
  // Store end address in parallel array for LookupUnwindInfo.
  unwind_table_end_address_[unwind_table_slot] =
      DWORD(fn_entry.BeginAddress + func_info.code_size.total);
}

void* Win32A64CodeCache::LookupUnwindInfo(uint64_t host_pc) {
  // ARM64 RUNTIME_FUNCTION lacks EndAddress, so we do a manual binary search
  // using our parallel end address array.
  uint32_t key = static_cast<uint32_t>(host_pc - kGeneratedCodeExecuteBase);
  uint32_t count = unwind_table_count_;
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (key < unwind_table_[mid].BeginAddress) {
      hi = mid;
    } else if (key >= unwind_table_end_address_[mid]) {
      lo = mid + 1;
    } else {
      return &unwind_table_[mid];
    }
  }
  return nullptr;
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
