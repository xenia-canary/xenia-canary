/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_hir_builder.h"

#include <stddef.h>
#include <cmath>
#include <cstring>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/string.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
DEFINE_bool(
    break_on_unimplemented_instructions, true,
    "Break to the host debugger (or crash if no debugger attached) if an "
    "unimplemented PowerPC instruction is encountered.",
    "CPU");

namespace xe {
namespace cpu {
namespace ppc {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Label;
using xe::cpu::hir::TypeName;
using xe::cpu::hir::Value;

// The number of times each opcode has been translated.
// Accumulated across the entire run.
uint32_t opcode_translation_counts[static_cast<int>(PPCOpcode::kInvalid)] = {0};

void DumpAllOpcodeCounts() {
  StringBuffer sb;
  sb.Append("Instruction translation counts:\n");
  for (size_t i = 0; i < xe::countof(opcode_translation_counts); ++i) {
    auto opcode = static_cast<PPCOpcode>(i);
    auto& opcode_info = GetOpcodeInfo(opcode);
    auto& disasm_info = GetOpcodeDisasmInfo(opcode);
    auto translation_count = opcode_translation_counts[i];
    if (translation_count) {
      sb.AppendFormat("{:8d} : {}\n", translation_count, disasm_info.name);
    }
  }
  fprintf(stdout, "%s", sb.to_string().c_str());
  fflush(stdout);
}

PPCHIRBuilder::PPCHIRBuilder(PPCFrontend* frontend)
    : HIRBuilder(), frontend_(frontend), comment_buffer_(4096) {}

PPCHIRBuilder::~PPCHIRBuilder() = default;

PPCBuiltins* PPCHIRBuilder::builtins() const { return frontend_->builtins(); }

void PPCHIRBuilder::Reset() {
  function_ = nullptr;
  start_address_ = 0;
  instr_count_ = 0;
  instr_offset_list_ = NULL;
  label_list_ = NULL;
  with_debug_info_ = false;
  HIRBuilder::Reset();
}

bool PPCHIRBuilder::Emit(GuestFunction* function, uint32_t flags) {
  SCOPE_profile_cpu_f("cpu");

  Memory* memory = frontend_->memory();

  function_ = function;
  start_address_ = function_->address();
  // chrispy: i've seen this one happen, not sure why but i think from trying to
  // precompile twice i've also seen ones with a start and end address that are
  // the same...
  assert_true(function_->address() <= function_->end_address());
  instr_count_ = (function_->end_address() - function_->address()) / 4 + 1;

  with_debug_info_ = (flags & EMIT_DEBUG_COMMENTS) == EMIT_DEBUG_COMMENTS;
  if (with_debug_info_) {
    CommentFormat("{} fn {:08X}-{:08X} {}", function_->module()->name().c_str(),
                  function_->address(), function_->end_address(),
                  function_->name().c_str());
  }

  // Allocate offset list.
  // This is used to quickly map labels to instructions.
  // The list is built as the instructions are traversed, with the values
  // being the previous HIR Instr before the given instruction. An
  // instruction may have a label assigned to it if it hasn't been hit
  // yet.
  size_t list_size = instr_count_ * sizeof(void*);
  instr_offset_list_ = (Instr**)arena_->Alloc(list_size, alignof(void*));
  label_list_ = (Label**)arena_->Alloc(list_size, alignof(void*));
  std::memset(instr_offset_list_, 0, list_size);
  std::memset(label_list_, 0, list_size);

  // Always mark entry with label.
  label_list_[0] = NewLabel();

  uint32_t start_address = function_->address();
  uint32_t end_address = function_->end_address();
  for (uint32_t address = start_address, offset = 0; address <= end_address;
       address += 4, offset++) {
    trace_info_.dest_count = 0;
    uint32_t code =
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
    auto opcode = LookupOpcode(code);
    auto& opcode_info = GetOpcodeInfo(opcode);

    // Mark label, if we were assigned one earlier on in the walk.
    // We may still get a label, but it'll be inserted by LookupLabel
    // as needed.
    Label* label = label_list_[offset];
    if (label) {
      MarkLabel(label);
    }

    Instr* first_instr = 0;
    if (with_debug_info_) {
      if (label) {
        AnnotateLabel(address, label);
      }
      comment_buffer_.Reset();
      comment_buffer_.AppendFormat("{:08X} {:08X} ", address, code);
      DisasmPPC(address, code, &comment_buffer_);
      Comment(comment_buffer_);
      first_instr = last_instr();
    }

    // Mark source offset for debugging.
    // We could omit this if we never wanted to debug.
    SourceOffset(address);
    if (!first_instr) {
      first_instr = last_instr();
    }

    // Stash instruction offset. It's either the SOURCE_OFFSET or the COMMENT.
    instr_offset_list_[offset] = first_instr;

    if (opcode == PPCOpcode::kInvalid) {
      XELOGE("Invalid instruction {:08X} {:08X}", address, code);
      Comment("INVALID!");
      // TraceInvalidInstruction(i);
      continue;
    }
    ++opcode_translation_counts[static_cast<int>(opcode)];

    // Synchronize the PPC context as required.
    // This will ensure all registers are saved to the PPC context before this
    // instruction executes.
    if (opcode_info.type == PPCOpcodeType::kSync) {
      ContextBarrier();
    }

    MaybeBreakOnInstruction(address);

    InstrData i;
    i.address = address;
    i.code = code;
    i.opcode = opcode;
    i.opcode_info = &opcode_info;
    if (!opcode_info.emit || opcode_info.emit(*this, i)) {
      auto& disasm_info = GetOpcodeDisasmInfo(opcode);
      XELOGE(
          "Unimplemented instr {:08X} {:08X} {} - report the game to Xenia "
          "developers; to skip, disable break_on_unimplemented_instructions",
          address, code, disasm_info.name);
      Comment("UNIMPLEMENTED!");
      if (cvars::break_on_unimplemented_instructions) {
        DebugBreak();
      }
    }
  }

  if (false) {
    DumpAllOpcodeCounts();
  }

  return Finalize();
}

void PPCHIRBuilder::MaybeBreakOnInstruction(uint32_t address) {
  if (address != cvars::break_on_instruction) {
    return;
  }

  Comment("--break-on-instruction target");

  if (cvars::break_condition_gpr < 0) {
    DebugBreak();
    return;
  }

  auto left = LoadGPR(cvars::break_condition_gpr);
  auto right = LoadConstantUint64(cvars::break_condition_value);
  if (cvars::break_condition_truncate) {
    left = Truncate(left, INT32_TYPE);
    right = Truncate(right, INT32_TYPE);
  }

  auto op = cvars::break_condition_op.c_str();
  // TODO(rick): table?
  if (xe_strcasecmp(op, "eq") == 0) {
    TrapTrue(CompareEQ(left, right));
  } else if (xe_strcasecmp(op, "ne") == 0) {
    TrapTrue(CompareNE(left, right));
  } else if (xe_strcasecmp(op, "slt") == 0) {
    TrapTrue(CompareSLT(left, right));
  } else if (xe_strcasecmp(op, "sle") == 0) {
    TrapTrue(CompareSLE(left, right));
  } else if (xe_strcasecmp(op, "sgt") == 0) {
    TrapTrue(CompareSGT(left, right));
  } else if (xe_strcasecmp(op, "sge") == 0) {
    TrapTrue(CompareSGE(left, right));
  } else if (xe_strcasecmp(op, "ult") == 0) {
    TrapTrue(CompareULT(left, right));
  } else if (xe_strcasecmp(op, "ule") == 0) {
    TrapTrue(CompareULE(left, right));
  } else if (xe_strcasecmp(op, "ugt") == 0) {
    TrapTrue(CompareUGT(left, right));
  } else if (xe_strcasecmp(op, "uge") == 0) {
    TrapTrue(CompareUGE(left, right));
  } else {
    assert_always();
  }
}

void PPCHIRBuilder::AnnotateLabel(uint32_t address, Label* label) {
  // chrispy: label->name is unused, it would be nice to be able to remove the
  // field and this code
  char name_buffer[13];
  auto format_result = fmt::format_to_n(name_buffer, 12, "loc_{:08X}", address);
  name_buffer[format_result.size] = '\0';
  label->name = (char*)arena_->Alloc(sizeof(name_buffer), 1);
  memcpy(label->name, name_buffer, sizeof(name_buffer));
}

Function* PPCHIRBuilder::LookupFunction(uint32_t address) {
  return frontend_->processor()->LookupFunction(address);
}

Label* PPCHIRBuilder::LookupLabel(uint32_t address) {
  if (address < start_address_) {
    return nullptr;
  }
  size_t offset = (address - start_address_) / 4;
  if (offset >= instr_count_) {
    return nullptr;
  }
  Label* label = label_list_[offset];
  if (label) {
    return label;
  }
  // No label. If we haven't yet hit the instruction in the walk
  // then create a label. Otherwise, we must go back and insert
  // the label.
  label = NewLabel();
  label_list_[offset] = label;
  Instr* instr = instr_offset_list_[offset];
  if (instr) {
    if (instr->prev) {
      // Insert label, breaking up existing instructions.
      InsertLabel(label, instr->prev);
    } else {
      // Instruction is at the head of a block, so just add the label.
      MarkLabel(label, instr->block);
    }

    // Annotate the label, as we won't do it later.
    if (with_debug_info_) {
      AnnotateLabel(address, label);
    }
  }
  return label;
}

// Value* PPCHIRBuilder::LoadXER() {
//}
//
// void PPCHIRBuilder::StoreXER(Value* value) {
//}

Value* PPCHIRBuilder::LoadLR() {
  return LoadContext(offsetof(PPCContext, lr), INT64_TYPE);
}

void PPCHIRBuilder::StoreLR(Value* value) {
  assert_true(value->type == INT64_TYPE);
  StoreContext(offsetof(PPCContext, lr), value);

  auto& trace_reg = trace_info_.dests[trace_info_.dest_count++];
  trace_reg.reg = 64;
  trace_reg.value = value;
}

Value* PPCHIRBuilder::LoadCTR() {
  return LoadContext(offsetof(PPCContext, ctr), INT64_TYPE);
}

void PPCHIRBuilder::StoreCTR(Value* value) {
  assert_true(value->type == INT64_TYPE);
  StoreContext(offsetof(PPCContext, ctr), value);

  auto& trace_reg = trace_info_.dests[trace_info_.dest_count++];
  trace_reg.reg = 65;
  trace_reg.value = value;
}

Value* PPCHIRBuilder::LoadCR() {
  // All bits. This is expensive, but seems to be less used than the
  // field-specific LoadCR.
  Value* v = LoadCR(0);
  for (int i = 1; i <= 7; ++i) {
    v = Or(v, LoadCR(i));
  }
  return v;
}

Value* PPCHIRBuilder::LoadCR(uint32_t n) {
  // Construct the entire word of just the bits we care about.
  // This makes it easier for the optimizer to exclude things, though
  // we could be even more clever and watch sequences.
  Value* v = Shl(ZeroExtend(LoadContext(offsetof(PPCContext, cr0) + (4 * n) + 0,
                                        INT8_TYPE),
                            INT64_TYPE),
                 4 * (7 - n) + 3);
  v = Or(v, Shl(ZeroExtend(LoadContext(offsetof(PPCContext, cr0) + (4 * n) + 1,
                                       INT8_TYPE),
                           INT64_TYPE),
                4 * (7 - n) + 2));
  v = Or(v, Shl(ZeroExtend(LoadContext(offsetof(PPCContext, cr0) + (4 * n) + 2,
                                       INT8_TYPE),
                           INT64_TYPE),
                4 * (7 - n) + 1));
  v = Or(v, Shl(ZeroExtend(LoadContext(offsetof(PPCContext, cr0) + (4 * n) + 3,
                                       INT8_TYPE),
                           INT64_TYPE),
                4 * (7 - n) + 0));
  return v;
}

Value* PPCHIRBuilder::LoadCRField(uint32_t n, uint32_t bit) {
  return LoadContext(offsetof(PPCContext, cr0) + (4 * n) + bit, INT8_TYPE);
}

void PPCHIRBuilder::StoreCR(Value* value) {
  // All bits. This is expensive, but seems to be less used than the
  // field-specific StoreCR.
  for (int i = 0; i <= 7; ++i) {
    StoreCR(i, value);
  }
}

void PPCHIRBuilder::StoreCR(uint32_t n, Value* value) {
  // Pull out the bits we are interested in.
  // Optimization passes will kill any unneeded stores (mostly).
  StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 0,
               And(Truncate(Shr(value, 4 * (7 - n) + 3), INT8_TYPE),
                   LoadConstantUint8(1)));
  StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 1,
               And(Truncate(Shr(value, 4 * (7 - n) + 2), INT8_TYPE),
                   LoadConstantUint8(1)));
  StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 2,
               And(Truncate(Shr(value, 4 * (7 - n) + 1), INT8_TYPE),
                   LoadConstantUint8(1)));
  StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 3,
               And(Truncate(Shr(value, 4 * (7 - n) + 0), INT8_TYPE),
                   LoadConstantUint8(1)));
}

