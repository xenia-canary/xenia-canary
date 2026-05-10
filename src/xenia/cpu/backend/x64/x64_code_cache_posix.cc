/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_code_cache.h"

#include <cstring>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/cpu/backend/x64/x64_stack_layout.h"

// libgcc/libunwind APIs for registering DWARF .eh_frame unwind info.
extern "C" void __register_frame(void*);
extern "C" void __deregister_frame(void*);

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

// Maximum size of DWARF .eh_frame data per function (CIE + FDE + terminator).
static constexpr uint32_t kMaxUnwindInfoSize = 96;

// DWARF register numbers for x86-64.
static constexpr uint8_t kDwarfRegRBX = 3;
static constexpr uint8_t kDwarfRegRBP = 6;
static constexpr uint8_t kDwarfRegRSP = 7;
static constexpr uint8_t kDwarfRegR12 = 12;
static constexpr uint8_t kDwarfRegR13 = 13;
static constexpr uint8_t kDwarfRegR14 = 14;
static constexpr uint8_t kDwarfRegR15 = 15;
static constexpr uint8_t kDwarfRegRA = 16;

// DWARF CFA opcodes.
static constexpr uint8_t kDW_CFA_advance_loc1 = 0x02;
static constexpr uint8_t kDW_CFA_advance_loc2 = 0x03;
static constexpr uint8_t kDW_CFA_def_cfa = 0x0c;
static constexpr uint8_t kDW_CFA_def_cfa_offset = 0x0e;
static constexpr uint8_t kDW_CFA_nop = 0x00;

// DWARF pointer encoding constants.
static constexpr uint8_t kDW_EH_PE_pcrel = 0x10;
static constexpr uint8_t kDW_EH_PE_sdata4 = 0x0b;

static size_t WriteULEB128(uint8_t* p, uint64_t value) {
  size_t count = 0;
  do {
    uint8_t byte = value & 0x7F;
    value >>= 7;
    if (value) {
      byte |= 0x80;
    }
    p[count++] = byte;
  } while (value);
  return count;
}

static size_t WriteSLEB128(uint8_t* p, int64_t value) {
  size_t count = 0;
  bool more = true;
  while (more) {
    uint8_t byte = value & 0x7F;
    value >>= 7;
    if ((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40))) {
      more = false;
    } else {
      byte |= 0x80;
    }
    p[count++] = byte;
  }
  return count;
}

class PosixX64CodeCache : public X64CodeCache {
 public:
  PosixX64CodeCache();
  ~PosixX64CodeCache() override;

  bool Initialize() override;

  void* LookupUnwindInfo(uint64_t host_pc) override { return nullptr; }

 private:
  UnwindReservation RequestUnwindReservation(uint8_t* entry_address) override;
  void PlaceCode(uint32_t guest_address, void* machine_code,
                 const EmitFunctionInfo& func_info, void* code_execute_address,
                 UnwindReservation unwind_reservation) override;

  void InitializeUnwindEntry(uint8_t* unwind_entry_address,
                             void* code_execute_address,
                             const EmitFunctionInfo& func_info);

  // Pointers registered with __register_frame, for cleanup.
  std::vector<void*> registered_frames_;
  // Current number of unwind table entries.
  uint32_t unwind_table_count_ = 0;
};

std::unique_ptr<X64CodeCache> X64CodeCache::Create() {
  return std::make_unique<PosixX64CodeCache>();
}

PosixX64CodeCache::PosixX64CodeCache() = default;

PosixX64CodeCache::~PosixX64CodeCache() {
  for (auto frame : registered_frames_) {
    __deregister_frame(frame);
  }
}

bool PosixX64CodeCache::Initialize() {
  if (!X64CodeCache::Initialize()) {
    return false;
  }
  registered_frames_.reserve(kMaximumFunctionCount);
  return true;
}

