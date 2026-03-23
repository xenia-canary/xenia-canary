/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_sequences.h"

#include <cstring>

#include "xenia/base/math.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_seq_util.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/hir/instr.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

volatile int anchor_vector = 0;

// ============================================================================
// OPCODE_SPLAT
// ============================================================================
struct SPLAT_I8 : Sequence<SPLAT_I8, I<OPCODE_SPLAT, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.movi(VReg(i.dest.reg().getIdx()).b16,
             static_cast<uint8_t>(i.src1.constant()));
    } else {
      e.dup(VReg(i.dest.reg().getIdx()).b16, i.src1);
    }
  }
};
struct SPLAT_I16 : Sequence<SPLAT_I16, I<OPCODE_SPLAT, V128Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));
      e.dup(VReg(i.dest.reg().getIdx()).h8, e.w0);
    } else {
      e.dup(VReg(i.dest.reg().getIdx()).h8, i.src1);
    }
  }
};
struct SPLAT_I32 : Sequence<SPLAT_I32, I<OPCODE_SPLAT, V128Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      uint32_t val = static_cast<uint32_t>(i.src1.constant());
      // Use movz/movn via mov(xreg, uint64) for full 32-bit range.
      e.mov(e.x0, static_cast<uint64_t>(val));
      e.dup(VReg(i.dest.reg().getIdx()).s4, e.w0);
    } else {
      e.dup(VReg(i.dest.reg().getIdx()).s4, i.src1);
    }
  }
};
struct SPLAT_F32 : Sequence<SPLAT_F32, I<OPCODE_SPLAT, V128Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.dup(VReg(i.dest.reg().getIdx()).s4, e.w0);
    } else {
      int src_idx = i.src1.reg().getIdx();
      e.dup(VReg(i.dest.reg().getIdx()).s4, VReg(src_idx).s4[0]);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SPLAT, SPLAT_I8, SPLAT_I16, SPLAT_I32, SPLAT_F32);

// ============================================================================
// OPCODE_INSERT
// ============================================================================
struct INSERT_I8
    : Sequence<INSERT_I8, I<OPCODE_INSERT, V128Op, V128Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    int dest_idx = i.dest.reg().getIdx();
    if (!i.src1.is_constant && i.src1.reg().getIdx() != dest_idx) {
      e.orr(VReg(dest_idx).b16, VReg(i.src1.reg().getIdx()).b16,
            VReg(i.src1.reg().getIdx()).b16);
    } else if (i.src1.is_constant) {
      LoadV128Const(e, dest_idx, i.src1.constant());
    }
    uint8_t idx = VEC128_B(i.src2.constant());
    if (i.src3.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src3.constant() & 0xFF));
      e.ins(VReg(dest_idx).b16[idx], e.w0);
    } else {
      e.ins(VReg(dest_idx).b16[idx], i.src3);
    }
  }
};
struct INSERT_I16
    : Sequence<INSERT_I16, I<OPCODE_INSERT, V128Op, V128Op, I8Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    int dest_idx = i.dest.reg().getIdx();
    if (!i.src1.is_constant && i.src1.reg().getIdx() != dest_idx) {
      e.orr(VReg(dest_idx).b16, VReg(i.src1.reg().getIdx()).b16,
            VReg(i.src1.reg().getIdx()).b16);
    } else if (i.src1.is_constant) {
      LoadV128Const(e, dest_idx, i.src1.constant());
    }
    uint8_t idx = VEC128_W(i.src2.constant());
    if (i.src3.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src3.constant() & 0xFFFF));
      e.ins(VReg(dest_idx).h8[idx], e.w0);
    } else {
      e.ins(VReg(dest_idx).h8[idx], i.src3);
    }
  }
};
struct INSERT_I32
    : Sequence<INSERT_I32, I<OPCODE_INSERT, V128Op, V128Op, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    int dest_idx = i.dest.reg().getIdx();
    if (!i.src1.is_constant && i.src1.reg().getIdx() != dest_idx) {
      e.orr(VReg(dest_idx).b16, VReg(i.src1.reg().getIdx()).b16,
            VReg(i.src1.reg().getIdx()).b16);
    } else if (i.src1.is_constant) {
      LoadV128Const(e, dest_idx, i.src1.constant());
    }
    uint8_t idx = VEC128_D(i.src2.constant());
    if (i.src3.is_constant) {
      e.mov(e.w0,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
      e.ins(VReg(dest_idx).s4[idx], e.w0);
    } else {
      e.ins(VReg(dest_idx).s4[idx], i.src3);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_INSERT, INSERT_I8, INSERT_I16, INSERT_I32);

// ============================================================================
// OPCODE_EXTRACT
// ============================================================================
struct EXTRACT_I8
    : Sequence<EXTRACT_I8, I<OPCODE_EXTRACT, I8Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int src_idx = SrcVReg(e, i.src1, 0);
    if (i.src2.is_constant) {
      e.umov(i.dest, VReg(src_idx).b16[VEC128_B(i.src2.constant())]);
    } else {
      // Dynamic: XOR index with 3, use as TBL index.
      e.mov(e.w0, static_cast<uint64_t>(0x03));
      e.eor(e.w0, e.w0, i.src2);
      e.and_(e.w0, e.w0, 0x0F);
      e.fmov(e.s1, e.w0);
      e.tbl(VReg(1).b16, VReg(src_idx).b16, 1, VReg(1).b16);
      e.umov(i.dest, VReg(1).b16[0]);
    }
  }
};
struct EXTRACT_I16
    : Sequence<EXTRACT_I16, I<OPCODE_EXTRACT, I16Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int src_idx = SrcVReg(e, i.src1, 0);
    if (i.src2.is_constant) {
      e.umov(i.dest, VReg(src_idx).h8[VEC128_W(i.src2.constant())]);
    } else {
      // Dynamic: XOR with 1, extract.
      e.mov(e.w0, static_cast<uint64_t>(0x01));
      e.eor(e.w0, e.w0, i.src2);
      e.and_(e.w0, e.w0, 0x07);
      // Compute byte offset = index * 2.
      e.lsl(e.w1, e.w0, 1);
      e.add(e.w0, e.w1, 1);
      // Build 2-byte TBL control: {w1, w0} = {lo_byte_idx, hi_byte_idx}.
      e.fmov(e.s1, e.w1);
      e.ins(VReg(1).b16[1], e.w0);
      e.tbl(VReg(1).b16, VReg(src_idx).b16, 1, VReg(1).b16);
      e.umov(i.dest, VReg(1).h8[0]);
    }
  }
};
struct EXTRACT_I32
    : Sequence<EXTRACT_I32, I<OPCODE_EXTRACT, I32Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int src_idx = SrcVReg(e, i.src1, 0);
    if (i.src2.is_constant) {
      uint8_t idx = VEC128_D(i.src2.constant());
      if (idx == 0) {
        e.umov(i.dest, VReg(src_idx).s4[0]);
      } else {
        e.umov(i.dest, VReg(src_idx).s4[idx]);
      }
    } else {
      // Dynamic: use TBL with computed control.
      e.and_(e.w0, i.src2, 0x03);
      e.lsl(e.w0, e.w0, 2);
      // Build 4-byte control: {w0, w0+1, w0+2, w0+3}.
      e.add(e.w1, e.w0, 1);
      e.add(e.w2, e.w0, 2);
      e.add(e.w3, e.w0, 3);
      e.fmov(e.s1, e.w0);
      e.ins(VReg(1).b16[1], e.w1);
      e.ins(VReg(1).b16[2], e.w2);
      e.ins(VReg(1).b16[3], e.w3);
      e.tbl(VReg(1).b16, VReg(src_idx).b16, 1, VReg(1).b16);
      e.umov(i.dest, VReg(1).s4[0]);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_EXTRACT, EXTRACT_I8, EXTRACT_I16, EXTRACT_I32);

// ============================================================================
// OPCODE_VECTOR_CONVERT_I2F
// ============================================================================
struct VECTOR_CONVERT_I2F
    : Sequence<VECTOR_CONVERT_I2F,
               I<OPCODE_VECTOR_CONVERT_I2F, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      int s = SrcVReg(e, i.src1, 0);
      int d = i.dest.reg().getIdx();
      if (i.instr->flags & ARITHMETIC_UNSIGNED) {
        // ARM64 ucvtf does a single-step unsigned int->float conversion,
        // avoiding the double-rounding issue that x64 has to work around.
        e.ucvtf(VReg(d).s4, VReg(s).s4);
      } else {
        e.scvtf(VReg(d).s4, VReg(s).s4);
      }
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_CONVERT_I2F, VECTOR_CONVERT_I2F);