void PPCHIRBuilder::StoreCRField(uint32_t n, uint32_t bit, Value* value) {
  StoreContext(offsetof(PPCContext, cr0) + (4 * n) + bit, value);

  // TODO(benvanik): trace CR.
}

void PPCHIRBuilder::UpdateCR(uint32_t n, Value* lhs, bool is_signed) {
  UpdateCR(n, Truncate(lhs, INT32_TYPE), LoadZeroInt32(), is_signed);
}

void PPCHIRBuilder::UpdateCR(uint32_t n, Value* lhs, Value* rhs,
                             bool is_signed) {
  if (is_signed) {
    Value* lt = CompareSLT(lhs, rhs);
    StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 0, lt);
    Value* gt = CompareSGT(lhs, rhs);
    StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 1, gt);
  } else {
    Value* lt = CompareULT(lhs, rhs);
    StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 0, lt);
    Value* gt = CompareUGT(lhs, rhs);
    StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 1, gt);
  }
  Value* eq = CompareEQ(lhs, rhs);
  StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 2, eq);

  // A snapshot of XER[SO], so it cannot be resolved lazily at mfcr time.
  StoreContext(offsetof(PPCContext, cr0) + (4 * n) + 3,
               LoadContext(offsetof(PPCContext, xer_so), INT8_TYPE));

  // TOOD(benvanik): trace CR.
}

