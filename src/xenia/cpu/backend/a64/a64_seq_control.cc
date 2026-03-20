/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_sequences.h"

#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/hir/instr.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

volatile int anchor_control = 0;

// ============================================================================
// OPCODE_BRANCH
// ============================================================================
struct BRANCH : Sequence<BRANCH, I<OPCODE_BRANCH, VoidOp, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.b(e.GetLabel(i.src1.value->id));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_BRANCH, BRANCH);

// ============================================================================
// OPCODE_BRANCH_TRUE
// ============================================================================
struct BRANCH_TRUE_I8
    : Sequence<BRANCH_TRUE_I8, I<OPCODE_BRANCH_TRUE, VoidOp, I8Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbnz(i.src1, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_TRUE_I16
    : Sequence<BRANCH_TRUE_I16, I<OPCODE_BRANCH_TRUE, VoidOp, I16Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbnz(i.src1, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_TRUE_I32
    : Sequence<BRANCH_TRUE_I32, I<OPCODE_BRANCH_TRUE, VoidOp, I32Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbnz(i.src1, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_TRUE_I64
    : Sequence<BRANCH_TRUE_I64, I<OPCODE_BRANCH_TRUE, VoidOp, I64Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbnz(i.src1, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_TRUE_F32
    : Sequence<BRANCH_TRUE_F32, I<OPCODE_BRANCH_TRUE, VoidOp, F32Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.fmov(e.w0, i.src1);
    e.cbnz(e.w0, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_TRUE_F64
    : Sequence<BRANCH_TRUE_F64, I<OPCODE_BRANCH_TRUE, VoidOp, F64Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.fmov(e.x0, i.src1);
    e.cbnz(e.x0, e.GetLabel(i.src2.value->id));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_BRANCH_TRUE, BRANCH_TRUE_I8, BRANCH_TRUE_I16,
                     BRANCH_TRUE_I32, BRANCH_TRUE_I64, BRANCH_TRUE_F32,
                     BRANCH_TRUE_F64);

// ============================================================================
// OPCODE_BRANCH_FALSE
// ============================================================================
struct BRANCH_FALSE_I8
    : Sequence<BRANCH_FALSE_I8, I<OPCODE_BRANCH_FALSE, VoidOp, I8Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbz(i.src1, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_FALSE_I16
    : Sequence<BRANCH_FALSE_I16,
               I<OPCODE_BRANCH_FALSE, VoidOp, I16Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbz(i.src1, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_FALSE_I32
    : Sequence<BRANCH_FALSE_I32,
               I<OPCODE_BRANCH_FALSE, VoidOp, I32Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbz(i.src1, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_FALSE_I64
    : Sequence<BRANCH_FALSE_I64,
               I<OPCODE_BRANCH_FALSE, VoidOp, I64Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbz(i.src1, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_FALSE_F32
    : Sequence<BRANCH_FALSE_F32,
               I<OPCODE_BRANCH_FALSE, VoidOp, F32Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.fmov(e.w0, i.src1);
    e.cbz(e.w0, e.GetLabel(i.src2.value->id));
  }
};
struct BRANCH_FALSE_F64
    : Sequence<BRANCH_FALSE_F64,
               I<OPCODE_BRANCH_FALSE, VoidOp, F64Op, LabelOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.fmov(e.x0, i.src1);
    e.cbz(e.x0, e.GetLabel(i.src2.value->id));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_BRANCH_FALSE, BRANCH_FALSE_I8, BRANCH_FALSE_I16,
                     BRANCH_FALSE_I32, BRANCH_FALSE_I64, BRANCH_FALSE_F32,
                     BRANCH_FALSE_F64);

// ============================================================================
// OPCODE_RETURN
// ============================================================================
struct RETURN : Sequence<RETURN, I<OPCODE_RETURN, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Jump to epilog (unless this is the last instruction before epilog).
    if (i.instr->next || i.instr->block->next) {
      e.b(e.epilog_label());
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RETURN, RETURN);

// ============================================================================
// OPCODE_SET_RETURN_ADDRESS
// ============================================================================
struct SET_RETURN_ADDRESS
    : Sequence<SET_RETURN_ADDRESS,
               I<OPCODE_SET_RETURN_ADDRESS, VoidOp, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.SetReturnAddress(i.src1.constant());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SET_RETURN_ADDRESS, SET_RETURN_ADDRESS);

// ============================================================================
// OPCODE_DEBUG_BREAK
// ============================================================================
struct DEBUG_BREAK : Sequence<DEBUG_BREAK, I<OPCODE_DEBUG_BREAK, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) { e.DebugBreak(); }
};
EMITTER_OPCODE_TABLE(OPCODE_DEBUG_BREAK, DEBUG_BREAK);

// ============================================================================
// OPCODE_DEBUG_BREAK_TRUE
// ============================================================================
struct DEBUG_BREAK_TRUE_I8
    : Sequence<DEBUG_BREAK_TRUE_I8, I<OPCODE_DEBUG_BREAK_TRUE, VoidOp, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.DebugBreak();
    e.L(skip);
  }
};
struct DEBUG_BREAK_TRUE_I16
    : Sequence<DEBUG_BREAK_TRUE_I16,
               I<OPCODE_DEBUG_BREAK_TRUE, VoidOp, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.DebugBreak();
    e.L(skip);
  }
};
struct DEBUG_BREAK_TRUE_I32
    : Sequence<DEBUG_BREAK_TRUE_I32,
               I<OPCODE_DEBUG_BREAK_TRUE, VoidOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.DebugBreak();
    e.L(skip);
  }
};
struct DEBUG_BREAK_TRUE_I64
    : Sequence<DEBUG_BREAK_TRUE_I64,
               I<OPCODE_DEBUG_BREAK_TRUE, VoidOp, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.DebugBreak();
    e.L(skip);
  }
};
struct DEBUG_BREAK_TRUE_F32
    : Sequence<DEBUG_BREAK_TRUE_F32,
               I<OPCODE_DEBUG_BREAK_TRUE, VoidOp, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.fmov(e.w0, i.src1);
    auto& skip = e.NewCachedLabel();
    e.cbz(e.w0, skip);
    e.DebugBreak();
    e.L(skip);
  }
};
struct DEBUG_BREAK_TRUE_F64
    : Sequence<DEBUG_BREAK_TRUE_F64,
               I<OPCODE_DEBUG_BREAK_TRUE, VoidOp, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.fmov(e.x0, i.src1);
    auto& skip = e.NewCachedLabel();
    e.cbz(e.x0, skip);
    e.DebugBreak();
    e.L(skip);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DEBUG_BREAK_TRUE, DEBUG_BREAK_TRUE_I8,
                     DEBUG_BREAK_TRUE_I16, DEBUG_BREAK_TRUE_I32,
                     DEBUG_BREAK_TRUE_I64, DEBUG_BREAK_TRUE_F32,
                     DEBUG_BREAK_TRUE_F64);

