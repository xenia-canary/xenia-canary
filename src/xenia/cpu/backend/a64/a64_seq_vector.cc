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
      LoadV128Const(e, i.dest.reg().getIdx(), vec128b(i.src1.constant()));
    } else {
      e.dup(VReg(i.dest.reg().getIdx()).b16, i.src1);
    }
  }
};
struct SPLAT_I16 : Sequence<SPLAT_I16, I<OPCODE_SPLAT, V128Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      LoadV128Const(e, i.dest.reg().getIdx(), vec128s(i.src1.constant()));
    } else {
      e.dup(VReg(i.dest.reg().getIdx()).h8, i.src1);
    }
  }
};
struct SPLAT_I32 : Sequence<SPLAT_I32, I<OPCODE_SPLAT, V128Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      LoadV128Const(e, i.dest.reg().getIdx(), vec128i(i.src1.constant()));
    } else {
      e.dup(VReg(i.dest.reg().getIdx()).s4, i.src1);
    }
  }
};
struct SPLAT_F32 : Sequence<SPLAT_F32, I<OPCODE_SPLAT, V128Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      LoadV128Const(e, i.dest.reg().getIdx(),
                    vec128i(std::bit_cast<uint32_t, float>(i.src1.constant())));
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
    LoadV128Const(e, 2, vec128i(0x7F800000u), 0);
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
    e.fmov(VReg(0).s4, 3.0f);
    e.fmaxnm(VReg(d).s4, VReg(s).s4, VReg(0).s4);
    LoadV128Const(e, 0, vec128i(0x404000FFu), 0);  // 3.0f + 255*2^-22
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
    LoadV128Const(e, 0, vec128i(0x403F8001u), 0);
    e.fmaxnm(VReg(d).s4, VReg(s).s4, VReg(0).s4);
    LoadV128Const(e, 0, vec128i(0x40407FFFu), 0);
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
    LoadV128Const(e, 0, vec128i(0x403F8001u), 0);
    e.fmaxnm(VReg(d).s4, VReg(s).s4, VReg(0).s4);
    LoadV128Const(e, 0, vec128i(0x40407FFFu), 0);
    e.fminnm(VReg(d).s4, VReg(d).s4, VReg(0).s4);
    // TBL ctrl for PACK_SHORT_4: bytes 8-11={0x04,0x05,0x00,0x01},
    // 12-15={0x0C,0x0D,0x08,0x09}
    vec128_t ctrl;
    ctrl.low = 0xFFFFFFFF'FFFFFFFFull;
    ctrl.high = 0x09080D0C'01000504ull;
    LoadV128Const(e, 0, ctrl);
    e.tbl(VReg(d).b16, VReg(d).b16, 1, VReg(0).b16);
  }
  // Inline Xenos float→half conversion for 4 float32 lanes in v0.
  // Xenos half saturates to 0x7FFF instead of producing IEEE inf/NaN.
  // Denormals flush to zero (preserve_denormal=false).
  // If round_to_even is true, applies round-to-nearest-even before truncation.
  // Result: 4 halfwords in the low 16 bits of each 32-bit lane in v1.
  // Clobbers v0-v3, w0.  Sign is spilled to GUEST_SCRATCH.
  static void EmitFloatToXenosHalf4(A64Emitter& e, bool round_to_even) {
    // v0 = input float32 bits.  Extract sign, compute abs.
    e.ushr(VReg(1).s4, VReg(0).s4, 31);
    e.shl(VReg(1).s4, VReg(1).s4, 15);  // v1 = sign at bit 15
    // Spill sign to stack (we need all 4 scratch regs).
    e.str(QReg(1),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));

    // v0 = abs (clear sign bit)
    e.shl(VReg(0).s4, VReg(0).s4, 1);
    e.ushr(VReg(0).s4, VReg(0).s4, 1);

    // Normal path: rebias exponent and shift.
    // result = abs + 0xC8000000 (subtract 112 from float exponent, unsigned)
    LoadV128Const(e, 2, vec128i(0xC8000000u), 0);
    e.add(VReg(2).s4, VReg(0).s4, VReg(2).s4);  // v2 = rebiased

    if (round_to_even) {
      // Round to nearest even: result += 0xFFF + ((result >> 13) & 1)
      e.ushr(VReg(3).s4, VReg(2).s4, 13);
      e.movi(VReg(1).s4, 1);
      e.and_(VReg(3).b16, VReg(3).b16, VReg(1).b16);  // (result>>13)&1
      LoadV128Const(e, 1, vec128i(0xFFFu), 0);
      e.add(VReg(3).s4, VReg(3).s4, VReg(1).s4);  // 0xFFF + bit
      e.add(VReg(2).s4, VReg(2).s4, VReg(3).s4);  // rounded
    }

    // Shift and mask to 15 bits.
    e.ushr(VReg(2).s4, VReg(2).s4, 13);
    LoadV128Const(e, 3, vec128i(0x7FFFu), 0);
    e.and_(VReg(2).b16, VReg(2).b16, VReg(3).b16);  // v2 = normal result

    // Saturation: where abs >= 0x47FFE000, force to 0x7FFF.
    // v3 already holds 0x7FFF splat = saturation value.
    LoadV128Const(e, 1, vec128i(0x47FFE000u), 0);
    e.cmhs(VReg(1).s4, VReg(0).s4, VReg(1).s4);    // v1 = sat mask
    e.bsl(VReg(1).b16, VReg(3).b16, VReg(2).b16);  // v1 = sat?7FFF:normal

    // Flush: where abs < 0x38800000, force to 0.
    LoadV128Const(e, 2, vec128i(0x38800000u), 0);
    e.cmhi(VReg(2).s4, VReg(2).s4, VReg(0).s4);    // v2 = small mask
    e.bic(VReg(1).b16, VReg(1).b16, VReg(2).b16);  // zero where small

    // Restore sign from stack and combine.
    e.ldr(QReg(0),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.orr(VReg(1).b16, VReg(1).b16, VReg(0).b16);
    // v1 = 4 Xenos halfs in low 16 bits of each 32-bit lane.
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
    e.mov(VReg(0).b16, VReg(s).b16);
    EmitFloatToXenosHalf4(e, false);
    // v1 has halfs in s[0..3].  FLOAT16_2 uses only lanes 0,1.
    // Output: h[7]=half(f32[0]), h[6]=half(f32[1]), rest=0.
    // Narrow to halfwords, swap within word pairs, place at top of vector.
    e.xtn(VReg(0).h4, VReg(1).s4);    // v0.h[0..3] = narrowed halfs
    e.rev32(VReg(0).h4, VReg(0).h4);  // swap within word pairs
    // v0.s[0] now has {half[1], half[0]} — put at s[3] of zeroed dest.
    e.movi(VReg(d).d2, 0);
    e.ins(VReg(d).s4[3], VReg(0).s4[0]);
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
    e.mov(VReg(0).b16, VReg(s).b16);
    EmitFloatToXenosHalf4(e, true);
    // v1 has halfs in s[0..3].  Output mapping:
    //   idx0→h[5], idx1→h[4], idx2→h[7], idx3→h[6]
    // = upper 64 bits with within-word swap, lower 64 bits zero.
    // Narrow to halfwords.
    e.xtn(VReg(0).h4, VReg(1).s4);    // v0.h[0..3] = narrowed halfs
    e.rev32(VReg(0).h4, VReg(0).h4);  // swap within word pairs
    // v0 low 64 = {half[1],half[0], half[3],half[2]} — goes to upper 64.
    e.movi(VReg(d).d2, 0);
    e.ins(VReg(d).d2[1], VReg(0).d2[0]);
  }
  static void EmitUINT_2101010(A64Emitter& e, const EmitArgType& i) {
    // Inline PACK: clamp magic floats, extract bit fields, shift, OR-fold.
    int s = SrcVReg(e, i.src1, 2);
    int d = i.dest.reg().getIdx();

    // Clamp to valid range: fmaxnm(src, min) then fminnm(result, max).
    // fmaxnm clamps NaN to min (NaN packs as the minimum value).
    // XYZ min=0x403FFE01 (-511), max=0x404001FF (+511)
    // W   min=0x40400000 (0),    max=0x40400003 (+3)
    e.mov(e.x0, 0x403FFE01403FFE01ull);
    e.fmov(DReg(0), e.x0);
    e.mov(e.x0, 0x40400000403FFE01ull);
    e.ins(VReg(0).d2[1], e.x0);
    e.fmaxnm(VReg(d).s4, VReg(s).s4, VReg(0).s4);

    e.mov(e.x0, 0x404001FF404001FFull);
    e.fmov(DReg(0), e.x0);
    e.mov(e.x0, 0x40400003404001FFull);
    e.ins(VReg(0).d2[1], e.x0);
    e.fminnm(VReg(d).s4, VReg(d).s4, VReg(0).s4);

    // Mask to extract encoded integer bits: {0x3FF, 0x3FF, 0x3FF, 0x3}
    e.mov(e.x0, 0x000003FF000003FFull);
    e.fmov(DReg(0), e.x0);
    e.mov(e.x0, 0x00000003000003FFull);
    e.ins(VReg(0).d2[1], e.x0);
    e.and_(VReg(d).b16, VReg(d).b16, VReg(0).b16);

    // Variable left-shift to position each field: {<<0, <<10, <<20, <<30}
    e.mov(e.x0, 0x0000000A00000000ull);  // {0, 10}
    e.fmov(DReg(0), e.x0);
    e.mov(e.x0, 0x0000001E00000014ull);  // {20, 30}
    e.ins(VReg(0).d2[1], e.x0);
    e.ushl(VReg(d).s4, VReg(d).s4, VReg(0).s4);

    // OR-fold all 4 lanes into one packed value.
    e.ext(VReg(0).b16, VReg(d).b16, VReg(d).b16, 4);
    e.orr(VReg(d).b16, VReg(d).b16, VReg(0).b16);
    e.ext(VReg(0).b16, VReg(d).b16, VReg(d).b16, 8);
    e.orr(VReg(d).b16, VReg(d).b16, VReg(0).b16);
    // Result is in all 4 lanes; consumer reads u32[3].
  }
  static void EmitULONG_4202020(A64Emitter& e, const EmitArgType& i) {
    // Inline PACK: clamp magic floats, extract bit fields, pack into 64-bit.
    // 20-bit fields cross 32-bit boundaries, so use scalar GPR packing.
    int s = SrcVReg(e, i.src1, 2);
    int d = i.dest.reg().getIdx();

    // Clamp: fmaxnm(src, min) then fminnm(result, max).
    // XYZ min=0x40400000+(-524287)=0x40380001, max=0x40400000+524287=0x4047FFFF
    // W   min=0x40400000+0=0x40400000, max=0x40400000+15=0x4040000F
    e.mov(e.x0, 0x4038000140380001ull);
    e.fmov(DReg(0), e.x0);
    e.mov(e.x0, 0x4040000040380001ull);
    e.ins(VReg(0).d2[1], e.x0);
    e.fmaxnm(VReg(d).s4, VReg(s).s4, VReg(0).s4);

    e.mov(e.x0, 0x4047FFFF4047FFFFull);
    e.fmov(DReg(0), e.x0);
    e.mov(e.x0, 0x4040000F4047FFFFull);
    e.ins(VReg(0).d2[1], e.x0);
    e.fminnm(VReg(d).s4, VReg(d).s4, VReg(0).s4);

    // Extract clamped values to GPRs: mask low bits from magic float encoding.
    // XYZ: & 0xFFFFF (20 bits), W: & 0xF (4 bits)
    e.umov(e.w1, VReg(d).s4[0]);
    e.and_(e.w1, e.w1, 0xFFFFF);  // x
    e.umov(e.w2, VReg(d).s4[1]);
    e.and_(e.w2, e.w2, 0xFFFFF);  // y
    e.umov(e.w3, VReg(d).s4[2]);
    e.and_(e.w3, e.w3, 0xFFFFF);  // z
    e.umov(e.w4, VReg(d).s4[3]);
    e.and_(e.w4, e.w4, 0xF);  // w

    // Pack into 64-bit: x | (y<<20) | (z<<40) | (w<<60)
    e.orr(e.x0, e.x1, e.x2, LSL, 20);
    e.orr(e.x0, e.x0, e.x3, LSL, 40);
    e.orr(e.x0, e.x0, e.x4, LSL, 60);

    // Store as u32[2]=high, u32[3]=low.  ror 32 swaps halves for LE layout.
    e.ror(e.x0, e.x0, 32);
    e.movi(VReg(d).d2, 0);
    e.ins(VReg(d).d2[1], e.x0);
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
      e.fmov(VReg(d).s4, 1.0f);
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
    e.fmov(VReg(0).s4, 1.0f);
    e.orr(VReg(d).b16, VReg(d).b16, VReg(0).b16);
  }
  // Inline Xenos half→float conversion for 4 lanes in v0 (zero-extended
  // to 32-bit).  Xenos half-float has no inf/NaN (exp=31 is a normal
  // value), so we cannot use fcvtl.  Instead: integer shift + bias.
  // Result written to v_dest.  Clobbers v0-v3, w0.
  static void EmitXenosHalfToFloat4(A64Emitter& e, int dest) {
    // v0 = zero-extended halfs (16-bit values in 32-bit lanes).
    // abs = v0 & 0x7FFF
    LoadV128Const(e, 1, vec128i(0x7FFFu), 0);
    e.and_(VReg(2).b16, VReg(0).b16, VReg(1).b16);  // v2 = abs

    // value = (abs << 13) + 0x38000000  (bias adjustment: 112 << 23)
    e.shl(VReg(1).s4, VReg(2).s4, 13);
    LoadV128Const(e, 3, vec128i(0x38000000u), 0);
    e.add(VReg(1).s4, VReg(1).s4, VReg(3).s4);  // v1 = biased value

    // sign = (v0 >> 15) << 31
    e.ushr(VReg(3).s4, VReg(0).s4, 15);
    e.shl(VReg(3).s4, VReg(3).s4, 31);  // v3 = sign at bit 31

    // result = sign | value
    e.orr(VReg(0).b16, VReg(3).b16, VReg(1).b16);

    // Flush to ±0 where exponent == 0 (denormals and zeros).
    // exp = abs >> 10
    e.ushr(VReg(1).s4, VReg(2).s4, 10);
    e.cmeq(VReg(1).s4, VReg(1).s4, 0);  // v1 = mask: all-1s where exp==0
    // Where exp==0: use sign only (±0), else: use full result.
    e.bsl(VReg(1).b16, VReg(3).b16, VReg(0).b16);

    // Fix PPC word-pair ordering.
    e.rev64(VReg(dest).s4, VReg(1).s4);
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
    // Input halfs are at h[6],h[7] (bytes 12-15). Rotate to h[0],h[1]
    // and zero-extend to 32-bit lanes for the conversion helper.
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();
    e.ext(VReg(0).b16, VReg(s).b16, VReg(s).b16, 12);
    e.uxtl(VReg(0).s4, VReg(0).h4);
    EmitXenosHalfToFloat4(e, d);
    // Only lanes 0,1 are valid. Set lanes 2,3 = {0.0f, 1.0f}.
    e.ins(VReg(d).s4[2], e.wzr);
    e.mov(e.w0, 0x3F800000u);
    e.ins(VReg(d).s4[3], e.w0);
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
    // Input halfs are at h[4..7] (upper 64 bits). Zero-extend to 32-bit
    // lanes and run the conversion.
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();
    e.uxtl2(VReg(0).s4, VReg(s).h8);
    EmitXenosHalfToFloat4(e, d);
  }
  // Apply the magic-float overflow fixup on 4 lanes in v0.
  // If any lane equals 0x403F8000 (short value -32768 overflows the
  // encoding), replace it with QNaN 0x7FC00000.  Result in v_dest.
  // Clobbers v0-v2, w0.
  static void EmitMagicFloatOverflowCheck(A64Emitter& e, int dest) {
    LoadV128Const(e, 1, vec128i(0x403F8000u), 0);
    e.cmeq(VReg(1).s4, VReg(0).s4, VReg(1).s4);
    LoadV128Const(e, 2, vec128i(0x7FC00000u), 0);
    e.bsl(VReg(1).b16, VReg(2).b16, VReg(0).b16);
    if (dest != 1) {
      e.mov(VReg(dest).b16, VReg(1).b16);
    }
  }
  static void EmitSHORT_2(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src1.value->IsConstantZero()) {
      vec128_t c;
      c.f32[0] = 3.0f;
      c.f32[1] = 3.0f;
      c.f32[2] = 0.0f;
      c.f32[3] = 1.0f;
      LoadV128Const(e, i.dest.reg().getIdx(), c);
      return;
    }
    // Source shorts at h[6],h[7] (top 32 bits).  Rotate to h[0..1],
    // sign-extend to 32-bit, add magic constant, fix ordering.
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();
    e.ext(VReg(0).b16, VReg(s).b16, VReg(s).b16, 12);
    e.sxtl(VReg(0).s4, VReg(0).h4);
    // v0 = {sext(h[6])=y, sext(h[7])=x, junk, junk}
    LoadV128Const(e, 1, vec128i(0x40400000u), 0);
    e.add(VReg(0).s4, VReg(0).s4, VReg(1).s4);
    // Swap low pair to get {magic+x, magic+y, ...}
    e.rev64(VReg(0).s4, VReg(0).s4);
    // Set lanes 2,3 = {0.0f, 1.0f}
    e.ins(VReg(0).s4[2], e.wzr);
    e.mov(e.w0, 0x3F800000u);
    e.ins(VReg(0).s4[3], e.w0);
    EmitMagicFloatOverflowCheck(e, d);
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
    // Source shorts at h[4..7] (upper 64 bits).
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();
    // Sign-extend upper halfs: sxtl2 h[4..7] → s[0..3] = {w, z, y, x}
    e.sxtl2(VReg(0).s4, VReg(s).h8);
    LoadV128Const(e, 1, vec128i(0x40400000u), 0);
    e.add(VReg(0).s4, VReg(0).s4, VReg(1).s4);
    // Reorder {w,z,y,x} → {x,y,z,w}: rev64 then swap halves.
    e.rev64(VReg(0).s4, VReg(0).s4);                  // {z,w,x,y}
    e.ext(VReg(0).b16, VReg(0).b16, VReg(0).b16, 8);  // {x,y,z,w}
    EmitMagicFloatOverflowCheck(e, d);
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
    // Inline UNPACK: extract 10-10-10-2 fields, sign-extend XYZ, add magic.
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();

    // Splat the packed u32[3] to all 4 lanes.
    e.dup(VReg(0).s4, VReg(s).s4[3]);

    // Mask per-lane bit fields: {0x3FF, 0xFFC00, 0x3FF00000, 0xC0000000}
    e.mov(e.x0, 0x000FFC00000003FFull);
    e.fmov(DReg(1), e.x0);
    e.mov(e.x0, 0xC00000003FF00000ull);
    e.ins(VReg(1).d2[1], e.x0);
    e.and_(VReg(0).b16, VReg(0).b16, VReg(1).b16);

    // Variable right-shift to align each field to bit 0.
    // Shift counts: {0, -10, -20, -30} (negative = right shift for ushl).
    e.mov(e.x0, 0xFFFFFFF600000000ull);  // {0, -10}
    e.fmov(DReg(1), e.x0);
    e.mov(e.x0, 0xFFFFFFE2FFFFFFECull);  // {-20, -30}
    e.ins(VReg(1).d2[1], e.x0);
    e.ushl(VReg(0).s4, VReg(0).s4, VReg(1).s4);

    // Sign-extend XYZ from 10-bit via shl 22 + sshr 22.
    // W is 2-bit unsigned — shl 22 puts it at bits 22-23 with bit 31=0,
    // so sshr 22 zero-extends it correctly.
    e.shl(VReg(0).s4, VReg(0).s4, 22);
    e.sshr(VReg(0).s4, VReg(0).s4, 22);

    // Add magic constants: {3.0, 3.0, 3.0, 1.0}
    e.mov(e.x0, 0x4040000040400000ull);
    e.fmov(DReg(1), e.x0);
    e.mov(e.x0, 0x3F80000040400000ull);
    e.ins(VReg(1).d2[1], e.x0);
    e.add(VReg(0).s4, VReg(0).s4, VReg(1).s4);

    // Overflow check: XYZ == 0x403FFE00 → QNaN (0x7FC00000).
    // W result (0x3F800000..0x3F800003) never matches, so splat is safe.
    LoadV128Const(e, 1, vec128i(0x403FFE00u), 0);
    e.cmeq(VReg(1).s4, VReg(0).s4, VReg(1).s4);
    LoadV128Const(e, 2, vec128i(0x7FC00000u), 0);
    e.bsl(VReg(1).b16, VReg(2).b16, VReg(0).b16);
    if (d != 1) {
      e.mov(VReg(d).b16, VReg(1).b16);
    }
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
    // Inline UNPACK: extract 20-20-20-4 fields from 64-bit packed value,
    // sign-extend XYZ, add magic constants.  Fields cross 32-bit boundaries
    // so we use scalar GPR extraction via sbfx/ubfx.
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();

    // Extract 64-bit packed from d2[1] (u32[2]:u32[3]).
    // LE d2[1] = u32[2] | (u32[3]<<32), but packed = (u32[2]<<32) | u32[3].
    // ror by 32 swaps the two 32-bit halves.
    e.umov(e.x0, VReg(s).d2[1]);
    e.ror(e.x0, e.x0, 32);

    // Extract and sign-extend each field.
    e.sbfx(e.x1, e.x0, 0, 20);   // x: bits 0-19, signed
    e.sbfx(e.x2, e.x0, 20, 20);  // y: bits 20-39, signed
    e.sbfx(e.x3, e.x0, 40, 20);  // z: bits 40-59, signed
    e.ubfx(e.x4, e.x0, 60, 4);   // w: bits 60-63, unsigned

    // Add magic constants and insert into dest vector.
    e.mov(e.w0, 0x40400000u);
    e.add(e.w1, e.w1, e.w0);  // x + 3.0
    e.add(e.w2, e.w2, e.w0);  // y + 3.0
    e.add(e.w3, e.w3, e.w0);  // z + 3.0
    e.mov(e.w0, 0x3F800000u);
    e.add(e.w4, e.w4, e.w0);  // w + 1.0

    e.ins(VReg(d).s4[0], e.w1);
    e.ins(VReg(d).s4[1], e.w2);
    e.ins(VReg(d).s4[2], e.w3);
    e.ins(VReg(d).s4[3], e.w4);

    // Overflow: XYZ == 0x40380000 → QNaN.  W can't match.
    LoadV128Const(e, 0, vec128i(0x40380000u), 0);
    e.cmeq(VReg(0).s4, VReg(d).s4, VReg(0).s4);
    LoadV128Const(e, 1, vec128i(0x7FC00000u), 0);
    e.bsl(VReg(0).b16, VReg(1).b16, VReg(d).b16);
    e.mov(VReg(d).b16, VReg(0).b16);
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
    // Inline LVL using TBL.  The bswap-within-lanes base pattern plus the
    // address offset gives a TBL control vector.  Indices >= 16 naturally
    // produce zero from TBL, which is the correct LVL behaviour.
    auto addr = ComputeMemoryAddress(e, i.src1);
    int d = i.dest.reg().getIdx();

    // x0 = host address
    e.add(e.x0, e.GetMembaseReg(), addr);
    // w17 = offset within 16-byte block
    e.and_(e.w17, e.w0, 0xF);
    // Align address down to 16-byte boundary and load.
    e.and_(e.x0, e.x0, ~0xFull);
    e.ldr(QReg(0), ptr(e.x0));

    // Build bswap-within-lanes base pattern in v1:
    //   {3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12}
    e.mov(e.x0, 0x0405060700010203ull);
    e.fmov(DReg(1), e.x0);
    e.mov(e.x0, 0x0C0D0E0F08090A0Bull);
    e.ins(VReg(1).d2[1], e.x0);

    // ctrl = base + offset; TBL gives 0 for ctrl >= 16.
    e.dup(VReg(2).b16, e.w17);
    e.add(VReg(1).b16, VReg(1).b16, VReg(2).b16);

    // Single-register TBL: dest[i] = mem[ctrl[i]] or 0.
    e.tbl(VReg(d).b16, VReg(0).b16, 1, VReg(1).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LVL, LVL_V128);