void PPCHIRBuilder::UpdateCR6(Value* src_value) {
  // Testing for all 1's and all 0's.
  // if (Rc) CR6 = all_equal | 0 | none_equal | 0
  // TODO(benvanik): efficient instruction?

  // chrispy: nothing seems to write cr6_1, figure out if no documented
  // instructions write anything other than 0 to it and remove these stores if
  // so
  StoreContext(offsetof(PPCContext, cr6.cr6_1), LoadZeroInt8());
  StoreContext(offsetof(PPCContext, cr6.cr6_3), LoadZeroInt8());
  StoreContext(offsetof(PPCContext, cr6.cr6_all_equal),
               IsFalse(Not(src_value)));
  StoreContext(offsetof(PPCContext, cr6.cr6_none_equal), IsFalse(src_value));

  // TOOD(benvanik): trace CR.
}

Value* PPCHIRBuilder::LoadFPSCR() {
  return LoadContext(offsetof(PPCContext, fpscr), INT32_TYPE);
}

void PPCHIRBuilder::StoreFPSCR(Value* value) {
  assert_true(value->type == INT32_TYPE);
  StoreContext(offsetof(PPCContext, fpscr), value);

  auto& trace_reg = trace_info_.dests[trace_info_.dest_count++];
  trace_reg.reg = 67;
  trace_reg.value = value;
}