// ============================================================================
// OPCODE_TRAP
// ============================================================================
struct TRAP : Sequence<TRAP, I<OPCODE_TRAP, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.Trap(i.instr->flags);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_TRAP, TRAP);

// ============================================================================
// OPCODE_TRAP_TRUE
// ============================================================================
struct TRAP_TRUE_I8
    : Sequence<TRAP_TRUE_I8, I<OPCODE_TRAP_TRUE, VoidOp, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.Trap(i.instr->flags);
    e.L(skip);
  }
};
struct TRAP_TRUE_I16
    : Sequence<TRAP_TRUE_I16, I<OPCODE_TRAP_TRUE, VoidOp, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.Trap(i.instr->flags);
    e.L(skip);
  }
};
struct TRAP_TRUE_I32
    : Sequence<TRAP_TRUE_I32, I<OPCODE_TRAP_TRUE, VoidOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.Trap(i.instr->flags);
    e.L(skip);
  }
};
struct TRAP_TRUE_I64
    : Sequence<TRAP_TRUE_I64, I<OPCODE_TRAP_TRUE, VoidOp, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.Trap(i.instr->flags);
    e.L(skip);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_TRAP_TRUE, TRAP_TRUE_I8, TRAP_TRUE_I16,
                     TRAP_TRUE_I32, TRAP_TRUE_I64);

