/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_OP_H_
#define XENIA_CPU_BACKEND_A64_A64_OP_H_

#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/hir/instr.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace Xbyak_aarch64;

// Selects the right byte/word/etc from a vector. PPC is big-endian so
// we need to flip logical indices (0,1,2,3,4,5,6,7,...) = (3,2,1,0,7,6,5,4,...)
#define VEC128_B(n) ((n) ^ 0x3)
#define VEC128_W(n) ((n) ^ 0x1)
#define VEC128_D(n) (n)
#define VEC128_F(n) (n)

enum KeyType {
  KEY_TYPE_X = OPCODE_SIG_TYPE_X,
  KEY_TYPE_L = OPCODE_SIG_TYPE_L,
  KEY_TYPE_O = OPCODE_SIG_TYPE_O,
  KEY_TYPE_S = OPCODE_SIG_TYPE_S,
  KEY_TYPE_V_I8 = OPCODE_SIG_TYPE_V + INT8_TYPE,
  KEY_TYPE_V_I16 = OPCODE_SIG_TYPE_V + INT16_TYPE,
  KEY_TYPE_V_I32 = OPCODE_SIG_TYPE_V + INT32_TYPE,
  KEY_TYPE_V_I64 = OPCODE_SIG_TYPE_V + INT64_TYPE,
  KEY_TYPE_V_F32 = OPCODE_SIG_TYPE_V + FLOAT32_TYPE,
  KEY_TYPE_V_F64 = OPCODE_SIG_TYPE_V + FLOAT64_TYPE,
  KEY_TYPE_V_V128 = OPCODE_SIG_TYPE_V + VEC128_TYPE,
};

using InstrKeyValue = uint32_t;
#pragma pack(push, 1)
union InstrKey {
  InstrKeyValue value;
  struct {
    InstrKeyValue opcode : 8;
    InstrKeyValue dest : 5;
    InstrKeyValue src1 : 5;
    InstrKeyValue src2 : 5;
    InstrKeyValue src3 : 5;
    InstrKeyValue reserved : 4;
  };

  operator InstrKeyValue() const { return value; }

  InstrKey() : value(0) { static_assert_size(*this, sizeof(value)); }
  InstrKey(InstrKeyValue v) : value(v) {}

  InstrKey(const Instr* i) : value(0) {
    const OpcodeInfo* info = i->GetOpcodeInfo();
    InstrKeyValue sig = info->signature;

    OpcodeSignatureType dest_type, src1_type, src2_type, src3_type;
    UnpackOpcodeSig(sig, dest_type, src1_type, src2_type, src3_type);

    InstrKeyValue out_desttype = (InstrKeyValue)dest_type;
    InstrKeyValue out_src1type = (InstrKeyValue)src1_type;
    InstrKeyValue out_src2type = (InstrKeyValue)src2_type;
    InstrKeyValue out_src3type = (InstrKeyValue)src3_type;

    Value* destv = i->dest;
    Value* src1v = i->src1.value;
    Value* src2v = i->src2.value;
    Value* src3v = i->src3.value;

    if (out_src1type == OPCODE_SIG_TYPE_V) {
      out_src1type += src1v->type;
    }
    if (out_src2type == OPCODE_SIG_TYPE_V) {
      out_src2type += src2v->type;
    }
    if (out_src3type == OPCODE_SIG_TYPE_V) {
      out_src3type += src3v->type;
    }
    opcode = info->num;
    dest = out_desttype ? OPCODE_SIG_TYPE_V + destv->type : 0;
    src1 = out_src1type;
    src2 = out_src2type;
    src3 = out_src3type;
  }

  template <Opcode OPCODE, KeyType DEST = KEY_TYPE_X, KeyType SRC1 = KEY_TYPE_X,
            KeyType SRC2 = KEY_TYPE_X, KeyType SRC3 = KEY_TYPE_X>
  struct Construct {
    static const InstrKeyValue value =
        (OPCODE) | (DEST << 8) | (SRC1 << 13) | (SRC2 << 18) | (SRC3 << 23);
  };
};
#pragma pack(pop)
static_assert(sizeof(InstrKey) <= 4, "Key must be 4 bytes");

// ============================================================================
// Op types — architecture-independent
// ============================================================================

struct OpBase {};

template <typename T, KeyType KEY_TYPE>
struct Op : OpBase {
  static constexpr KeyType key_type = KEY_TYPE;
};

struct VoidOp : Op<VoidOp, KEY_TYPE_X> {
 protected:
  template <hir::Opcode OPCODE, typename... Ts>
  friend struct I;
  void Load(const Instr::Op& op) {}
};

struct OffsetOp : Op<OffsetOp, KEY_TYPE_O> {
  uint64_t value;

 protected:
  template <hir::Opcode OPCODE, typename... Ts>
  friend struct I;
  void Load(const Instr::Op& op) { this->value = op.offset; }
};