// Writes FX, FEX, VX and OX, the four bits CR1 mirrors. FEX isn't derived, so
// it's always zero.
void PPCHIRBuilder::StoreFPSCRSummary(Value* vx, bool update_cr1, Value* ox,
                                      Value* raised) {
  // TODO(benvanik): detect overflow and the inexact cases; the latter needs a
  // status register read after every operation.
  /*
    chrispy: i stubbed this out at one point because all it does is waste
     memory and CPU time, however, this introduced issues with raiden
    (substitute w/ titleid later) which probably means they stash stuff in the
    fpscr?

  */
  if (!ox) {
    ox = LoadConstantInt8(0);
  }
  Value* fx = Or(vx, ox);
  if (raised) {
    fx = Or(fx, raised);
  }

  // FX and OX are sticky: they accumulate until software clears them with
  // mtfsf, mtfsfi or mtfsb0, including bits that software set itself. FEX and
  // VX are replaced by this instruction's own value. The sticky bits stay in
  // place under the mask and the new ones are ORed in, rather than read out
  // and back: for the non-recording forms the new bits are constant zero, so
  // consecutive instructions fold to one masked store instead of chaining
  // through FPSCR.
  Value* new_bits = Or(Shl(ZeroExtend(fx, INT32_TYPE), 31),
                       Or(Shl(ZeroExtend(vx, INT32_TYPE), 29),
                          Shl(ZeroExtend(ox, INT32_TYPE), 28)));
  Value* bits = Or(And(LoadFPSCR(), LoadConstantUint32(0x9FFFFFFF)), new_bits);
  StoreFPSCR(bits);

  if (update_cr1) {
    // Store into the CR1 field. CR1 copies FPSCR's bits, so FX and OX take in
    // the sticky ones too.
    Value* one = LoadConstantInt8(1);
    StoreContext(offsetof(PPCContext, cr1.cr1_fx),
                 And(Truncate(Shr(bits, 31), INT8_TYPE), one));
    StoreContext(offsetof(PPCContext, cr1.cr1_fex), LoadConstantInt8(0));
    StoreContext(offsetof(PPCContext, cr1.cr1_vx), vx);
    StoreContext(offsetof(PPCContext, cr1.cr1_ox),
                 And(Truncate(Shr(bits, 28), INT8_TYPE), one));
  }
}

void PPCHIRBuilder::ClearFPSCRExceptions(bool update_cr1) {
  StoreFPSCRSummary(LoadConstantInt8(0), update_cr1);
}

// Magnitude of a double, for classifying it without a compare against a NaN.
static Value* FpMagnitude(PPCHIRBuilder& f, Value* value) {
  return f.And(f.Cast(value, INT64_TYPE),
               f.LoadConstantUint64(0x7FFFFFFFFFFFFFFFull));
}

// An invalid operation is the only way a NaN can appear that did not come
// from an operand, and an SNaN operand is invalid however it propagates.
static Value* InvalidFromResult(PPCHIRBuilder& f, Value* result,
                                std::initializer_list<Value*> operands) {
  Value* any_nan = nullptr;
  Value* any_snan = nullptr;
  for (Value* operand : operands) {
    Value* is_nan = f.IsNan(operand);
    Value* quiet_bit = f.And(f.Cast(operand, INT64_TYPE),
                             f.LoadConstantUint64(0x0008000000000000ull));
    Value* is_snan = f.And(is_nan, f.IsFalse(quiet_bit));
    any_nan = any_nan ? f.Or(any_nan, is_nan) : is_nan;
    any_snan = any_snan ? f.Or(any_snan, is_snan) : is_snan;
  }
  return f.And(f.IsNan(result), f.Or(any_snan, f.IsFalse(any_nan)));
}

// A finite nonzero double as a mantissa in [1, 2), keeping its sign, and its
// exponent. A denormal is scaled into the normal range first, which is exact.
static Value* FpNormalize(PPCHIRBuilder& f, Value* value, Value** exponent) {
  Value* exponent_mask = f.LoadConstantUint64(0x7FF);
  Value* denormal =
      f.IsFalse(f.And(f.Shr(f.Cast(value, INT64_TYPE), 52), exponent_mask));
  Value* scaled = f.Select(
      denormal, f.Mul(value, f.LoadConstantFloat64(18446744073709551616.0)),
      value);
  Value* bits = f.Cast(scaled, INT64_TYPE);
  // 64 less for a scaled denormal
  *exponent = f.Sub(f.And(f.Shr(bits, 52), exponent_mask),
                    f.Shl(f.ZeroExtend(denormal, INT64_TYPE), 6));
  return f.Cast(f.Or(f.And(bits, f.LoadConstantUint64(0x800FFFFFFFFFFFFFull)),
                     f.LoadConstantUint64(0x3FF0000000000000ull)),
                FLOAT64_TYPE);
}