X64CodeCache::UnwindReservation PosixX64CodeCache::RequestUnwindReservation(
    uint8_t* entry_address) {
#if defined(NDEBUG)
  if (unwind_table_count_ >= kMaximumFunctionCount) {
    xe::FatalError(
        "Unwind table count exceeded maximum! Please report this to "
        "Xenia developers");
  }
#else
  assert_false(unwind_table_count_ >= kMaximumFunctionCount);
#endif
  UnwindReservation unwind_reservation;
  unwind_reservation.data_size = xe::round_up(kMaxUnwindInfoSize, 16);
  unwind_reservation.table_slot = unwind_table_count_++;
  unwind_reservation.entry_address = entry_address;
  return unwind_reservation;
}

void PosixX64CodeCache::PlaceCode(uint32_t guest_address, void* machine_code,
                                  const EmitFunctionInfo& func_info,
                                  void* code_execute_address,
                                  UnwindReservation unwind_reservation) {
  // Write the DWARF .eh_frame data into the reserved unwind space.
  InitializeUnwindEntry(unwind_reservation.entry_address, code_execute_address,
                        func_info);

  // Register with the runtime unwinder using the execute-side address.
  // The execute mapping is readable (kExecuteReadOnly = PROT_EXEC|PROT_READ),
  // so the unwinder can read the .eh_frame data at runtime.
  void* unwind_execute_address = unwind_reservation.entry_address -
                                 generated_code_write_base_ +
                                 generated_code_execute_base_;
  __register_frame(unwind_execute_address);
  registered_frames_.push_back(unwind_execute_address);
}