struct SymbolOp : Op<SymbolOp, KEY_TYPE_S> {
  Function* value;

 protected:
  template <hir::Opcode OPCODE, typename... Ts>
  friend struct I;
  bool Load(const Instr::Op& op) {
    this->value = op.symbol;
    return true;
  }
};

struct LabelOp : Op<LabelOp, KEY_TYPE_L> {
  hir::Label* value;

 protected:
  template <hir::Opcode OPCODE, typename... Ts>
  friend struct I;
  void Load(const Instr::Op& op) { this->value = op.label; }
};

// ============================================================================
// ValueOp — ARM64 register type specializations
// ============================================================================

template <typename T, KeyType KEY_TYPE, typename REG_TYPE, typename CONST_TYPE>
struct ValueOp : Op<ValueOp<T, KEY_TYPE, REG_TYPE, CONST_TYPE>, KEY_TYPE> {
  typedef REG_TYPE reg_type;
  const Value* value;
  bool is_constant;
  virtual bool ConstantFitsIn32Reg() const { return true; }

  const REG_TYPE& reg() const {
    assert_true(!is_constant);
    return reg_;
  }
  operator const REG_TYPE&() const { return reg(); }

  bool IsEqual(const T& b) const {
    if (is_constant && b.is_constant) {
      return reinterpret_cast<const T*>(this)->constant() == b.constant();
    } else if (!is_constant && !b.is_constant) {
      return reg_.getIdx() == b.reg_.getIdx();
    }
    return false;
  }
  bool operator==(const T& b) const { return IsEqual(b); }
  bool operator!=(const T& b) const { return !IsEqual(b); }

  void Load(const Instr::Op& op) {
    value = op.value;
    is_constant = value->IsConstant();
    if (!is_constant) {
      A64Emitter::SetupReg(value, reg_);
    }
  }

 protected:
  REG_TYPE reg_ = REG_TYPE(0);
};

// ARM64 integer operands use WReg (32-bit) and XReg (64-bit).
// I8 and I16 are handled via WReg with masking/extension as needed.
struct I8Op : ValueOp<I8Op, KEY_TYPE_V_I8, WReg, int8_t> {
  typedef ValueOp<I8Op, KEY_TYPE_V_I8, WReg, int8_t> BASE;
  int8_t constant() const {
    assert_true(BASE::is_constant);
    return BASE::value->constant.i8;
  }
};
struct I16Op : ValueOp<I16Op, KEY_TYPE_V_I16, WReg, int16_t> {
  typedef ValueOp<I16Op, KEY_TYPE_V_I16, WReg, int16_t> BASE;
  int16_t constant() const {
    assert_true(BASE::is_constant);
    return BASE::value->constant.i16;
  }
};
struct I32Op : ValueOp<I32Op, KEY_TYPE_V_I32, WReg, int32_t> {
  typedef ValueOp<I32Op, KEY_TYPE_V_I32, WReg, int32_t> BASE;
  int32_t constant() const {
    assert_true(BASE::is_constant);
    return BASE::value->constant.i32;
  }
};
struct I64Op : ValueOp<I64Op, KEY_TYPE_V_I64, XReg, int64_t> {
  typedef ValueOp<I64Op, KEY_TYPE_V_I64, XReg, int64_t> BASE;
  int64_t constant() const {
    assert_true(BASE::is_constant);
    return BASE::value->constant.i64;
  }
  bool ConstantFitsIn32Reg() const override {
    int64_t v = BASE::value->constant.i64;
    if ((v & ~0x7FFFFFFF) == 0) {
      return true;
    } else if ((v & ~0x7FFFFFFFUL) == ~0x7FFFFFFFUL) {
      return true;
    }
    return false;
  }
};

// ARM64 float/vector operands use SReg, DReg, QReg.
struct F32Op : ValueOp<F32Op, KEY_TYPE_V_F32, SReg, float> {
  typedef ValueOp<F32Op, KEY_TYPE_V_F32, SReg, float> BASE;
  float constant() const {
    assert_true(BASE::is_constant);
    return BASE::value->constant.f32;
  }
};
struct F64Op : ValueOp<F64Op, KEY_TYPE_V_F64, DReg, double> {
  typedef ValueOp<F64Op, KEY_TYPE_V_F64, DReg, double> BASE;
  double constant() const {
    assert_true(BASE::is_constant);
    return BASE::value->constant.f64;
  }
};
struct V128Op : ValueOp<V128Op, KEY_TYPE_V_V128, QReg, vec128_t> {
  typedef ValueOp<V128Op, KEY_TYPE_V_V128, QReg, vec128_t> BASE;
  const vec128_t& constant() const {
    assert_true(BASE::is_constant);
    return BASE::value->constant.v128;
  }
};