// Whether x * y differs from z, exactly, for finite x, y and z. The mantissas
// are compared with a fused multiply-subtract, which can't lose a nonzero
// difference between numbers in [1, 4), so tiny and denormal values are
// judged right too.
static Value* ProductDiffers(PPCHIRBuilder& f, Value* x, Value* y, Value* z) {
  Value* zero = f.LoadZeroFloat64();
  Value* any_zero = f.Or(f.CompareEQ(x, zero), f.CompareEQ(y, zero));
  Value* z_zero = f.CompareEQ(z, zero);
  Value* x_exponent;
  Value* y_exponent;
  Value* z_exponent;
  Value* x_mantissa = FpNormalize(f, x, &x_exponent);
  Value* y_mantissa = FpNormalize(f, y, &y_exponent);
  Value* z_mantissa = FpNormalize(f, z, &z_exponent);
  // The mantissas' product is in [1, 4), so z's exponent is theirs or one up.
  // The exponents are biased, so the sum has one bias too many.
  Value* shift = f.Sub(f.Add(z_exponent, f.LoadConstantUint64(1023)),
                       f.Add(x_exponent, y_exponent));
  Value* one_up = f.CompareEQ(shift, f.LoadConstantUint64(1));
  Value* aligned = f.Select(
      one_up, f.Mul(z_mantissa, f.LoadConstantFloat64(2.0)), z_mantissa);
  Value* differs =
      f.Or(f.And(f.IsTrue(shift), f.IsFalse(one_up)),
           f.CompareNE(f.MulSub(x_mantissa, y_mantissa, aligned), zero));
  // A zero on one side only matches a zero on the other
  return f.Select(f.Or(any_zero, z_zero), f.CompareNE(any_zero, z_zero),
                  differs);
}

// Whether a + b, for finite a and b, differs from the double `sum`. With the
// larger magnitude first, sum - larger is exact, so it gives back the smaller
// one only if the sum was exact.
static Value* SumDiffers(PPCHIRBuilder& f, Value* a, Value* b, Value* sum) {
  Value* a_larger = f.CompareUGE(FpMagnitude(f, a), FpMagnitude(f, b));
  Value* larger = f.Select(a_larger, a, b);
  Value* smaller = f.Select(a_larger, b, a);
  return f.CompareNE(f.Sub(sum, larger), smaller);
}

// The exceptions besides invalid operation, as OX and the others together.
// Overflow is an infinite result from finite operands, zero divide a zero
// divisor (or fres and frsqrte operand) with a finite nonzero dividend, and
// inexact, which takes in underflow, compares the result with the operands.
static void FpOtherExceptions(PPCHIRBuilder& f, Value* result,
                              std::initializer_list<Value*> operands,
                              PPCHIRBuilder::FPOperation operation, Value** ox,
                              Value** raised) {
  using FPOperation = PPCHIRBuilder::FPOperation;
  Value* inf = f.LoadConstantUint64(0x7FF0000000000000ull);
  Value* finite = nullptr;
  for (Value* operand : operands) {
    Value* is_finite = f.CompareULT(FpMagnitude(f, operand), inf);
    finite = finite ? f.And(finite, is_finite) : is_finite;
  }
  Value* result_magnitude = FpMagnitude(f, result);
  Value* a = *operands.begin();
  Value* b = operands.size() > 1 ? operands.begin()[1] : nullptr;
  Value* zero = f.LoadZeroFloat64();

  Value* zero_divide = nullptr;
  Value* inexact = nullptr;
  switch (operation) {
    case FPOperation::kSum: {
      // A single result is rounded again from the double sum
      Value* sum = f.Add(a, b);
      inexact = f.Or(SumDiffers(f, a, b, sum), f.CompareNE(result, sum));
      break;
    }
    case FPOperation::kDifference: {
      Value* difference = f.Sub(a, b);
      inexact = f.Or(SumDiffers(f, a, f.Neg(b), difference),
                     f.CompareNE(result, difference));
      break;
    }
    case FPOperation::kProduct:
      inexact = ProductDiffers(f, a, b, result);
      break;
    case FPOperation::kQuotient:
      zero_divide = f.And(f.CompareEQ(b, zero), f.CompareNE(a, zero));
      inexact = ProductDiffers(f, result, b, a);
      break;
    case FPOperation::kSquareRoot:
      inexact = ProductDiffers(f, result, result, a);
      break;
    case FPOperation::kRoundToSingle:
      inexact = f.CompareNE(result, a);
      break;
    case FPOperation::kEstimate:
      zero_divide = f.CompareEQ(a, zero);
      break;
    default:
      break;
  }

  Value* overflow = f.And(finite, f.CompareEQ(result_magnitude, inf));
  if (zero_divide) {
    zero_divide = f.And(finite, zero_divide);
    overflow = f.And(overflow, f.IsFalse(zero_divide));
  }
  *ox = overflow;
  *raised = zero_divide;
  if (inexact) {
    // A NaN or infinite result from finite operands is invalid or overflow
    inexact =
        f.And(f.And(finite, f.CompareULT(result_magnitude, inf)), inexact);
    *raised = *raised ? f.Or(*raised, inexact) : inexact;
  }
}