// ============================================================================
// OPCODE_RETURN_TRUE
// ============================================================================
struct RETURN_TRUE_I8
    : Sequence<RETURN_TRUE_I8, I<OPCODE_RETURN_TRUE, VoidOp, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbnz(i.src1, e.epilog_label());
  }
};
struct RETURN_TRUE_I16
    : Sequence<RETURN_TRUE_I16, I<OPCODE_RETURN_TRUE, VoidOp, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbnz(i.src1, e.epilog_label());
  }
};
struct RETURN_TRUE_I32
    : Sequence<RETURN_TRUE_I32, I<OPCODE_RETURN_TRUE, VoidOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbnz(i.src1, e.epilog_label());
  }
};
struct RETURN_TRUE_I64
    : Sequence<RETURN_TRUE_I64, I<OPCODE_RETURN_TRUE, VoidOp, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.cbnz(i.src1, e.epilog_label());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RETURN_TRUE, RETURN_TRUE_I8, RETURN_TRUE_I16,
                     RETURN_TRUE_I32, RETURN_TRUE_I64);

// ============================================================================
// OPCODE_CALL
// ============================================================================
struct CALL : Sequence<CALL, I<OPCODE_CALL, VoidOp, SymbolOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src1.value->is_guest());
    e.Call(i.instr, static_cast<GuestFunction*>(i.src1.value));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CALL, CALL);

// ============================================================================
// OPCODE_CALL_TRUE
// ============================================================================
struct CALL_TRUE_I8
    : Sequence<CALL_TRUE_I8, I<OPCODE_CALL_TRUE, VoidOp, I8Op, SymbolOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->is_guest());
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.Call(i.instr, static_cast<GuestFunction*>(i.src2.value));
    e.L(skip);
    e.ForgetFpcrMode();
  }
};
struct CALL_TRUE_I16
    : Sequence<CALL_TRUE_I16, I<OPCODE_CALL_TRUE, VoidOp, I16Op, SymbolOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->is_guest());
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.Call(i.instr, static_cast<GuestFunction*>(i.src2.value));
    e.L(skip);
    e.ForgetFpcrMode();
  }
};
struct CALL_TRUE_I32
    : Sequence<CALL_TRUE_I32, I<OPCODE_CALL_TRUE, VoidOp, I32Op, SymbolOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->is_guest());
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.Call(i.instr, static_cast<GuestFunction*>(i.src2.value));
    e.L(skip);
    e.ForgetFpcrMode();
  }
};
struct CALL_TRUE_I64
    : Sequence<CALL_TRUE_I64, I<OPCODE_CALL_TRUE, VoidOp, I64Op, SymbolOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->is_guest());
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    e.Call(i.instr, static_cast<GuestFunction*>(i.src2.value));
    e.L(skip);
    e.ForgetFpcrMode();
  }
};
struct CALL_TRUE_F32
    : Sequence<CALL_TRUE_F32, I<OPCODE_CALL_TRUE, VoidOp, F32Op, SymbolOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always("CALL_TRUE with float condition is not possible");
  }
};
struct CALL_TRUE_F64
    : Sequence<CALL_TRUE_F64, I<OPCODE_CALL_TRUE, VoidOp, F64Op, SymbolOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always("CALL_TRUE with float condition is not possible");
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CALL_TRUE, CALL_TRUE_I8, CALL_TRUE_I16,
                     CALL_TRUE_I32, CALL_TRUE_I64, CALL_TRUE_F32,
                     CALL_TRUE_F64);