void PosixX64CodeCache::InitializeUnwindEntry(
    uint8_t* unwind_entry_address, void* code_execute_address,
    const EmitFunctionInfo& func_info) {
  // Compute execute-side base address of the unwind buffer.
  // We write via the write mapping but pc-relative offsets must be relative
  // to the execute mapping (which is what __register_frame sees).
  uint8_t* unwind_execute_base = unwind_entry_address -
                                 generated_code_write_base_ +
                                 generated_code_execute_base_;

  uint8_t* p = unwind_entry_address;
  uint8_t* cie_start = p;

  // === CIE (Common Information Entry) ===
  uint8_t* cie_length_ptr = p;
  p += 4;  // placeholder for length

  uint8_t* cie_content_start = p;

  // CIE ID = 0 (distinguishes CIE from FDE in .eh_frame format).
  *reinterpret_cast<uint32_t*>(p) = 0;
  p += 4;

  // Version = 1.
  *p++ = 1;

  // Augmentation string "zR" - indicates augmentation data with FDE encoding.
  *p++ = 'z';
  *p++ = 'R';
  *p++ = '\0';

  // Code alignment factor = 1.
  p += WriteULEB128(p, 1);

  // Data alignment factor = -8.
  p += WriteSLEB128(p, -8);

  // Return address register column = 16 (x86-64 RA).
  p += WriteULEB128(p, kDwarfRegRA);

  // Augmentation data length = 1 (just the FDE encoding byte).
  p += WriteULEB128(p, 1);

  // FDE pointer encoding: pc-relative, signed 32-bit.
  *p++ = kDW_EH_PE_pcrel | kDW_EH_PE_sdata4;

  // Initial instructions:
  // DW_CFA_def_cfa RSP, 8 — at function entry, CFA = RSP + 8.
  *p++ = kDW_CFA_def_cfa;
  p += WriteULEB128(p, kDwarfRegRSP);
  p += WriteULEB128(p, 8);

  // DW_CFA_offset RA, 1 — return address at CFA - 8 (factored: 1 * 8).
  *p++ = 0x80 | kDwarfRegRA;
  p += WriteULEB128(p, 1);

  // Pad CIE to pointer-size (8-byte) alignment.
  size_t cie_content_len = static_cast<size_t>(p - cie_content_start);
  size_t cie_padded_len = xe::round_up(cie_content_len, sizeof(void*));
  while (p < cie_content_start + cie_padded_len) {
    *p++ = kDW_CFA_nop;
  }

  // Write CIE length (excludes the length field itself).
  *reinterpret_cast<uint32_t*>(cie_length_ptr) =
      static_cast<uint32_t>(p - cie_content_start);

  // === FDE (Frame Description Entry) ===
  uint8_t* fde_length_ptr = p;
  p += 4;  // placeholder for length

  uint8_t* fde_content_start = p;

  // CIE pointer: offset from this field back to the start of the CIE.
  *reinterpret_cast<uint32_t*>(p) = static_cast<uint32_t>(p - cie_start);
  p += 4;

  // PC begin: pc-relative offset to the start of the function code.
  // Computed relative to the execute-side address of this field.
  uint8_t* pc_begin_execute_addr =
      unwind_execute_base + (p - unwind_entry_address);
  *reinterpret_cast<int32_t*>(p) =
      static_cast<int32_t>(reinterpret_cast<intptr_t>(code_execute_address) -
                           reinterpret_cast<intptr_t>(pc_begin_execute_addr));
  p += 4;

  // PC range: size of the function code.
  *reinterpret_cast<uint32_t*>(p) =
      static_cast<uint32_t>(func_info.code_size.total);
  p += 4;

  // Augmentation data length = 0 (no LSDA pointer).
  p += WriteULEB128(p, 0);

  // FDE instructions: describe how the stack frame changes during the prolog.
  if (func_info.stack_size > 0) {
    // Advance location to the instruction after the stack allocation.
    size_t alloc_offset = func_info.prolog_stack_alloc_offset;
    assert_true(alloc_offset > 0);
    if (alloc_offset < 64) {
      *p++ = 0x40 | static_cast<uint8_t>(alloc_offset);
    } else if (alloc_offset < 256) {
      *p++ = kDW_CFA_advance_loc1;
      *p++ = static_cast<uint8_t>(alloc_offset);
    } else {
      *p++ = kDW_CFA_advance_loc2;
      *reinterpret_cast<uint16_t*>(p) = static_cast<uint16_t>(alloc_offset);
      p += 2;
    }

    // DW_CFA_def_cfa_offset: CFA = RSP + 8 + stack_size after stack alloc.
    *p++ = kDW_CFA_def_cfa_offset;
    p += WriteULEB128(p, 8 + func_info.stack_size);

    // For thunk functions, encode callee-saved register save locations.
    // The thunk saves non-volatile registers at known offsets from RSP.
    if (func_info.stack_size == StackLayout::THUNK_STACK_SIZE) {
      size_t cfa = 8 + func_info.stack_size;  // 272

      // RBX at rsp+0x18 → CFA-248, factored offset = 31
      *p++ = 0x80 | kDwarfRegRBX;
      p += WriteULEB128(p, (cfa - 0x18) / 8);

      // RBP at rsp+0x20 → CFA-240, factored offset = 30
      *p++ = 0x80 | kDwarfRegRBP;
      p += WriteULEB128(p, (cfa - 0x20) / 8);

      // R12 at rsp+0x40 → CFA-208, factored offset = 26
      *p++ = 0x80 | kDwarfRegR12;
      p += WriteULEB128(p, (cfa - 0x40) / 8);

      // R13 at rsp+0x48 → CFA-200, factored offset = 25
      *p++ = 0x80 | kDwarfRegR13;
      p += WriteULEB128(p, (cfa - 0x48) / 8);

      // R14 at rsp+0x50 → CFA-192, factored offset = 24
      *p++ = 0x80 | kDwarfRegR14;
      p += WriteULEB128(p, (cfa - 0x50) / 8);

      // R15 at rsp+0x58 → CFA-184, factored offset = 23
      *p++ = 0x80 | kDwarfRegR15;
      p += WriteULEB128(p, (cfa - 0x58) / 8);
    }
  }

  // Pad FDE to pointer-size (8-byte) alignment.
  size_t fde_content_len = static_cast<size_t>(p - fde_content_start);
  size_t fde_padded_len = xe::round_up(fde_content_len, sizeof(void*));
  while (p < fde_content_start + fde_padded_len) {
    *p++ = kDW_CFA_nop;
  }

  // Write FDE length.
  *reinterpret_cast<uint32_t*>(fde_length_ptr) =
      static_cast<uint32_t>(p - fde_content_start);

  // === Terminator (zero-length entry) ===
  *reinterpret_cast<uint32_t*>(p) = 0;
  p += 4;

  assert_true(static_cast<size_t>(p - unwind_entry_address) <=
              kMaxUnwindInfoSize);
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