void PPCHIRBuilder::UpdateFPSCR(Value* result,
                                std::initializer_list<Value*> operands,
                                bool update_cr1, Value* suppress,
                                FPOperation operation) {
  if (!update_cr1) {
    ClearFPSCRExceptions(false);
    return;
  }

  Value* vx = InvalidFromResult(*this, result, operands);
  Value* ox = nullptr;
  Value* raised = nullptr;
  if (operation != FPOperation::kOther) {
    FpOtherExceptions(*this, result, operands, operation, &ox, &raised);
  }
  if (suppress) {
    Value* keep = IsFalse(suppress);
    vx = And(vx, keep);
    if (ox) {
      ox = And(ox, keep);
    }
    if (raised) {
      raised = And(raised, keep);
    }
  }

  StoreFPSCRSummary(vx, true, ox, raised);
}

// x + y as the double sum and its rounding error, exactly. Needs round to
// nearest.
static void TwoSum(PPCHIRBuilder& f, Value* x, Value* y, Value** sum,
                   Value** error) {
  Value* s = f.Add(x, y);
  Value* y_part = f.Sub(s, x);
  Value* x_part = f.Sub(s, y_part);
  *error = f.Add(f.Sub(x, x_part), f.Sub(y, y_part));
  *sum = s;
}

// Whether a x c + y, for finite operands, differs from the double `fused`,
// finite too. The fused multiply-add's error is a sum of two doubles, which
// ErrFma (Boldo and Muller) finds exactly in round to nearest, as long as
// a x c neither underflows nor overflows. So a tiny product is scaled up by
// 2^1100 and a huge one down by 2^-128, with y and the result alongside, which
// keeps the comparison exact.
static Value* FusedDiffers(PPCHIRBuilder& f, Value* a, Value* c, Value* y,
                           Value* fused) {
  Value* zero = f.LoadZeroFloat64();
  Value* up = f.LoadConstantFloat64(std::ldexp(1.0, 550));
  Value* down = f.LoadConstantFloat64(std::ldexp(1.0, -128));
  Value* product_magnitude = FpMagnitude(f, f.Mul(a, c));
  // Below 2^-900 and from 2^1020 up
  Value* tiny = f.CompareULT(product_magnitude,
                             f.LoadConstantUint64(0x07B0000000000000ull));
  Value* huge = f.CompareUGE(product_magnitude,
                             f.LoadConstantUint64(0x7FB0000000000000ull));
  Value* a_scaled =
      f.Select(tiny, f.Mul(a, up), f.Select(huge, f.Mul(a, down), a));
  Value* c_scaled = f.Select(tiny, f.Mul(c, up), c);
  Value* y_scaled = f.Select(tiny, f.Mul(f.Mul(y, up), up),
                             f.Select(huge, f.Mul(y, down), y));
  Value* fused_scaled = f.Select(tiny, f.Mul(f.Mul(fused, up), up),
                                 f.Select(huge, f.Mul(fused, down), fused));
  // A nonzero product under 2^-900 can't add exactly to a y from 2^-100 up,
  // which is also too big to scale. Next to a product from 2^1020 up, a y that
  // loses bits scaled down is too small to add exactly.
  Value* product_nonzero = f.And(f.CompareNE(a, zero), f.CompareNE(c, zero));
  Value* y_big = f.CompareUGE(FpMagnitude(f, y),
                              f.LoadConstantUint64(0x39B0000000000000ull));
  Value* y_lost = f.CompareNE(
      f.Mul(y_scaled, f.LoadConstantFloat64(std::ldexp(1.0, 128))), y);
  Value* known =
      f.Or(f.And(tiny, f.And(product_nonzero, y_big)), f.And(huge, y_lost));

  // ErrFma: a x c + y = fused + gamma + alpha_error, exactly
  Value* product = f.Mul(a_scaled, c_scaled);
  Value* product_error = f.MulSub(a_scaled, c_scaled, product);
  Value* alpha;
  Value* alpha_error;
  TwoSum(f, y_scaled, product_error, &alpha, &alpha_error);
  Value* beta;
  Value* beta_error;
  TwoSum(f, product, alpha, &beta, &beta_error);
  Value* gamma = f.Add(f.Sub(beta, fused_scaled), beta_error);
  // With both, the scaled y overflows; the product is then 0, which is exact
  Value* scaled = f.IsFalse(f.And(tiny, y_big));
  return f.Or(known, f.And(scaled, f.CompareNE(gamma, f.Neg(alpha_error))));
}