// ============================================================================
// OPCODE_CALL_INDIRECT
// ============================================================================
struct CALL_INDIRECT
    : Sequence<CALL_INDIRECT, I<OPCODE_CALL_INDIRECT, VoidOp, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) [[unlikely]] {
      if (i.src1.constant() == 0) {
        e.nop();
      } else {
        e.mov(e.w16, static_cast<uint32_t>(i.src1.constant()));
        e.CallIndirect(i.instr, 16);
      }
    } else {
      e.CallIndirect(i.instr, i.src1.reg().getIdx());
    }
    e.ForgetFpcrMode();
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CALL_INDIRECT, CALL_INDIRECT);

// ============================================================================
// OPCODE_CALL_INDIRECT_TRUE
// ============================================================================
struct CALL_INDIRECT_TRUE_I8
    : Sequence<CALL_INDIRECT_TRUE_I8,
               I<OPCODE_CALL_INDIRECT_TRUE, VoidOp, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    if (i.src2.is_constant) {
      e.mov(e.w16, static_cast<uint32_t>(i.src2.constant()));
      e.CallIndirect(i.instr, 16);
    } else {
      e.CallIndirect(i.instr, i.src2.reg().getIdx());
    }
    e.L(skip);
  }
};
struct CALL_INDIRECT_TRUE_I16
    : Sequence<CALL_INDIRECT_TRUE_I16,
               I<OPCODE_CALL_INDIRECT_TRUE, VoidOp, I16Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    if (i.src2.is_constant) {
      e.mov(e.w16, static_cast<uint32_t>(i.src2.constant()));
      e.CallIndirect(i.instr, 16);
    } else {
      e.CallIndirect(i.instr, i.src2.reg().getIdx());
    }
    e.L(skip);
  }
};
struct CALL_INDIRECT_TRUE_I32
    : Sequence<CALL_INDIRECT_TRUE_I32,
               I<OPCODE_CALL_INDIRECT_TRUE, VoidOp, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    if (i.src2.is_constant) {
      e.mov(e.w16, static_cast<uint32_t>(i.src2.constant()));
      e.CallIndirect(i.instr, 16);
    } else {
      e.CallIndirect(i.instr, i.src2.reg().getIdx());
    }
    e.L(skip);
  }
};
struct CALL_INDIRECT_TRUE_I64
    : Sequence<CALL_INDIRECT_TRUE_I64,
               I<OPCODE_CALL_INDIRECT_TRUE, VoidOp, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto& skip = e.NewCachedLabel();
    e.cbz(i.src1, skip);
    if (i.src2.is_constant) {
      e.mov(e.w16, static_cast<uint32_t>(i.src2.constant()));
      e.CallIndirect(i.instr, 16);
    } else {
      e.CallIndirect(i.instr, i.src2.reg().getIdx());
    }
    e.L(skip);
  }
};
struct CALL_INDIRECT_TRUE_F32
    : Sequence<CALL_INDIRECT_TRUE_F32,
               I<OPCODE_CALL_INDIRECT_TRUE, VoidOp, F32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always("CALL_INDIRECT_TRUE with float condition is not possible");
  }
};
struct CALL_INDIRECT_TRUE_F64
    : Sequence<CALL_INDIRECT_TRUE_F64,
               I<OPCODE_CALL_INDIRECT_TRUE, VoidOp, F64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always("CALL_INDIRECT_TRUE with float condition is not possible");
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CALL_INDIRECT_TRUE, CALL_INDIRECT_TRUE_I8,
                     CALL_INDIRECT_TRUE_I16, CALL_INDIRECT_TRUE_I32,
                     CALL_INDIRECT_TRUE_I64, CALL_INDIRECT_TRUE_F32,
                     CALL_INDIRECT_TRUE_F64);

// ============================================================================
// OPCODE_CALL_EXTERN
// ============================================================================
struct CALL_EXTERN
    : Sequence<CALL_EXTERN, I<OPCODE_CALL_EXTERN, VoidOp, SymbolOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CallExtern(i.instr, i.src1.value);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CALL_EXTERN, CALL_EXTERN);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