// ============================================================================
// OPCODE_VECTOR_CONVERT_F2I
// ============================================================================
struct VECTOR_CONVERT_F2I
    : Sequence<VECTOR_CONVERT_F2I,
               I<OPCODE_VECTOR_CONVERT_F2I, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      int s = SrcVReg(e, i.src1, 0);
      int d = i.dest.reg().getIdx();
      if (i.instr->flags & ARITHMETIC_UNSIGNED) {
        // ARM64 fcvtzu: NaN->0, negative->0, overflow->UINT_MAX.
        e.fcvtzu(VReg(d).s4, VReg(s).s4);
      } else {
        // ARM64 fcvtzs: NaN->0, overflow saturates to INT_MIN/INT_MAX.
        e.fcvtzs(VReg(d).s4, VReg(s).s4);
      }
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_CONVERT_F2I, VECTOR_CONVERT_F2I);

// ============================================================================
// OPCODE_VECTOR_ADD
// ============================================================================
struct VECTOR_ADD
    : Sequence<VECTOR_ADD, I<OPCODE_VECTOR_ADD, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const TypeName part_type = static_cast<TypeName>(i.instr->flags & 0xFF);
    if (part_type == FLOAT32_TYPE) {
      EmitVmxFpBinOp_V128(e, i.dest.reg().getIdx(), i.src1, i.src2,
                          VmxFpBinOp::Add);
      return;
    }
    const uint32_t arith = i.instr->flags >> 8;
    bool is_unsigned = !!(arith & hir::ARITHMETIC_UNSIGNED);
    bool saturate = !!(arith & hir::ARITHMETIC_SATURATE);
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    switch (part_type) {
      case INT8_TYPE:
        if (saturate) {
          if (is_unsigned)
            e.uqadd(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
          else
            e.sqadd(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        } else {
          e.add(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        }
        break;
      case INT16_TYPE:
        if (saturate) {
          if (is_unsigned)
            e.uqadd(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
          else
            e.sqadd(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        } else {
          e.add(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        }
        break;
      case INT32_TYPE:
        if (saturate) {
          if (is_unsigned)
            e.uqadd(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
          else
            e.sqadd(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        } else {
          e.add(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        }
        break;
      default:
        assert_unhandled_case(part_type);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_ADD, VECTOR_ADD);

// ============================================================================
// OPCODE_VECTOR_SUB
// ============================================================================
struct VECTOR_SUB
    : Sequence<VECTOR_SUB, I<OPCODE_VECTOR_SUB, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const TypeName part_type = static_cast<TypeName>(i.instr->flags & 0xFF);
    if (part_type == FLOAT32_TYPE) {
      EmitVmxFpBinOp_V128(e, i.dest.reg().getIdx(), i.src1, i.src2,
                          VmxFpBinOp::Sub);
      return;
    }
    const uint32_t arith = i.instr->flags >> 8;
    bool is_unsigned = !!(arith & hir::ARITHMETIC_UNSIGNED);
    bool saturate = !!(arith & hir::ARITHMETIC_SATURATE);
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    switch (part_type) {
      case INT8_TYPE:
        if (saturate) {
          if (is_unsigned)
            e.uqsub(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
          else
            e.sqsub(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        } else {
          e.sub(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        }
        break;
      case INT16_TYPE:
        if (saturate) {
          if (is_unsigned)
            e.uqsub(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
          else
            e.sqsub(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        } else {
          e.sub(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        }
        break;
      case INT32_TYPE:
        if (saturate) {
          if (is_unsigned)
            e.uqsub(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
          else
            e.sqsub(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        } else {
          e.sub(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        }
        break;
      default:
        assert_unhandled_case(part_type);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_SUB, VECTOR_SUB);

// ============================================================================
// OPCODE_VECTOR_MAX
// ============================================================================
struct VECTOR_MAX
    : Sequence<VECTOR_MAX, I<OPCODE_VECTOR_MAX, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t part_type = i.instr->flags >> 8;
    if (part_type == FLOAT32_TYPE) {
      EmitVmxFpMaxMin(e, i, /*is_min=*/false);
      return;
    }
    bool is_unsigned = !!(i.instr->flags & hir::ARITHMETIC_UNSIGNED);
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    switch (part_type) {
      case INT8_TYPE:
        if (is_unsigned)
          e.umax(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        else
          e.smax(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        break;
      case INT16_TYPE:
        if (is_unsigned)
          e.umax(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        else
          e.smax(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        break;
      case INT32_TYPE:
        if (is_unsigned)
          e.umax(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        else
          e.smax(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      default:
        assert_unhandled_case(part_type);
        break;
    }
  }

  template <typename T>
  static void EmitVmxFpMaxMin(A64Emitter& e, const T& i, bool is_min) {
    EmitWithVmxFpcr(e, [&] {
      int s1, s2;
      PrepareVmxFpSources(e, i.src1, i.src2, s1, s2);
      if (is_min) {
        e.fmin(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
      } else {
        e.fmax(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
      }
      FixupVmxMaxMinNan(e);
      FlushDenormals_V128(e, 2, 0, 1);
      e.mov(VReg(i.dest.reg().getIdx()).b16, VReg(2).b16);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_MAX, VECTOR_MAX);

// ============================================================================
// OPCODE_VECTOR_MIN
// ============================================================================
struct VECTOR_MIN
    : Sequence<VECTOR_MIN, I<OPCODE_VECTOR_MIN, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t part_type = i.instr->flags >> 8;
    if (part_type == FLOAT32_TYPE) {
      VECTOR_MAX::EmitVmxFpMaxMin(e, i, /*is_min=*/true);
      return;
    }
    bool is_unsigned = !!(i.instr->flags & hir::ARITHMETIC_UNSIGNED);
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    switch (part_type) {
      case INT8_TYPE:
        if (is_unsigned)
          e.umin(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        else
          e.smin(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        break;
      case INT16_TYPE:
        if (is_unsigned)
          e.umin(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        else
          e.smin(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        break;
      case INT32_TYPE:
        if (is_unsigned)
          e.umin(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        else
          e.smin(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      default:
        assert_unhandled_case(part_type);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_MIN, VECTOR_MIN);

// ============================================================================
// OPCODE_VECTOR_COMPARE_EQ
// ============================================================================
struct VECTOR_COMPARE_EQ_V128
    : Sequence<VECTOR_COMPARE_EQ_V128,
               I<OPCODE_VECTOR_COMPARE_EQ, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    switch (i.instr->flags) {
      case INT8_TYPE:
        e.cmeq(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        break;
      case INT16_TYPE:
        e.cmeq(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        break;
      case INT32_TYPE:
        e.cmeq(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case FLOAT32_TYPE:
        EmitWithVmxFpcr(e,
                        [&] { e.fcmeq(VReg(d).s4, VReg(s1).s4, VReg(s2).s4); });
        break;
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_EQ, VECTOR_COMPARE_EQ_V128);

// ============================================================================
// OPCODE_VECTOR_COMPARE_SGT
// ============================================================================
struct VECTOR_COMPARE_SGT_V128
    : Sequence<VECTOR_COMPARE_SGT_V128,
               I<OPCODE_VECTOR_COMPARE_SGT, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    switch (i.instr->flags) {
      case INT8_TYPE:
        e.cmgt(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        break;
      case INT16_TYPE:
        e.cmgt(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        break;
      case INT32_TYPE:
        e.cmgt(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case FLOAT32_TYPE:
        EmitWithVmxFpcr(e,
                        [&] { e.fcmgt(VReg(d).s4, VReg(s1).s4, VReg(s2).s4); });
        break;
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_SGT, VECTOR_COMPARE_SGT_V128);

// ============================================================================
// OPCODE_VECTOR_COMPARE_SGE
// ============================================================================
struct VECTOR_COMPARE_SGE_V128
    : Sequence<VECTOR_COMPARE_SGE_V128,
               I<OPCODE_VECTOR_COMPARE_SGE, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    switch (i.instr->flags) {
      case INT8_TYPE:
        e.cmge(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        break;
      case INT16_TYPE:
        e.cmge(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        break;
      case INT32_TYPE:
        e.cmge(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case FLOAT32_TYPE:
        EmitWithVmxFpcr(e,
                        [&] { e.fcmge(VReg(d).s4, VReg(s1).s4, VReg(s2).s4); });
        break;
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_SGE, VECTOR_COMPARE_SGE_V128);

// ============================================================================
// OPCODE_VECTOR_COMPARE_UGT
// ============================================================================
struct VECTOR_COMPARE_UGT_V128
    : Sequence<VECTOR_COMPARE_UGT_V128,
               I<OPCODE_VECTOR_COMPARE_UGT, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    switch (i.instr->flags) {
      case INT8_TYPE:
        e.cmhi(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        break;
      case INT16_TYPE:
        e.cmhi(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        break;
      case INT32_TYPE:
        e.cmhi(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case FLOAT32_TYPE:
        // Unsigned FP compare = ordered GT.
        EmitWithVmxFpcr(e,
                        [&] { e.fcmgt(VReg(d).s4, VReg(s1).s4, VReg(s2).s4); });
        break;
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_UGT, VECTOR_COMPARE_UGT_V128);

// ============================================================================
// OPCODE_VECTOR_COMPARE_UGE
// ============================================================================
struct VECTOR_COMPARE_UGE_V128
    : Sequence<VECTOR_COMPARE_UGE_V128,
               I<OPCODE_VECTOR_COMPARE_UGE, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    switch (i.instr->flags) {
      case INT8_TYPE:
        e.cmhs(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        break;
      case INT16_TYPE:
        e.cmhs(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        break;
      case INT32_TYPE:
        e.cmhs(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case FLOAT32_TYPE:
        EmitWithVmxFpcr(e,
                        [&] { e.fcmge(VReg(d).s4, VReg(s1).s4, VReg(s2).s4); });
        break;
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_UGE, VECTOR_COMPARE_UGE_V128);

// ============================================================================
// OPCODE_VECTOR_SHL
// ============================================================================
struct VECTOR_SHL_V128
    : Sequence<VECTOR_SHL_V128, I<OPCODE_VECTOR_SHL, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    // Mask shift amounts to element width, then ushl.
    switch (i.instr->flags) {
      case INT8_TYPE: {
        e.movi(VReg(2).b16, 0x07);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.ushl(VReg(d).b16, VReg(s1).b16, VReg(2).b16);
        break;
      }
      case INT16_TYPE: {
        e.movi(VReg(2).h8, 0x0F);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.ushl(VReg(d).h8, VReg(s1).h8, VReg(2).h8);
        break;
      }
      case INT32_TYPE: {
        e.movi(VReg(2).s4, 0x1F);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.ushl(VReg(d).s4, VReg(s1).s4, VReg(2).s4);
        break;
      }
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_SHL, VECTOR_SHL_V128);

// ============================================================================
// OPCODE_VECTOR_SHR
// ============================================================================
struct VECTOR_SHR_V128
    : Sequence<VECTOR_SHR_V128, I<OPCODE_VECTOR_SHR, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    // Mask, negate, then ushl (negative shift = right shift).
    switch (i.instr->flags) {
      case INT8_TYPE: {
        e.movi(VReg(2).b16, 0x07);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.neg(VReg(2).b16, VReg(2).b16);
        e.ushl(VReg(d).b16, VReg(s1).b16, VReg(2).b16);
        break;
      }
      case INT16_TYPE: {
        e.movi(VReg(2).h8, 0x0F);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.neg(VReg(2).h8, VReg(2).h8);
        e.ushl(VReg(d).h8, VReg(s1).h8, VReg(2).h8);
        break;
      }
      case INT32_TYPE: {
        e.movi(VReg(2).s4, 0x1F);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.neg(VReg(2).s4, VReg(2).s4);
        e.ushl(VReg(d).s4, VReg(s1).s4, VReg(2).s4);
        break;
      }
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_SHR, VECTOR_SHR_V128);

// ============================================================================
// OPCODE_VECTOR_SHA (arithmetic right shift)
// ============================================================================
struct VECTOR_SHA_V128
    : Sequence<VECTOR_SHA_V128, I<OPCODE_VECTOR_SHA, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    // Mask, negate, then sshl (signed shift with negative = arith right).
    switch (i.instr->flags) {
      case INT8_TYPE: {
        e.movi(VReg(2).b16, 0x07);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.neg(VReg(2).b16, VReg(2).b16);
        e.sshl(VReg(d).b16, VReg(s1).b16, VReg(2).b16);
        break;
      }
      case INT16_TYPE: {
        e.movi(VReg(2).h8, 0x0F);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.neg(VReg(2).h8, VReg(2).h8);
        e.sshl(VReg(d).h8, VReg(s1).h8, VReg(2).h8);
        break;
      }
      case INT32_TYPE: {
        e.movi(VReg(2).s4, 0x1F);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.neg(VReg(2).s4, VReg(2).s4);
        e.sshl(VReg(d).s4, VReg(s1).s4, VReg(2).s4);
        break;
      }
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_SHA, VECTOR_SHA_V128);

// ============================================================================
// OPCODE_VECTOR_ROTATE_LEFT
// ============================================================================
struct VECTOR_ROTATE_LEFT_V128
    : Sequence<VECTOR_ROTATE_LEFT_V128,
               I<OPCODE_VECTOR_ROTATE_LEFT, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    // rotate = (src << amt) | (src >> (width - amt))
    switch (i.instr->flags) {
      case INT8_TYPE: {
        e.movi(VReg(2).b16, 0x07);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        // Left shift.
        e.ushl(VReg(3).b16, VReg(s1).b16, VReg(2).b16);
        // Right shift = negate amount.
        e.movi(VReg(0).b16, 8);
        e.sub(VReg(0).b16, VReg(0).b16, VReg(2).b16);
        e.neg(VReg(0).b16, VReg(0).b16);
        e.ushl(VReg(0).b16, VReg(s1).b16, VReg(0).b16);
        e.orr(VReg(d).b16, VReg(3).b16, VReg(0).b16);
        break;
      }
      case INT16_TYPE: {
        e.movi(VReg(2).h8, 0x0F);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.ushl(VReg(3).h8, VReg(s1).h8, VReg(2).h8);
        e.movi(VReg(0).h8, 16);
        e.sub(VReg(0).h8, VReg(0).h8, VReg(2).h8);
        e.neg(VReg(0).h8, VReg(0).h8);
        e.ushl(VReg(0).h8, VReg(s1).h8, VReg(0).h8);
        e.orr(VReg(d).b16, VReg(3).b16, VReg(0).b16);
        break;
      }
      case INT32_TYPE: {
        e.movi(VReg(2).s4, 0x1F);
        e.and_(VReg(2).b16, VReg(s2).b16, VReg(2).b16);
        e.ushl(VReg(3).s4, VReg(s1).s4, VReg(2).s4);
        e.movi(VReg(0).s4, 32);
        e.sub(VReg(0).s4, VReg(0).s4, VReg(2).s4);
        e.neg(VReg(0).s4, VReg(0).s4);
        e.ushl(VReg(0).s4, VReg(s1).s4, VReg(0).s4);
        e.orr(VReg(d).b16, VReg(3).b16, VReg(0).b16);
        break;
      }
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_ROTATE_LEFT, VECTOR_ROTATE_LEFT_V128);

// ============================================================================
// OPCODE_VECTOR_AVERAGE
// ============================================================================
struct VECTOR_AVERAGE
    : Sequence<VECTOR_AVERAGE,
               I<OPCODE_VECTOR_AVERAGE, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const TypeName part_type = static_cast<TypeName>(i.instr->flags & 0xFF);
    const uint32_t arith = i.instr->flags >> 8;
    bool is_unsigned = !!(arith & hir::ARITHMETIC_UNSIGNED);
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    // ARM64 has native rounding halving add: (a + b + 1) >> 1.
    switch (part_type) {
      case INT8_TYPE:
        if (is_unsigned)
          e.urhadd(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        else
          e.srhadd(VReg(d).b16, VReg(s1).b16, VReg(s2).b16);
        break;
      case INT16_TYPE:
        if (is_unsigned)
          e.urhadd(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        else
          e.srhadd(VReg(d).h8, VReg(s1).h8, VReg(s2).h8);
        break;
      case INT32_TYPE:
        if (is_unsigned)
          e.urhadd(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        else
          e.srhadd(VReg(d).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      default:
        assert_unhandled_case(part_type);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_AVERAGE, VECTOR_AVERAGE);

// ============================================================================
// OPCODE_VECTOR_DENORMFLUSH
// ============================================================================
struct VECTOR_DENORMFLUSH
    : Sequence<VECTOR_DENORMFLUSH,
               I<OPCODE_VECTOR_DENORMFLUSH, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();
    // Extract exponent bits; if exponent == 0 and mantissa != 0, it's denormal.
    // Replace with signed zero (keep sign bit).
    // Mask: exponent = bits 30:23 of each float.
    // If (val & 0x7F800000) == 0 then it's zero or denormal.
    // For denormals (mantissa != 0 but exponent == 0), replace with sign | 0.
    e.mov(e.w0, static_cast<uint64_t>(0x7F800000u));
    e.dup(VReg(2).s4, e.w0);
    e.and_(VReg(0).b16, VReg(s).b16, VReg(2).b16);
    // v0 = exponent bits. Compare with zero.
    e.cmeq(VReg(0).s4, VReg(0).s4, 0);
    // v0 = all-ones where exponent is zero (denormal or zero).
    // Keep sign bits of denormals.
    e.movi(VReg(1).s4, 0x80, LSL, 24);
    e.and_(VReg(1).b16, VReg(s).b16, VReg(1).b16);
    // v1 = sign bits only.
    // Select: where exponent is zero -> sign bits, else -> original.
    e.bsl(VReg(0).b16, VReg(1).b16, VReg(s).b16);
    if (d != 0) {
      e.orr(VReg(d).b16, VReg(0).b16, VReg(0).b16);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_DENORMFLUSH, VECTOR_DENORMFLUSH);

// ============================================================================
// OPCODE_PERMUTE
// ============================================================================
struct PERMUTE_I32
    : Sequence<PERMUTE_I32, I<OPCODE_PERMUTE, V128Op, I32Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src1.is_constant);
    uint32_t control = i.src1.constant();
    int s2 = SrcVReg(e, i.src2, 0);
    int s3 = SrcVReg(e, i.src3, 1);
    int d = i.dest.reg().getIdx();
    // Build TBL control from the I32 permute control word.
    // Each byte of control selects: bits [1:0] = which dword, bit [2] = src2 vs
    // src3. PPC word i = vec128_t.u32[i] = NEON element s[i] (direct mapping).
    uint8_t tbl_ctrl[16];
    for (int idx = 0; idx < 4; idx++) {
      uint8_t sel = (control >> (idx * 8)) & 0xFF;
      uint8_t src_dword = sel & 0x3;
      bool from_src3 = (sel >> 2) & 1;
      uint8_t base = from_src3 ? 16 : 0;
      for (int b = 0; b < 4; b++) {
        tbl_ctrl[idx * 4 + b] = base + src_dword * 4 + b;
      }
    }
    // Ensure src2 in v0, src3 in v1 (consecutive for TBL).
    if (s2 != 0) {
      e.orr(VReg(0).b16, VReg(s2).b16, VReg(s2).b16);
    }
    if (s3 != 1) {
      e.orr(VReg(1).b16, VReg(s3).b16, VReg(s3).b16);
    }
    // Load TBL control vector.
    vec128_t ctrl_vec;
    std::memcpy(&ctrl_vec, tbl_ctrl, 16);
    LoadV128Const(e, 2, ctrl_vec);
    e.tbl(VReg(d).b16, VReg(0).b16, 2, VReg(2).b16);
  }
};
struct PERMUTE_V128
    : Sequence<PERMUTE_V128,
               I<OPCODE_PERMUTE, V128Op, V128Op, V128Op, V128Op>> {
  static void EmitByInt8(A64Emitter& e, const EmitArgType& i) {
    int d = i.dest.reg().getIdx();
    // Copy src2 to v0, src3 to v1 (consecutive for 2-register TBL).
    if (i.src2.is_constant) {
      LoadV128Const(e, 0, i.src2.constant());
    } else if (i.src2.reg().getIdx() != 0) {
      e.orr(VReg(0).b16, VReg(i.src2.reg().getIdx()).b16,
            VReg(i.src2.reg().getIdx()).b16);
    }
    if (i.src3.is_constant) {
      LoadV128Const(e, 1, i.src3.constant());
    } else if (i.src3.reg().getIdx() != 1) {
      e.orr(VReg(1).b16, VReg(i.src3.reg().getIdx()).b16,
            VReg(i.src3.reg().getIdx()).b16);
    }
    // Load control vector into v2, XOR each byte with 3 for endian swap.
    int ctrl;
    if (i.src1.is_constant) {
      LoadV128Const(e, 2, i.src1.constant());
      ctrl = 2;
    } else {
      ctrl = i.src1.reg().getIdx();
      if (ctrl == 0 || ctrl == 1) {
        // Control conflicts with table registers, copy to v2.
        e.orr(VReg(2).b16, VReg(ctrl).b16, VReg(ctrl).b16);
        ctrl = 2;
      }
    }
    // XOR control bytes with 0x03 to remap PPC byte indices to LE,
    // then mask to 5 bits (0-31) so TBL indices stay in range.
    e.movi(VReg(3).b16, 0x03);
    e.eor(VReg(3).b16, VReg(ctrl).b16, VReg(3).b16);
    e.movi(VReg(2).b16, 0x1F);
    e.and_(VReg(3).b16, VReg(3).b16, VReg(2).b16);
    // TBL with 2-register table {v0, v1}.
    e.tbl(VReg(d).b16, VReg(0).b16, 2, VReg(3).b16);
  }

  static void EmitByInt16(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src1.is_constant);
    int d = i.dest.reg().getIdx();
    // Convert halfword element indices to byte-level TBL control.
    // PPC halfword index H maps to NEON u16 index (H&7)^1 (halfword swap
    // within 32-bit words). For src3 (indices >= 8), add 16 byte offset.
    vec128_t ctrl = i.src1.constant();
    vec128_t tbl_ctrl = {};
    for (int k = 0; k < 8; k++) {
      uint16_t h = ctrl.u16[k] & 0xF;
      uint32_t base = (h >= 8) ? 16 : 0;
      uint32_t neon_hw = (h & 7) ^ 1;
      tbl_ctrl.u8[2 * k] = static_cast<uint8_t>(base + 2 * neon_hw);
      tbl_ctrl.u8[2 * k + 1] = static_cast<uint8_t>(base + 2 * neon_hw + 1);
    }
    // Copy src2 to v0, src3 to v1 (consecutive for 2-register TBL).
    if (i.src2.is_constant) {
      LoadV128Const(e, 0, i.src2.constant());
    } else if (i.src2.reg().getIdx() != 0) {
      e.orr(VReg(0).b16, VReg(i.src2.reg().getIdx()).b16,
            VReg(i.src2.reg().getIdx()).b16);
    }
    if (i.src3.is_constant) {
      LoadV128Const(e, 1, i.src3.constant());
    } else if (i.src3.reg().getIdx() != 1) {
      e.orr(VReg(1).b16, VReg(i.src3.reg().getIdx()).b16,
            VReg(i.src3.reg().getIdx()).b16);
    }
    // Load precomputed byte-level TBL control.
    LoadV128Const(e, 2, tbl_ctrl);
    e.tbl(VReg(d).b16, VReg(0).b16, 2, VReg(2).b16);
  }

  static void Emit(A64Emitter& e, const EmitArgType& i) {
    switch (i.instr->flags) {
      case INT8_TYPE:
        EmitByInt8(e, i);
        break;
      case INT16_TYPE:
        EmitByInt16(e, i);
        break;
      case INT32_TYPE:
        // INT32 permutes go through PERMUTE_I32 sequence.
        assert_always();
        break;
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_PERMUTE, PERMUTE_I32, PERMUTE_V128);

// ============================================================================
// OPCODE_SWIZZLE
// ============================================================================
struct SWIZZLE
    : Sequence<SWIZZLE, I<OPCODE_SWIZZLE, V128Op, V128Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto element_type = i.instr->flags;
    if (element_type == INT32_TYPE || element_type == FLOAT32_TYPE) {
      uint8_t swizzle_mask = static_cast<uint8_t>(i.src2.value);
      int s = SrcVReg(e, i.src1, 0);
      int d = i.dest.reg().getIdx();
      // Build TBL control for dword swizzle.
      // swizzle_mask bits [1:0]=X(PPC word 0), [3:2]=Y, [5:4]=Z, [7:6]=W.
      // PPC word i = NEON element s[i] (direct mapping).
      uint8_t ctrl[16];
      for (int idx = 0; idx < 4; idx++) {
        uint8_t src_dw = (swizzle_mask >> (idx * 2)) & 0x3;
        for (int b = 0; b < 4; b++) {
          ctrl[idx * 4 + b] = src_dw * 4 + b;
        }
      }
      vec128_t ctrl_vec;
      std::memcpy(&ctrl_vec, ctrl, 16);
      LoadV128Const(e, 2, ctrl_vec);
      e.tbl(VReg(d).b16, VReg(s).b16, 1, VReg(2).b16);
    } else {
      e.DebugBreak();
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SWIZZLE, SWIZZLE);

// ============================================================================
// OPCODE_LOAD_VECTOR_SHL
// ============================================================================
struct LOAD_VECTOR_SHL_I8
    : Sequence<LOAD_VECTOR_SHL_I8, I<OPCODE_LOAD_VECTOR_SHL, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int d = i.dest.reg().getIdx();
    // Build base pattern in PPC byte order (byte-swapped within 32-bit words):
    // PPC indices {0,1,2,...,15} stored as vec128b would give:
    //   {3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12}
    e.mov(e.x0, static_cast<uint64_t>(0x0405060700010203ull));
    e.mov(e.x1, static_cast<uint64_t>(0x0C0D0E0F08090A0Bull));
    e.stp(e.x0, e.x1,
          ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH)));
    e.ldr(QReg(d), ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH)));
    // Add shift amount (splatted).
    if (i.src1.is_constant) {
      if (i.src1.constant() != 0) {
        e.movi(VReg(0).b16, static_cast<uint8_t>(i.src1.constant()));
        e.add(VReg(d).b16, VReg(d).b16, VReg(0).b16);
      }
    } else {
      e.dup(VReg(0).b16, i.src1);
      e.add(VReg(d).b16, VReg(d).b16, VReg(0).b16);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_VECTOR_SHL, LOAD_VECTOR_SHL_I8);

// ============================================================================
// OPCODE_LOAD_VECTOR_SHR
// ============================================================================
struct LOAD_VECTOR_SHR_I8
    : Sequence<LOAD_VECTOR_SHR_I8, I<OPCODE_LOAD_VECTOR_SHR, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int d = i.dest.reg().getIdx();
    // Build base pattern in PPC byte order (byte-swapped within 32-bit words):
    // PPC indices {16,17,...,31} stored as vec128b would give:
    //   {19,18,17,16, 23,22,21,20, 27,26,25,24, 31,30,29,28}
    e.mov(e.x0, static_cast<uint64_t>(0x1415161710111213ull));
    e.mov(e.x1, static_cast<uint64_t>(0x1C1D1E1F18191A1Bull));
    e.stp(e.x0, e.x1,
          ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH)));
    e.ldr(QReg(d), ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH)));
    // Subtract shift amount (splatted).
    if (i.src1.is_constant) {
      if (i.src1.constant() != 0) {
        e.movi(VReg(0).b16, static_cast<uint8_t>(i.src1.constant()));
        e.sub(VReg(d).b16, VReg(d).b16, VReg(0).b16);
      }
    } else {
      e.dup(VReg(0).b16, i.src1);
      e.sub(VReg(d).b16, VReg(d).b16, VReg(0).b16);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_VECTOR_SHR, LOAD_VECTOR_SHR_I8);

// ============================================================================
// PACK/UNPACK C helper functions (called via CallNativeSafe)
// ============================================================================

// PACK FLOAT16_2: pack first 2 floats to Xenos half-float format.
// Args: x0=PPCContext*, x1=pointer to vec128_t (in-place).
static void EmulatePACK_FLOAT16_2(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  vec128_t result = {};
  for (int i = 0; i < 2; i++) {
    result.u16[7 - i] = float_to_xenos_half(data->f32[i]);
  }
  *data = result;
}

// PACK FLOAT16_4: pack all 4 floats to Xenos half-float (round to even).
static void EmulatePACK_FLOAT16_4(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  vec128_t result = {};
  for (int idx = 0; idx < 4; ++idx) {
    result.u16[7 - (idx ^ 2)] =
        float_to_xenos_half(data->f32[idx], false, true);
  }
  *data = result;
}

// PACK UINT_2101010: XYZ 10-bit signed saturated, W 2-bit unsigned saturated.
static void EmulatePACK_UINT_2101010(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  // Clamp and extract integer values from magic float encoding.
  // Input floats are in 3.0+val*2^-22 format.
  auto clamp_extract = [](uint32_t bits, int32_t min_val, int32_t max_val,
                          uint32_t mask) -> uint32_t {
    // Reinterpret as float for clamping.
    float f;
    memcpy(&f, &bits, 4);
    float fmin, fmax;
    uint32_t umin = 0x40400000u + static_cast<uint32_t>(min_val);
    uint32_t umax = 0x40400000u + static_cast<uint32_t>(max_val);
    memcpy(&fmin, &umin, 4);
    memcpy(&fmax, &umax, 4);
    if (std::isnan(f) || f < fmin) f = fmin;
    if (f > fmax) f = fmax;
    uint32_t fbits;
    memcpy(&fbits, &f, 4);
    return fbits & mask;
  };
  uint32_t x = clamp_extract(data->u32[0], -511, 511, 0x3FF);
  uint32_t y = clamp_extract(data->u32[1], -511, 511, 0x3FF);
  uint32_t z = clamp_extract(data->u32[2], -511, 511, 0x3FF);
  uint32_t w = clamp_extract(data->u32[3], 0, 3, 0x3);
  vec128_t result = {};
  result.u32[3] = x | (y << 10) | (z << 20) | (w << 30);
  *data = result;
}

// PACK ULONG_4202020: XYZ 20-bit signed saturated, W 4-bit unsigned saturated.
static void EmulatePACK_ULONG_4202020(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  auto clamp_extract = [](uint32_t bits, int32_t min_val, int32_t max_val,
                          uint32_t mask) -> uint32_t {
    float f;
    memcpy(&f, &bits, 4);
    float fmin, fmax;
    uint32_t umin = 0x40400000u + static_cast<uint32_t>(min_val);
    uint32_t umax = 0x40400000u + static_cast<uint32_t>(max_val);
    memcpy(&fmin, &umin, 4);
    memcpy(&fmax, &umax, 4);
    if (std::isnan(f) || f < fmin) f = fmin;
    if (f > fmax) f = fmax;
    uint32_t fbits;
    memcpy(&fbits, &f, 4);
    return fbits & mask;
  };
  uint32_t x = clamp_extract(data->u32[0], -524287, 524287, 0xFFFFF);
  uint32_t y = clamp_extract(data->u32[1], -524287, 524287, 0xFFFFF);
  uint32_t z = clamp_extract(data->u32[2], -524287, 524287, 0xFFFFF);
  uint32_t w = clamp_extract(data->u32[3], 0, 15, 0xF);
  // Pack: 64-bit result in lanes 2-3
  uint64_t packed =
      static_cast<uint64_t>(x) | (static_cast<uint64_t>(y) << 20) |
      (static_cast<uint64_t>(z) << 40) | (static_cast<uint64_t>(w) << 60);
  vec128_t result = {};
  result.u32[2] = static_cast<uint32_t>(packed >> 32);
  result.u32[3] = static_cast<uint32_t>(packed);
  *data = result;
}

// UNPACK FLOAT16_2: convert 2 Xenos half-floats to float.
static void EmulateUNPACK_FLOAT16_2(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  vec128_t src = *data;
  vec128_t result = {};
  for (int i = 0; i < 2; i++) {
    result.f32[i] = xenos_half_to_float(src.u16[VEC128_W(6 + i)]);
  }
  result.f32[2] = 0.0f;
  result.f32[3] = 1.0f;
  *data = result;
}

// UNPACK FLOAT16_4: convert 4 Xenos half-floats to float.
static void EmulateUNPACK_FLOAT16_4(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  vec128_t src = *data;
  vec128_t result = {};
  for (int idx = 0; idx < 4; ++idx) {
    result.f32[idx] = xenos_half_to_float(src.u16[VEC128_W(4 + idx)]);
  }
  *data = result;
}

// UNPACK SHORT_2: unpack 2 signed shorts to magic float format (3.0+val*2^-22).
static void EmulateUNPACK_SHORT_2(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  // Source: packed value in lane 3.
  // Upper halfword = X, lower halfword = Y.
  int16_t x_val = static_cast<int16_t>(data->u16[7]);
  int16_t y_val = static_cast<int16_t>(data->u16[6]);
  vec128_t result = {};
  // Sign-extend to 32-bit and add magic constant.
  // Magic constant {3.0f, 3.0f, 0.0f, 1.0f} for SHORT2/SHORT4 unpack.
  result.u32[0] =
      0x40400000u + static_cast<uint32_t>(static_cast<int32_t>(x_val));
  result.u32[1] =
      0x40400000u + static_cast<uint32_t>(static_cast<int32_t>(y_val));
  result.u32[2] = 0;
  result.u32[3] = 0x3F800000u;
  // Overflow check: if result == 0x403F8000, replace with QNaN.
  for (int j = 0; j < 4; j++) {
    if (result.u32[j] == 0x403F8000u) {
      result.u32[j] = 0x7FC00000u;
    }
  }
  *data = result;
}

// UNPACK SHORT_4: unpack 4 signed shorts to magic float format.
static void EmulateUNPACK_SHORT_4(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  // Source: lanes 2-3 contain 2 packed values, each with 2 shorts.
  // Lane 3: upper half = X, lower half = Y
  // Lane 2: upper half = Z, lower half = W
  int16_t x_val = static_cast<int16_t>(data->u16[7]);
  int16_t y_val = static_cast<int16_t>(data->u16[6]);
  int16_t z_val = static_cast<int16_t>(data->u16[5]);
  int16_t w_val = static_cast<int16_t>(data->u16[4]);
  vec128_t result = {};
  result.u32[0] =
      0x40400000u + static_cast<uint32_t>(static_cast<int32_t>(x_val));
  result.u32[1] =
      0x40400000u + static_cast<uint32_t>(static_cast<int32_t>(y_val));
  result.u32[2] =
      0x40400000u + static_cast<uint32_t>(static_cast<int32_t>(z_val));
  result.u32[3] =
      0x40400000u + static_cast<uint32_t>(static_cast<int32_t>(w_val));
  for (int j = 0; j < 4; j++) {
    if (result.u32[j] == 0x403F8000u) {
      result.u32[j] = 0x7FC00000u;
    }
  }
  *data = result;
}

// UNPACK UINT_2101010: unpack 10-10-10-2 to magic float format.
static void EmulateUNPACK_UINT_2101010(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  uint32_t packed = data->u32[3];
  // Extract components.
  int32_t x = static_cast<int32_t>(packed & 0x3FF);
  int32_t y = static_cast<int32_t>((packed >> 10) & 0x3FF);
  int32_t z = static_cast<int32_t>((packed >> 20) & 0x3FF);
  uint32_t w = (packed >> 30) & 0x3;
  // Sign-extend XYZ (10-bit signed).
  if (x & 0x200) x |= ~0x3FF;
  if (y & 0x200) y |= ~0x3FF;
  if (z & 0x200) z |= ~0x3FF;
  // Build magic float: 3.0 + val * 2^-22 for XYZ, 1.0 + val for W.
  vec128_t result = {};
  result.u32[0] = 0x40400000u + static_cast<uint32_t>(x);
  result.u32[1] = 0x40400000u + static_cast<uint32_t>(y);
  result.u32[2] = 0x40400000u + static_cast<uint32_t>(z);
  result.u32[3] = 0x3F800000u + w;
  // Overflow check.
  uint32_t overflow_xyz = 0x403FFE00u;
  for (int j = 0; j < 3; j++) {
    if (result.u32[j] == overflow_xyz) {
      result.u32[j] = 0x7FC00000u;
    }
  }
  *data = result;
}

// UNPACK ULONG_4202020: unpack 20-20-20-4 to magic float format.
static void EmulateUNPACK_ULONG_4202020(void* /*ctx*/, void* vdata) {
  auto* data = reinterpret_cast<vec128_t*>(vdata);
  // 64-bit packed value in lanes 2-3.
  uint64_t packed = (static_cast<uint64_t>(data->u32[2]) << 32) |
                    static_cast<uint64_t>(data->u32[3]);
  int32_t x = static_cast<int32_t>(packed & 0xFFFFF);
  int32_t y = static_cast<int32_t>((packed >> 20) & 0xFFFFF);
  int32_t z = static_cast<int32_t>((packed >> 40) & 0xFFFFF);
  uint32_t w = static_cast<uint32_t>((packed >> 60) & 0xF);
  // Sign-extend XYZ (20-bit signed).
  if (x & 0x80000) x |= ~0xFFFFF;
  if (y & 0x80000) y |= ~0xFFFFF;
  if (z & 0x80000) z |= ~0xFFFFF;
  vec128_t result = {};
  result.u32[0] = 0x40400000u + static_cast<uint32_t>(x);
  result.u32[1] = 0x40400000u + static_cast<uint32_t>(y);
  result.u32[2] = 0x40400000u + static_cast<uint32_t>(z);
  result.u32[3] = 0x3F800000u + w;
  uint32_t overflow_xyz = 0x40380000u;
  for (int j = 0; j < 3; j++) {
    if (result.u32[j] == overflow_xyz) {
      result.u32[j] = 0x7FC00000u;
    }
  }
  *data = result;
}

// LVL/LVR/STVL/STVR C helper functions.
// Args: x0=PPCContext*, x1=host_addr(uint64_t), x2=data_ptr(void*)

static void EmulateLVL(void* /*ctx*/, uint64_t host_addr, void* result_ptr) {
  uint32_t offset = static_cast<uint32_t>(host_addr) & 0xF;
  const uint8_t* aligned =
      reinterpret_cast<const uint8_t*>(host_addr & ~0xFull);
  uint8_t mem[16];
  memcpy(mem, aligned, 16);
  // Shuffle: base = {3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12} (bswap within
  // lanes) ctrl[i] = base[i] + offset; if > 15, output 0.
  static const uint8_t base[16] = {3,  2,  1, 0, 7,  6,  5,  4,
                                   11, 10, 9, 8, 15, 14, 13, 12};
  uint8_t result[16] = {};
  for (int i = 0; i < 16; i++) {
    int idx = base[i] + offset;
    if (idx <= 15) {
      result[i] = mem[idx];
    }
  }
  memcpy(result_ptr, result, 16);
}

static void EmulateLVR(void* /*ctx*/, uint64_t host_addr, void* result_ptr) {
  uint32_t offset = static_cast<uint32_t>(host_addr) & 0xF;
  uint8_t result[16] = {};
  if (offset == 0) {
    memcpy(result_ptr, result, 16);
    return;
  }
  const uint8_t* aligned =
      reinterpret_cast<const uint8_t*>(host_addr & ~0xFull);
  uint8_t mem[16];
  memcpy(mem, aligned, 16);
  // Same base shuffle as LVL, but keep only indices > 15 (using idx & 0xF).
  static const uint8_t base[16] = {3,  2,  1, 0, 7,  6,  5,  4,
                                   11, 10, 9, 8, 15, 14, 13, 12};
  for (int i = 0; i < 16; i++) {
    int idx = base[i] + offset;
    if (idx > 15) {
      result[i] = mem[idx & 0xF];
    }
  }
  memcpy(result_ptr, result, 16);
}

static void EmulateSTVL(void* /*ctx*/, uint64_t host_addr, void* src_data) {
  uint32_t offset = static_cast<uint32_t>(host_addr) & 0xF;
  uint8_t* aligned = reinterpret_cast<uint8_t*>(host_addr & ~0xFull);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(src_data);
  uint8_t mem[16];
  memcpy(mem, aligned, 16);
  // Write bytes offset..15: mem[i] = src[bswap_lane_idx(i - offset)]
  for (int i = static_cast<int>(offset); i < 16; i++) {
    mem[i] = src[bswap_lane_idx(i - static_cast<int>(offset))];
  }
  memcpy(aligned, mem, 16);
}

static void EmulateSTVR(void* /*ctx*/, uint64_t host_addr, void* src_data) {
  uint32_t offset = static_cast<uint32_t>(host_addr) & 0xF;
  if (offset == 0) return;
  uint8_t* aligned = reinterpret_cast<uint8_t*>(host_addr & ~0xFull);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(src_data);
  uint8_t mem[16];
  memcpy(mem, aligned, 16);
  // Write bytes 0..(offset-1) from the right part of the source.
  for (int i = 0; i < static_cast<int>(offset); i++) {
    // Use pshufb-compatible index: (i - offset) ^ 0x83, take bits 3:0
    int src_idx =
        (static_cast<uint8_t>(i - static_cast<int>(offset)) ^ 0x83) & 0x0F;
    mem[i] = src[src_idx];
  }
  memcpy(aligned, mem, 16);
}

// ============================================================================
// OPCODE_PACK
// ============================================================================
struct PACK : Sequence<PACK, I<OPCODE_PACK, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    switch (i.instr->flags & hir::PACK_TYPE_MODE) {
      case hir::PACK_TYPE_D3DCOLOR:
        EmitD3DCOLOR(e, i);
        break;
      case hir::PACK_TYPE_FLOAT16_2:
        EmitFLOAT16_2(e, i);
        break;
      case hir::PACK_TYPE_FLOAT16_4:
        EmitFLOAT16_4(e, i);
        break;
      case hir::PACK_TYPE_SHORT_2:
        EmitSHORT_2(e, i);
        break;
      case hir::PACK_TYPE_SHORT_4:
        EmitSHORT_4(e, i);
        break;
      case hir::PACK_TYPE_UINT_2101010:
        EmitUINT_2101010(e, i);
        break;
      case hir::PACK_TYPE_ULONG_4202020:
        EmitULONG_4202020(e, i);
        break;
      case hir::PACK_TYPE_8_IN_16:
        Emit8_IN_16(e, i, i.instr->flags);
        break;
      case hir::PACK_TYPE_16_IN_32:
        Emit16_IN_32(e, i, i.instr->flags);
        break;
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
  static void EmitD3DCOLOR(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->IsConstantZero());
    int s = SrcVReg(e, i.src1, 2);
    int d = i.dest.reg().getIdx();
    // Clamp to [3.0f, 3.0f + 255*2^-22].
    // fmaxnm/fminnm: NaN operand returns the non-NaN value (pack NaN as zero).
    e.mov(e.w0, 0x40400000u);  // 3.0f
    e.dup(VReg(0).s4, e.w0);
    e.fmaxnm(VReg(d).s4, VReg(s).s4, VReg(0).s4);
    e.mov(e.w0, 0x404000FFu);  // 3.0f + 255*2^-22
    e.dup(VReg(0).s4, e.w0);
    e.fminnm(VReg(d).s4, VReg(d).s4, VReg(0).s4);
    // TBL: extract low byte from each lane, reorder RGBA->ARGB in lane 3.
    // Control: bytes 0-11=0xFF (->0), bytes 12-15={0x08,0x04,0x00,0x0C}
    vec128_t ctrl;
    ctrl.low = 0xFFFFFFFF'FFFFFFFFull;
    ctrl.high = 0x0C000408'FFFFFFFFull;
    LoadV128Const(e, 0, ctrl);
    e.tbl(VReg(d).b16, VReg(d).b16, 1, VReg(0).b16);
  }
  static void EmitSHORT_2(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->IsConstantZero());
    int s = SrcVReg(e, i.src1, 2);
    int d = i.dest.reg().getIdx();
    // Clamp to [PackSHORT_Min, PackSHORT_Max].
    e.mov(e.w0, 0x403F8001u);
    e.dup(VReg(0).s4, e.w0);
    e.fmaxnm(VReg(d).s4, VReg(s).s4, VReg(0).s4);
    e.mov(e.w0, 0x40407FFFu);
    e.dup(VReg(0).s4, e.w0);
    e.fminnm(VReg(d).s4, VReg(d).s4, VReg(0).s4);
    // TBL: extract low 2 bytes from lanes 0,1 -> pack into lane 3.
    // TBL ctrl for PACK_SHORT_2: bytes 12-15={0x04,0x05,0x00,0x01}, rest=0xFF
    vec128_t ctrl;
    ctrl.low = 0xFFFFFFFF'FFFFFFFFull;
    ctrl.high = 0x01000504'FFFFFFFFull;
    LoadV128Const(e, 0, ctrl);
    e.tbl(VReg(d).b16, VReg(d).b16, 1, VReg(0).b16);
  }
  static void EmitSHORT_4(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->IsConstantZero());
    int s = SrcVReg(e, i.src1, 2);
    int d = i.dest.reg().getIdx();
    e.mov(e.w0, 0x403F8001u);
    e.dup(VReg(0).s4, e.w0);
    e.fmaxnm(VReg(d).s4, VReg(s).s4, VReg(0).s4);
    e.mov(e.w0, 0x40407FFFu);
    e.dup(VReg(0).s4, e.w0);
    e.fminnm(VReg(d).s4, VReg(d).s4, VReg(0).s4);
    // TBL ctrl for PACK_SHORT_4: bytes 8-11={0x04,0x05,0x00,0x01},
    // 12-15={0x0C,0x0D,0x08,0x09}
    vec128_t ctrl;
    ctrl.low = 0xFFFFFFFF'FFFFFFFFull;
    ctrl.high = 0x09080D0C'01000504ull;
    LoadV128Const(e, 0, ctrl);
    e.tbl(VReg(d).b16, VReg(d).b16, 1, VReg(0).b16);
  }
  static void EmitFLOAT16_2(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->IsConstantZero());
    if (i.src1.is_constant) {
      vec128_t result = {};
      for (int j = 0; j < 2; j++) {
        result.u16[7 - j] = float_to_xenos_half(i.src1.constant().f32[j]);
      }
      LoadV128Const(e, i.dest.reg().getIdx(), result);
      return;
    }
    int s = i.src1.reg().getIdx();
    int d = i.dest.reg().getIdx();
    e.str(QReg(s),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.add(e.x1, e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulatePACK_FLOAT16_2));
    e.ldr(QReg(d),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
  }
  static void EmitFLOAT16_4(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      vec128_t result = {};
      for (int idx = 0; idx < 4; ++idx) {
        result.u16[7 - (idx ^ 2)] =
            float_to_xenos_half(i.src1.constant().f32[idx], false, true);
      }
      LoadV128Const(e, i.dest.reg().getIdx(), result);
      return;
    }
    int s = i.src1.reg().getIdx();
    int d = i.dest.reg().getIdx();
    e.str(QReg(s),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.add(e.x1, e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulatePACK_FLOAT16_4));
    e.ldr(QReg(d),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
  }
  static void EmitUINT_2101010(A64Emitter& e, const EmitArgType& i) {
    int s = SrcVReg(e, i.src1, 2);
    int d = i.dest.reg().getIdx();
    e.str(QReg(s),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.add(e.x1, e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulatePACK_UINT_2101010));
    e.ldr(QReg(d),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
  }
  static void EmitULONG_4202020(A64Emitter& e, const EmitArgType& i) {
    int s = SrcVReg(e, i.src1, 2);
    int d = i.dest.reg().getIdx();
    e.str(QReg(s),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.add(e.x1, e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulatePACK_ULONG_4202020));
    e.ldr(QReg(d),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
  }
  // Keep existing 8_IN_16 and 16_IN_32 implementations unchanged:
  static void Emit8_IN_16(A64Emitter& e, const EmitArgType& i, uint32_t flags) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    bool saturate = hir::IsPackOutSaturate(flags);
    bool in_unsigned = hir::IsPackInUnsigned(flags);
    bool out_unsigned = hir::IsPackOutUnsigned(flags);
    // PPC: src1(VA) -> bytes 0-7, src2(VB) -> bytes 8-15.
    // NEON xtn narrows to low half (bytes 0-7), xtn2 to high half (bytes 8-15).
    // So: xtn from src1 (VA -> low), xtn2 from src2 (VB -> high).
    if (saturate) {
      if (in_unsigned && out_unsigned) {
        // unsigned -> unsigned saturate
        e.uqxtn(VReg(2).b8, VReg(s1).h8);
        e.uqxtn2(VReg(2).b16, VReg(s2).h8);
      } else if (!in_unsigned && out_unsigned) {
        // signed -> unsigned saturate (vpkshus)
        e.sqxtun(VReg(2).b8, VReg(s1).h8);
        e.sqxtun2(VReg(2).b16, VReg(s2).h8);
      } else if (!in_unsigned && !out_unsigned) {
        // signed -> signed saturate
        e.sqxtn(VReg(2).b8, VReg(s1).h8);
        e.sqxtn2(VReg(2).b16, VReg(s2).h8);
      } else {
        // unsigned -> signed saturate (shouldn't happen)
        e.uqxtn(VReg(2).b8, VReg(s1).h8);
        e.uqxtn2(VReg(2).b16, VReg(s2).h8);
      }
    } else {
      // Modulo (truncate)
      e.xtn(VReg(2).b8, VReg(s1).h8);
      e.xtn2(VReg(2).b16, VReg(s2).h8);
    }
    // Swap halfwords within 32-bit words to fix PPC big-endian layout.
    // After narrowing, NEON h[0]=PPC_h1, h[1]=PPC_h0 within each word.
    // rev32.h swaps them to correct PPC order.
    e.rev32(VReg(2).h8, VReg(2).h8);
    if (d != 2) {
      e.mov(VReg(d).b16, VReg(2).b16);
    }
  }
  static void Emit16_IN_32(A64Emitter& e, const EmitArgType& i,
                           uint32_t flags) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    int d = i.dest.reg().getIdx();
    bool saturate = hir::IsPackOutSaturate(flags);
    bool in_unsigned = hir::IsPackInUnsigned(flags);
    bool out_unsigned = hir::IsPackOutUnsigned(flags);
    // PPC: src1(VA) -> halfwords 0-3, src2(VB) -> halfwords 4-7.
    // NEON xtn narrows to low half, xtn2 to high half.
    // So: xtn from src1 (VA -> low), xtn2 from src2 (VB -> high).
    if (saturate) {
      if (in_unsigned && out_unsigned) {
        e.uqxtn(VReg(2).h4, VReg(s1).s4);
        e.uqxtn2(VReg(2).h8, VReg(s2).s4);
      } else if (!in_unsigned && out_unsigned) {
        e.sqxtun(VReg(2).h4, VReg(s1).s4);
        e.sqxtun2(VReg(2).h8, VReg(s2).s4);
      } else if (!in_unsigned && !out_unsigned) {
        e.sqxtn(VReg(2).h4, VReg(s1).s4);
        e.sqxtn2(VReg(2).h8, VReg(s2).s4);
      } else {
        e.uqxtn(VReg(2).h4, VReg(s1).s4);
        e.uqxtn2(VReg(2).h8, VReg(s2).s4);
      }
    } else {
      e.xtn(VReg(2).h4, VReg(s1).s4);
      e.xtn2(VReg(2).h8, VReg(s2).s4);
    }
    // Swap halfwords within 32-bit words to fix PPC big-endian layout.
    e.rev32(VReg(2).h8, VReg(2).h8);
    if (d != 2) {
      e.mov(VReg(d).b16, VReg(2).b16);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_PACK, PACK);

// ============================================================================
// OPCODE_UNPACK
// ============================================================================
struct UNPACK : Sequence<UNPACK, I<OPCODE_UNPACK, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    switch (i.instr->flags & hir::PACK_TYPE_MODE) {
      case hir::PACK_TYPE_D3DCOLOR:
        EmitD3DCOLOR(e, i);
        break;
      case hir::PACK_TYPE_FLOAT16_2:
        EmitFLOAT16_2(e, i);
        break;
      case hir::PACK_TYPE_FLOAT16_4:
        EmitFLOAT16_4(e, i);
        break;
      case hir::PACK_TYPE_SHORT_2:
        EmitSHORT_2(e, i);
        break;
      case hir::PACK_TYPE_SHORT_4:
        EmitSHORT_4(e, i);
        break;
      case hir::PACK_TYPE_UINT_2101010:
        EmitUINT_2101010(e, i);
        break;
      case hir::PACK_TYPE_ULONG_4202020:
        EmitULONG_4202020(e, i);
        break;
      case hir::PACK_TYPE_8_IN_16:
        Emit8_IN_16(e, i, i.instr->flags);
        break;
      case hir::PACK_TYPE_16_IN_32:
        Emit16_IN_32(e, i, i.instr->flags);
        break;
      default:
        assert_unhandled_case(i.instr->flags);
        break;
    }
  }
  static void EmitD3DCOLOR(A64Emitter& e, const EmitArgType& i) {
    int s = SrcVReg(e, i.src1, 2);
    int d = i.dest.reg().getIdx();
    if (i.src1.is_constant && i.src1.value->IsConstantZero()) {
      // Zero -> 1.0f in all lanes.
      e.mov(e.w0, 0x3F800000u);
      e.dup(VReg(d).s4, e.w0);
      return;
    }
    // TBL: extract bytes from packed D3DCOLOR -> one byte per lane.
    // TBL ctrl for UNPACK_D3DCOLOR: lane0<-byte14(R), lane1<-byte13(G),
    // lane2<-byte12(B), lane3<-byte15(A)
    vec128_t ctrl;
    ctrl.low = 0xFFFFFF0D'FFFFFF0Eull;  // lane0: byte0=0x0E, lane1: byte4=0x0D
    ctrl.high =
        0xFFFFFF0F'FFFFFF0Cull;  // lane2: byte8=0x0C, lane3: byte12=0x0F
    LoadV128Const(e, 1, ctrl);
    e.tbl(VReg(d).b16, VReg(s).b16, 1, VReg(1).b16);
    // OR with 1.0f (0x3F800000) to form the magic float.
    e.mov(e.w0, 0x3F800000u);
    e.dup(VReg(0).s4, e.w0);
    e.orr(VReg(d).b16, VReg(d).b16, VReg(0).b16);
  }
  static void EmitCallHelper(A64Emitter& e, const EmitArgType& i, void* fn) {
    int s = SrcVReg(e, i.src1, 2);
    int d = i.dest.reg().getIdx();
    e.str(QReg(s),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.add(e.x1, e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH));
    e.CallNativeSafe(fn);
    e.ldr(QReg(d),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
  }
  static void EmitFLOAT16_2(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      vec128_t result = {};
      for (int j = 0; j < 2; j++) {
        result.f32[j] =
            xenos_half_to_float(i.src1.constant().u16[VEC128_W(6 + j)]);
      }
      result.f32[2] = 0.0f;
      result.f32[3] = 1.0f;
      LoadV128Const(e, i.dest.reg().getIdx(), result);
      return;
    }
    EmitCallHelper(e, i, reinterpret_cast<void*>(EmulateUNPACK_FLOAT16_2));
  }
  static void EmitFLOAT16_4(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      vec128_t result = {};
      for (int idx = 0; idx < 4; ++idx) {
        result.f32[idx] =
            xenos_half_to_float(i.src1.constant().u16[VEC128_W(4 + idx)]);
      }
      LoadV128Const(e, i.dest.reg().getIdx(), result);
      return;
    }
    EmitCallHelper(e, i, reinterpret_cast<void*>(EmulateUNPACK_FLOAT16_4));
  }
  static void EmitSHORT_2(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src1.value->IsConstantZero()) {
      // Return {3.0, 3.0, 0.0, 1.0}
      vec128_t c;
      c.f32[0] = 3.0f;
      c.f32[1] = 3.0f;
      c.f32[2] = 0.0f;
      c.f32[3] = 1.0f;
      LoadV128Const(e, i.dest.reg().getIdx(), c);
      return;
    }
    EmitCallHelper(e, i, reinterpret_cast<void*>(EmulateUNPACK_SHORT_2));
  }
  static void EmitSHORT_4(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src1.value->IsConstantZero()) {
      vec128_t c;
      c.f32[0] = 3.0f;
      c.f32[1] = 3.0f;
      c.f32[2] = 3.0f;
      c.f32[3] = 3.0f;
      LoadV128Const(e, i.dest.reg().getIdx(), c);
      return;
    }
    EmitCallHelper(e, i, reinterpret_cast<void*>(EmulateUNPACK_SHORT_4));
  }
  static void EmitUINT_2101010(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src1.value->IsConstantZero()) {
      vec128_t c;
      c.f32[0] = 3.0f;
      c.f32[1] = 3.0f;
      c.f32[2] = 3.0f;
      c.f32[3] = 1.0f;
      LoadV128Const(e, i.dest.reg().getIdx(), c);
      return;
    }
    EmitCallHelper(e, i, reinterpret_cast<void*>(EmulateUNPACK_UINT_2101010));
  }
  static void EmitULONG_4202020(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src1.value->IsConstantZero()) {
      vec128_t c;
      c.f32[0] = 3.0f;
      c.f32[1] = 3.0f;
      c.f32[2] = 3.0f;
      c.f32[3] = 1.0f;
      LoadV128Const(e, i.dest.reg().getIdx(), c);
      return;
    }
    EmitCallHelper(e, i, reinterpret_cast<void*>(EmulateUNPACK_ULONG_4202020));
  }
  // Keep existing 8_IN_16 and 16_IN_32 implementations unchanged:
  static void Emit8_IN_16(A64Emitter& e, const EmitArgType& i, uint32_t flags) {
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();
    bool is_unsigned = hir::IsPackOutUnsigned(flags);
    bool to_hi = hir::IsPackToHi(flags);
    // PPC stores vectors in word-swapped form: u32[0]=PPC_W0, but within each
    // word, bytes are in LE order.  PPC "high" bytes (0-7) are in the NEON low
    // half but with per-word byte reversal.  rev32(h8) swaps 16-bit halves
    // within each 32-bit word, fixing the byte order for sign extension.
    e.rev32(VReg(d).h8, VReg(s).h8);
    if (to_hi) {
      // PPC high bytes are in the NEON low half after rev32.
      if (is_unsigned)
        e.uxtl(VReg(d).h8, VReg(d).b8);
      else
        e.sxtl(VReg(d).h8, VReg(d).b8);
    } else {
      // PPC low bytes are in the NEON high half after rev32.
      if (is_unsigned)
        e.uxtl2(VReg(d).h8, VReg(d).b16);
      else
        e.sxtl2(VReg(d).h8, VReg(d).b16);
    }
  }
  static void Emit16_IN_32(A64Emitter& e, const EmitArgType& i,
                           uint32_t flags) {
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();
    bool is_unsigned = hir::IsPackOutUnsigned(flags);
    bool to_hi = hir::IsPackToHi(flags);
    // PPC "high" halfwords (HW0-3) are in the NEON low half (h[0]-h[3])
    // but with pairs swapped within each 32-bit word (HW0 at h[1], HW1 at
    // h[0], etc.).  Sign-extend the correct half, then rev64(s4) swaps
    // 32-bit pairs to fix the halfword ordering.
    if (to_hi) {
      // PPC high halfwords → NEON low half.
      if (is_unsigned)
        e.uxtl(VReg(d).s4, VReg(s).h4);
      else
        e.sxtl(VReg(d).s4, VReg(s).h4);
    } else {
      // PPC low halfwords → NEON high half.
      if (is_unsigned)
        e.uxtl2(VReg(d).s4, VReg(s).h8);
      else
        e.sxtl2(VReg(d).s4, VReg(s).h8);
    }
    e.rev64(VReg(d).s4, VReg(d).s4);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_UNPACK, UNPACK);

// ============================================================================
// OPCODE_LVL (Load Vector Left)
// ============================================================================
struct LVL_V128 : Sequence<LVL_V128, I<OPCODE_LVL, V128Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    int d = i.dest.reg().getIdx();
    // x1 = host address = membase + guest_addr
    e.add(e.x1, e.GetMembaseReg(), addr);
    // x2 = result pointer (scratch area)
    e.add(e.x2, e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateLVL));
    e.ldr(QReg(d),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LVL, LVL_V128);

// ============================================================================
// OPCODE_LVR (Load Vector Right)
// ============================================================================
struct LVR_V128 : Sequence<LVR_V128, I<OPCODE_LVR, V128Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    int d = i.dest.reg().getIdx();
    e.add(e.x1, e.GetMembaseReg(), addr);
    e.add(e.x2, e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateLVR));
    e.ldr(QReg(d),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LVR, LVR_V128);

// ============================================================================
// OPCODE_STVL (Store Vector Left)
// ============================================================================
struct STVL_V128 : Sequence<STVL_V128, I<OPCODE_STVL, VoidOp, I64Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    int s = SrcVReg(e, i.src2, 2);
    // Store source vec to scratch for C helper to read.
    e.str(QReg(s),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    // x1 = host address, x2 = src data pointer
    e.add(e.x1, e.GetMembaseReg(), addr);
    e.add(e.x2, e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateSTVL));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STVL, STVL_V128);

// ============================================================================
// OPCODE_STVR (Store Vector Right)
// ============================================================================
struct STVR_V128 : Sequence<STVR_V128, I<OPCODE_STVR, VoidOp, I64Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    int s = SrcVReg(e, i.src2, 2);
    e.str(QReg(s),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.add(e.x1, e.GetMembaseReg(), addr);
    e.add(e.x2, e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateSTVR));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STVR, STVR_V128);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