// ============================================================================
// OPCODE_LVR (Load Vector Right)
// ============================================================================
struct LVR_V128 : Sequence<LVR_V128, I<OPCODE_LVR, V128Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Inline LVR using a 2-register TBL.  Table = {zeros, mem}.
    // Indices 0-15 read zeros (from v0), 16-31 read mem (from v1).
    // base + offset produces indices > 15 exactly where LVR should output
    // the memory bytes, and <= 15 where it should output zero.
    // When offset == 0, all indices are 0-15 → all zeros, which is correct.
    auto addr = ComputeMemoryAddress(e, i.src1);
    int d = i.dest.reg().getIdx();

    // x0 = host address
    e.add(e.x0, e.GetMembaseReg(), addr);
    // w17 = offset
    e.and_(e.w17, e.w0, 0xF);
    // Align and load.  v0=zeros (table reg 0), v1=mem (table reg 1).
    e.movi(VReg(0).d2, 0);
    e.and_(e.x0, e.x0, ~0xFull);
    e.ldr(QReg(1), ptr(e.x0));

    // Build base pattern in v2.
    e.mov(e.x0, 0x0405060700010203ull);
    e.fmov(DReg(2), e.x0);
    e.mov(e.x0, 0x0C0D0E0F08090A0Bull);
    e.ins(VReg(2).d2[1], e.x0);

    // ctrl = base + offset.
    e.dup(VReg(3).b16, e.w17);
    e.add(VReg(2).b16, VReg(2).b16, VReg(3).b16);

    // 2-register TBL over {v0, v1}.
    e.tbl(VReg(d).b16, VReg(0).b16, 2, VReg(2).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LVR, LVR_V128);