void PPCHIRBuilder::UpdateFPSCRForMultiplyAdd(Value* result, Value* a, Value* c,
                                              Value* b, bool subtract,
                                              bool negate, bool update_cr1,
                                              Value* suppress) {
  if (!update_cr1) {
    ClearFPSCRExceptions(false);
    return;
  }

  // 0 x inf is invalid whatever the addend, but with a QNaN addend the result
  // is that QNaN, which reads as propagated, so test the multiplicands.
  Value* inf = LoadConstantUint64(0x7FF0000000000000ull);
  Value* a_magnitude = FpMagnitude(*this, a);
  Value* c_magnitude = FpMagnitude(*this, c);
  Value* zero_times_inf =
      Or(And(IsFalse(a_magnitude), CompareEQ(c_magnitude, inf)),
         And(CompareEQ(a_magnitude, inf), IsFalse(c_magnitude)));
  Value* vx = Or(InvalidFromResult(*this, result, {a, c, b}), zero_times_inf);

  // Overflow is an infinite result from finite operands. Otherwise the result
  // is inexact if the fused double is, or if rounding to single changed it.
  Value* finite =
      And(And(CompareULT(a_magnitude, inf), CompareULT(c_magnitude, inf)),
          CompareULT(FpMagnitude(*this, b), inf));
  Value* result_magnitude = FpMagnitude(*this, result);
  Value* ox = And(finite, CompareEQ(result_magnitude, inf));
  Value* y = subtract ? Neg(b) : b;
  Value* fused = MulAdd(a, c, y);
  Value* unnegated = negate ? Neg(result) : result;
  Value* inexact =
      And(And(finite, CompareULT(result_magnitude, inf)),
          Or(FusedDiffers(*this, a, c, y, fused), CompareNE(unnegated, fused)));
  if (suppress) {
    Value* keep = IsFalse(suppress);
    vx = And(vx, keep);
    ox = And(ox, keep);
    inexact = And(inexact, keep);
  }

  StoreFPSCRSummary(vx, true, ox, inexact);
}

// A denormalized double operand makes the single-precision arithmetic answer
// with the default QNaN and raise nothing at all. Divide and square root do not
// do it, and a NaN or infinite operand takes precedence, so this asks for every
// operand finite and at least one of them denormal.
Value* PPCHIRBuilder::SingleDenormalOperand(
    std::initializer_list<Value*> operands) {
  Value* any_denormal = nullptr;
  Value* any_nonfinite = nullptr;
  for (Value* operand : operands) {
    Value* magnitude = FpMagnitude(*this, operand);
    // Subtracting one wraps a zero operand up to the top, which is what keeps
    // zero from reading as denormal.
    Value* is_denormal = CompareULT(Sub(magnitude, LoadConstantUint64(1)),
                                    LoadConstantUint64(0x000FFFFFFFFFFFFFull));
    Value* is_nonfinite =
        CompareUGE(magnitude, LoadConstantUint64(0x7FF0000000000000ull));
    any_denormal = any_denormal ? Or(any_denormal, is_denormal) : is_denormal;
    any_nonfinite =
        any_nonfinite ? Or(any_nonfinite, is_nonfinite) : is_nonfinite;
  }
  return And(any_denormal, IsFalse(any_nonfinite));
}

Value* PPCHIRBuilder::ApplySingleDenormalOperand(Value* quirk, Value* result) {
  return Select(quirk,
                Cast(LoadConstantUint64(0x7FF8000000000000ull), FLOAT64_TYPE),
                result);
}

void PPCHIRBuilder::UpdateFPSCRForConvertToInteger(Value* b,
                                                   RoundMode round_mode,
                                                   bool to_int64,
                                                   bool update_cr1) {
  if (!update_cr1) {
    ClearFPSCRExceptions(false);
    return;
  }
  // Invalid is what the target range cannot hold, measured on the rounded
  // value: truncating 2^31 - 0.5 stays in range where rounding it up does
  // not. The limit is the power of two just past the maximum, which leaves
  // the minimum itself in range.
  Value* rounded = Round(b, round_mode);
  double limit = to_int64 ? 9223372036854775808.0 : 2147483648.0;
  Value* out_of_range = Or(CompareSGE(rounded, LoadConstantFloat64(limit)),
                           CompareSLT(rounded, LoadConstantFloat64(-limit)));
  Value* invalid = Or(IsNan(b), out_of_range);
  // Otherwise it's inexact if rounding changed it
  Value* inexact = And(IsFalse(invalid), CompareNE(rounded, b));
  StoreFPSCRSummary(invalid, true, nullptr, inexact);
}

void PPCHIRBuilder::CopyFPSCRToCR1() {
  // Pull out of FPSCR.
  Value* fpscr = LoadFPSCR();
  StoreContext(offsetof(PPCContext, cr1.cr1_fx),
               And(Truncate(Shr(fpscr, 31), INT8_TYPE), LoadConstantInt8(1)));
  StoreContext(offsetof(PPCContext, cr1.cr1_fex),
               And(Truncate(Shr(fpscr, 30), INT8_TYPE), LoadConstantInt8(1)));
  StoreContext(offsetof(PPCContext, cr1.cr1_vx),
               And(Truncate(Shr(fpscr, 29), INT8_TYPE), LoadConstantInt8(1)));
  StoreContext(offsetof(PPCContext, cr1.cr1_ox),
               And(Truncate(Shr(fpscr, 28), INT8_TYPE), LoadConstantInt8(1)));
}