// ============================================================================
// DestField — handles loading the destination operand
// ============================================================================

template <typename DEST, typename... Tf>
struct DestField;
template <typename DEST>
struct DestField<DEST> {
  DEST dest;

 protected:
  bool LoadDest(const Instr* i) {
    Instr::Op op;
    op.value = i->dest;
    dest.Load(op);
    return true;
  }
};
template <>
struct DestField<VoidOp> {
 protected:
  bool LoadDest(const Instr* i) { return true; }
};

// ============================================================================
// I<> — instruction pattern with 0-3 source operands
// ============================================================================

template <hir::Opcode OPCODE, typename... Ts>
struct I;

template <hir::Opcode OPCODE, typename DEST>
struct I<OPCODE, DEST> : DestField<DEST> {
  typedef DestField<DEST> BASE;
  static constexpr hir::Opcode opcode = OPCODE;
  static const uint32_t key =
      InstrKey::Construct<OPCODE, DEST::key_type>::value;
  static const KeyType dest_type = DEST::key_type;
  const Instr* instr;

 protected:
  template <typename SEQ, typename T>
  friend struct Sequence;
  bool Load(const Instr* i, InstrKeyValue kv) {
    if (kv == key && BASE::LoadDest(i)) {
      instr = i;
      return true;
    }
    return false;
  }
};

template <hir::Opcode OPCODE, typename DEST, typename SRC1>
struct I<OPCODE, DEST, SRC1> : DestField<DEST> {
  typedef DestField<DEST> BASE;
  static constexpr hir::Opcode opcode = OPCODE;
  static const uint32_t key =
      InstrKey::Construct<OPCODE, DEST::key_type, SRC1::key_type>::value;
  static const KeyType dest_type = DEST::key_type;
  static const KeyType src1_type = SRC1::key_type;
  const Instr* instr;
  SRC1 src1;

 protected:
  template <typename SEQ, typename T>
  friend struct Sequence;
  bool Load(const Instr* i, InstrKeyValue kv) {
    if (kv == key && BASE::LoadDest(i)) {
      instr = i;
      src1.Load(i->src1);
      return true;
    }
    return false;
  }
};

template <hir::Opcode OPCODE, typename DEST, typename SRC1, typename SRC2>
struct I<OPCODE, DEST, SRC1, SRC2> : DestField<DEST> {
  typedef DestField<DEST> BASE;
  static constexpr hir::Opcode opcode = OPCODE;
  static const uint32_t key =
      InstrKey::Construct<OPCODE, DEST::key_type, SRC1::key_type,
                          SRC2::key_type>::value;
  static const KeyType dest_type = DEST::key_type;
  static const KeyType src1_type = SRC1::key_type;
  static const KeyType src2_type = SRC2::key_type;
  const Instr* instr;
  SRC1 src1;
  SRC2 src2;

 protected:
  template <typename SEQ, typename T>
  friend struct Sequence;
  bool Load(const Instr* i, InstrKeyValue kv) {
    if (kv == key && BASE::LoadDest(i)) {
      instr = i;
      src1.Load(i->src1);
      src2.Load(i->src2);
      return true;
    }
    return false;
  }
};

template <hir::Opcode OPCODE, typename DEST, typename SRC1, typename SRC2,
          typename SRC3>
struct I<OPCODE, DEST, SRC1, SRC2, SRC3> : DestField<DEST> {
  typedef DestField<DEST> BASE;
  static constexpr hir::Opcode opcode = OPCODE;
  static const uint32_t key =
      InstrKey::Construct<OPCODE, DEST::key_type, SRC1::key_type,
                          SRC2::key_type, SRC3::key_type>::value;
  static const KeyType dest_type = DEST::key_type;
  static const KeyType src1_type = SRC1::key_type;
  static const KeyType src2_type = SRC2::key_type;
  static const KeyType src3_type = SRC3::key_type;
  const Instr* instr;
  SRC1 src1;
  SRC2 src2;
  SRC3 src3;

 protected:
  template <typename SEQ, typename T>
  friend struct Sequence;
  bool Load(const Instr* i, InstrKeyValue ikey) {
    if (ikey == key && BASE::LoadDest(i)) {
      instr = i;
      src1.Load(i->src1);
      src2.Load(i->src2);
      src3.Load(i->src3);
      return true;
    }
    return false;
  }
};

// ============================================================================
// Sequence<> — base for all ARM64 instruction sequences
// ============================================================================

template <typename SEQ, typename T>
struct Sequence {
  typedef T EmitArgType;

  static constexpr uint32_t head_key() { return T::key; }

  static bool Select(A64Emitter& e, const Instr* i, InstrKeyValue ikey) {
    T args;
    if (!args.Load(i, ikey)) {
      return false;
    }
    SEQ::Emit(e, args);
    return true;
  }
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_OP_H_