// ============================================================================
// OPCODE_STVL (Store Vector Left)
// ============================================================================
struct STVL_V128 : Sequence<STVL_V128, I<OPCODE_STVL, VoidOp, I64Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Inline STVL using 2-register TBL over {original_mem, rev32(src)}.
    // STVL writes bytes offset..15 from the byte-swapped source.
    // ctrl[i] = i (keep original) where i < offset,
    // ctrl[i] = 16 + (i - offset) (from rev32(src)) where i >= offset.
    // This equals: ctrl = identity + (mask & delta)
    //   where mask = (i >= offset), delta = (16 - offset).
    auto addr = ComputeMemoryAddress(e, i.src1);
    int s = SrcVReg(e, i.src2, 2);

    // x0 = host address, w17 = offset, x16 = aligned address (saved).
    e.add(e.x0, e.GetMembaseReg(), addr);
    e.and_(e.w17, e.w0, 0xF);
    e.and_(e.x0, e.x0, ~0xFull);
    e.mov(e.x16, e.x0);  // save aligned addr for final store

    // v0 = original mem (table reg 0), v1 = rev32(src) (table reg 1).
    e.ldr(QReg(0), ptr(e.x0));
    e.rev32(VReg(1).b16, VReg(s).b16);

    // Build identity {0,1,...,15} in v2.
    e.mov(e.x0, 0x0706050403020100ull);
    e.fmov(DReg(2), e.x0);
    e.mov(e.x0, 0x0F0E0D0C0B0A0908ull);
    e.ins(VReg(2).d2[1], e.x0);

    // Save identity to stack scratch (needed after mask computation).
    e.str(QReg(2),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));

    // v3 = mask: 0xFF where i >= offset.
    e.dup(VReg(3).b16, e.w17);
    e.cmhs(VReg(3).b16, VReg(2).b16, VReg(3).b16);

    // v2 = delta splat = (16 - offset).
    e.mov(e.w0, 16);
    e.sub(e.w0, e.w0, e.w17);
    e.dup(VReg(2).b16, e.w0);

    // v3 = masked delta = mask & delta.
    e.and_(VReg(3).b16, VReg(3).b16, VReg(2).b16);

    // Restore identity and compute ctrl = identity + masked_delta.
    e.ldr(QReg(2),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.add(VReg(2).b16, VReg(2).b16, VReg(3).b16);

    // 2-register TBL: blend original mem and rev32(src).
    e.tbl(VReg(2).b16, VReg(0).b16, 2, VReg(2).b16);
    e.str(QReg(2), ptr(e.x16));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STVL, STVL_V128);