// SO is bit 31, OV bit 30, CA bit 29, with no byte count modelled.
Value* PPCHIRBuilder::LoadXER() {
  Value* v = Shl(ZeroExtend(LoadCA(), INT64_TYPE), 29);
  v = Or(v, Shl(ZeroExtend(LoadContext(offsetof(PPCContext, xer_ov), INT8_TYPE),
                           INT64_TYPE),
                30));
  v = Or(v, Shl(ZeroExtend(LoadContext(offsetof(PPCContext, xer_so), INT8_TYPE),
                           INT64_TYPE),
                31));
  return Or(v, LoadXERCount());
}

void PPCHIRBuilder::StoreXER(Value* value) {
  StoreCA(Truncate(And(Shr(value, 29), LoadConstantInt64(1)), INT8_TYPE));
  StoreContext(offsetof(PPCContext, xer_ov),
               Truncate(And(Shr(value, 30), LoadConstantInt64(1)), INT8_TYPE));
  StoreContext(offsetof(PPCContext, xer_so),
               Truncate(And(Shr(value, 31), LoadConstantInt64(1)), INT8_TYPE));
  StoreContext(offsetof(PPCContext, xer_count),
               Truncate(And(value, LoadConstantInt64(0x7F)), INT8_TYPE));
}

Value* PPCHIRBuilder::LoadXERCount() {
  return ZeroExtend(LoadContext(offsetof(PPCContext, xer_count), INT8_TYPE),
                    INT64_TYPE);
}

void PPCHIRBuilder::StoreOV(Value* value) {
  assert_true(value->type == INT8_TYPE);
  StoreContext(offsetof(PPCContext, xer_ov), value);
  // Sticky until mtxer or mcrxr clears it.
  StoreContext(offsetof(PPCContext, xer_so),
               Or(LoadContext(offsetof(PPCContext, xer_so), INT8_TYPE), value));
}

Value* PPCHIRBuilder::LoadCA() {
  return LoadContext(offsetof(PPCContext, xer_ca), INT8_TYPE);
}

void PPCHIRBuilder::StoreCA(Value* value) {
  assert_true(value->type == INT8_TYPE);
  StoreContext(offsetof(PPCContext, xer_ca), value);

  auto& trace_reg = trace_info_.dests[trace_info_.dest_count++];
  trace_reg.reg = 66;
  trace_reg.value = value;
}

Value* PPCHIRBuilder::LoadSAT() {
  return LoadContext(offsetof(PPCContext, vscr_sat), INT8_TYPE);
}

void PPCHIRBuilder::StoreSAT(Value* value) {
  value = Truncate(value, INT8_TYPE);
  StoreContext(offsetof(PPCContext, vscr_sat), value);

  auto& trace_reg = trace_info_.dests[trace_info_.dest_count++];
  trace_reg.reg = 44;
  trace_reg.value = value;
}

Value* PPCHIRBuilder::LoadGPR(uint32_t reg) {
  return LoadContext(offsetof(PPCContext, r) + reg * 8, INT64_TYPE);
}

void PPCHIRBuilder::StoreGPR(uint32_t reg, Value* value) {
  assert_true(value->type == INT64_TYPE);
  StoreContext(offsetof(PPCContext, r) + reg * 8, value);

  auto& trace_reg = trace_info_.dests[trace_info_.dest_count++];
  trace_reg.reg = reg;
  trace_reg.value = value;
}

Value* PPCHIRBuilder::LoadFPR(uint32_t reg) {
  return LoadContext(offsetof(PPCContext, f) + reg * 8, FLOAT64_TYPE);
}

void PPCHIRBuilder::StoreFPR(uint32_t reg, Value* value) {
  assert_true(value->type == FLOAT64_TYPE);
  StoreContext(offsetof(PPCContext, f) + reg * 8, value);

  auto& trace_reg = trace_info_.dests[trace_info_.dest_count++];
  trace_reg.reg = reg + 32;
  trace_reg.value = value;
}

Value* PPCHIRBuilder::LoadVR(uint32_t reg) {
  return LoadContext(offsetof(PPCContext, v) + reg * 16, VEC128_TYPE);
}

void PPCHIRBuilder::StoreVR(uint32_t reg, Value* value) {
  assert_true(value->type == VEC128_TYPE);
  StoreContext(offsetof(PPCContext, v) + reg * 16, value);

  auto& trace_reg = trace_info_.dests[trace_info_.dest_count++];
  trace_reg.reg = 128 + reg;
  trace_reg.value = value;
}

void PPCHIRBuilder::SetReturnAddress(Value* value) {
  /*
     Record the address as being a possible target of a return. This is
     needed for longjmp emulation. See x64_emitter.cc's ResolveFunction
  */
  Module* mod = this->function_->module();
  if (value && value->IsConstant()) {
    if (mod) {
      XexModule* xexmod = dynamic_cast<XexModule*>(mod);
      if (xexmod) {
        auto flags = xexmod->GetInstructionAddressFlags(value->AsUint32());
        if (flags) {
          flags->is_return_site = true;
        }
      }
    }
  }

  HIRBuilder::SetReturnAddress(value);
}
}  // namespace ppc
}  // namespace cpu
}  // namespace xe