// ============================================================================
// OPCODE_STVR (Store Vector Right)
// ============================================================================
struct STVR_V128 : Sequence<STVR_V128, I<OPCODE_STVR, VoidOp, I64Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Inline STVR using 2-register TBL over {original_mem, rev32(src)}.
    // STVR writes bytes 0..offset-1 from the byte-swapped source tail.
    // ctrl[i] = 16 + (16 - offset + i) where i < offset (from rev32(src)),
    // ctrl[i] = i (keep original) where i >= offset.
    // This equals: ctrl = identity + (mask & delta)
    //   where mask = (i < offset), delta = (32 - offset).
    // When offset == 0, no bytes are written (mask is all-zero → identity →
    // load and store back the same memory, effectively a no-op).
    auto addr = ComputeMemoryAddress(e, i.src1);
    int s = SrcVReg(e, i.src2, 2);

    // x0 = host address, w17 = offset, x16 = aligned address (saved).
    e.add(e.x0, e.GetMembaseReg(), addr);
    e.and_(e.w17, e.w0, 0xF);
    e.and_(e.x0, e.x0, ~0xFull);
    e.mov(e.x16, e.x0);

    // v0 = original mem (table reg 0), v1 = rev32(src) (table reg 1).
    e.ldr(QReg(0), ptr(e.x0));
    e.rev32(VReg(1).b16, VReg(s).b16);

    // Build identity in v2.
    e.mov(e.x0, 0x0706050403020100ull);
    e.fmov(DReg(2), e.x0);
    e.mov(e.x0, 0x0F0E0D0C0B0A0908ull);
    e.ins(VReg(2).d2[1], e.x0);

    // Save identity to stack scratch.
    e.str(QReg(2),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));

    // v3 = mask: 0xFF where i < offset (complement of STVL's mask).
    e.dup(VReg(3).b16, e.w17);
    e.cmhi(VReg(3).b16, VReg(3).b16, VReg(2).b16);

    // v2 = delta splat = (32 - offset).
    e.mov(e.w0, 32);
    e.sub(e.w0, e.w0, e.w17);
    e.dup(VReg(2).b16, e.w0);

    // v3 = masked delta.
    e.and_(VReg(3).b16, VReg(3).b16, VReg(2).b16);

    // Restore identity and compute ctrl.
    e.ldr(QReg(2),
          ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.add(VReg(2).b16, VReg(2).b16, VReg(3).b16);

    // 2-register TBL and store.
    e.tbl(VReg(2).b16, VReg(0).b16, 2, VReg(2).b16);
    e.str(QReg(2), ptr(e.x16));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STVR, STVR_V128);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
