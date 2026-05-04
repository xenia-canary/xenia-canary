/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/hir/instr.h"

#include "xenia/cpu/hir/block.h"
#include "xenia/cpu/hir/hir_builder.h"
namespace xe {
namespace cpu {
namespace hir {
void Instr::set_srcN(Value* value, uint32_t idx) {
  if (srcs[idx].value == value) {
    return;
  }
  if (srcs_use[idx]) {
    srcs[idx].value->RemoveUse(srcs_use[idx]);
  }
  srcs[idx].value = value;
  srcs_use[idx] = value ? value->AddUse(block->arena, this) : nullptr;
}

void Instr::MoveBefore(Instr* other) {
  if (next == other) {
    return;
  }

  // Remove from current location.
  if (prev) {
    prev->next = next;
  } else {
    block->instr_head = next;
  }
  if (next) {
    next->prev = prev;
  } else {
    block->instr_tail = prev;
  }

  // Insert into new location.
  block = other->block;
  next = other;
  prev = other->prev;
  other->prev = this;
  if (prev) {
    prev->next = this;
  }
  if (other == block->instr_head) {
    block->instr_head = this;
  }
}

void Instr::Replace(const OpcodeInfo* new_opcode, uint16_t new_flags) {
  opcode = new_opcode;
  flags = new_flags;

  if (src1_use) {
    src1.value->RemoveUse(src1_use);
    src1.value = NULL;
    // src1_use = NULL;
  }
  if (src2_use) {
    src2.value->RemoveUse(src2_use);
    src2.value = NULL;
    // src2_use = NULL;
  }
  if (src3_use) {
    src3.value->RemoveUse(src3_use);
    src3.value = NULL;
    // src3_use = NULL;
  }

  if (src1_use) {
    HIRBuilder::GetCurrent()->DeallocateUse(src1_use);
    src1_use = nullptr;
  }
  if (src2_use) {
    HIRBuilder::GetCurrent()->DeallocateUse(src2_use);
    src2_use = nullptr;
  }

  if (src3_use) {
    HIRBuilder::GetCurrent()->DeallocateUse(src3_use);
    src3_use = nullptr;
  }
}

void Instr::UnlinkAndNOP() {
  // Remove all srcs/dest.
  Replace(&OPCODE_NOP_info, 0);

  if (prev) {
    prev->next = next;
  } else {
    block->instr_head = next;
  }
  if (next) {
    next->prev = prev;
  } else {
    block->instr_tail = prev;
  }
}

void Instr::Deallocate() {
  HIRBuilder::GetCurrent()->DeallocateInstruction(this);
}
Instr* Instr::GetDestDefSkipAssigns() {
  Instr* current_def = this;

  while (current_def->opcode == &OPCODE_ASSIGN_info) {
    Instr* next_def = current_def->src1.value->def;

    if (!next_def) {
      return nullptr;
    }

    current_def = next_def;
  }
  return current_def;
}
Instr* Instr::GetDestDefTunnelMovs(unsigned int* tunnel_flags) {
  unsigned int traversed_types = 0;
  unsigned int in_flags = *tunnel_flags;
  Instr* current_def = this;

  while (true) {
    Opcode op = current_def->opcode->num;

    switch (op) {
      case OPCODE_ASSIGN: {
        if ((in_flags & MOVTUNNEL_ASSIGNS)) {
          current_def = current_def->src1.value->def;
          traversed_types |= MOVTUNNEL_ASSIGNS;

        } else {
          goto exit_loop;
        }
        break;
      }
      case OPCODE_ZERO_EXTEND: {
        if ((in_flags & MOVTUNNEL_MOVZX)) {
          current_def = current_def->src1.value->def;
          traversed_types |= MOVTUNNEL_MOVZX;

        } else {
          goto exit_loop;
        }
        break;
      }
      case OPCODE_SIGN_EXTEND: {
        if ((in_flags & MOVTUNNEL_MOVSX)) {
          current_def = current_def->src1.value->def;
          traversed_types |= MOVTUNNEL_MOVSX;

        } else {
          goto exit_loop;
        }
        break;
      }
      case OPCODE_TRUNCATE: {
        if ((in_flags & MOVTUNNEL_TRUNCATE)) {
          current_def = current_def->src1.value->def;
          traversed_types |= MOVTUNNEL_TRUNCATE;

        } else {
          goto exit_loop;
        }
        break;
      }
      case OPCODE_AND: {
        if ((in_flags & MOVTUNNEL_AND32FF)) {
          auto [constant, nonconst] =
              current_def->BinaryValueArrangeAsConstAndVar();
          if (!constant || constant->AsUint64() != 0xFFFFFFFF) {
            goto exit_loop;
          }
          current_def = nonconst->def;
          traversed_types |= MOVTUNNEL_AND32FF;

        } else {
          goto exit_loop;
        }
        break;
      }
      default:
        goto exit_loop;
    }
    if (!current_def) {
      goto exit_loop;
    }
  }
exit_loop:
  *tunnel_flags = traversed_types;
  return current_def;
}
bool Instr::IsFake() const {
  Opcode num = opcode->num;
  switch (num) {
    case OPCODE_NOP:
    case OPCODE_COMMENT:
    case OPCODE_CONTEXT_BARRIER:
    case OPCODE_SOURCE_OFFSET:
#ifdef __APPLE__
    case OPCODE_DEBUG_BREAK:
    case OPCODE_DEBUG_BREAK_TRUE:
    case OPCODE_TRAP:
    case OPCODE_TRAP_TRUE:
    case OPCODE_CALL:
    case OPCODE_CALL_TRUE:
    case OPCODE_CALL_INDIRECT:
    case OPCODE_CALL_INDIRECT_TRUE:
    case OPCODE_CALL_EXTERN:
    case OPCODE_RETURN:
    case OPCODE_RETURN_TRUE:
    case OPCODE_BRANCH:
    case OPCODE_BRANCH_TRUE:
    case OPCODE_BRANCH_FALSE:
    case OPCODE_SET_RETURN_ADDRESS:
    case OPCODE_ASSIGN:
    case OPCODE_CAST:
    case OPCODE_ZERO_EXTEND:
    case OPCODE_SIGN_EXTEND:
    case OPCODE_TRUNCATE:
    case OPCODE_CONVERT:
    case OPCODE_ROUND:
    case OPCODE_VECTOR_CONVERT_I2F:
    case OPCODE_VECTOR_CONVERT_F2I:
    case OPCODE_LOAD_VECTOR_SHL:
    case OPCODE_LOAD_VECTOR_SHR:
    case OPCODE_LOAD_CLOCK:
    case OPCODE_LOAD_LOCAL:
    case OPCODE_STORE_LOCAL:
    case OPCODE_LOAD_CONTEXT:
    case OPCODE_STORE_CONTEXT:
    case OPCODE_LOAD_MMIO:
    case OPCODE_STORE_MMIO:
    case OPCODE_LOAD_OFFSET:
    case OPCODE_STORE_OFFSET:
    case OPCODE_LOAD:
    case OPCODE_STORE:
    case OPCODE_LVL:
    case OPCODE_LVR:
    case OPCODE_STVL:
    case OPCODE_STVR:
    case OPCODE_MEMSET:
    case OPCODE_CACHE_CONTROL:
    case OPCODE_MEMORY_BARRIER:
    case OPCODE_MAX:
    case OPCODE_VECTOR_MAX:
    case OPCODE_MIN:
    case OPCODE_VECTOR_MIN:
    case OPCODE_SELECT:
    case OPCODE_IS_NAN:
    case OPCODE_COMPARE_EQ:
    case OPCODE_COMPARE_NE:
    case OPCODE_COMPARE_SLT:
    case OPCODE_COMPARE_SLE:
    case OPCODE_COMPARE_ULT:
    case OPCODE_COMPARE_ULE:
    case OPCODE_COMPARE_SGT:
    case OPCODE_COMPARE_SGE:
    case OPCODE_COMPARE_UGT:
    case OPCODE_COMPARE_UGE:
    case OPCODE_DID_SATURATE:
    case OPCODE_VECTOR_COMPARE_EQ:
    case OPCODE_VECTOR_COMPARE_SGT:
    case OPCODE_VECTOR_COMPARE_SGE:
    case OPCODE_VECTOR_COMPARE_UGT:
    case OPCODE_VECTOR_COMPARE_UGE:
    case OPCODE_ADD:
    case OPCODE_ADD_CARRY:
    case OPCODE_VECTOR_ADD:
    case OPCODE_SUB:
    case OPCODE_VECTOR_SUB:
    case OPCODE_MUL:
    case OPCODE_MUL_HI:
    case OPCODE_DIV:
    case OPCODE_MUL_ADD:
    case OPCODE_MUL_SUB:
    case OPCODE_NEG:
    case OPCODE_ABS:
    case OPCODE_SQRT:
    case OPCODE_RSQRT:
    case OPCODE_RECIP:
    case OPCODE_POW2:
    case OPCODE_LOG2:
    case OPCODE_DOT_PRODUCT_3:
    case OPCODE_DOT_PRODUCT_4:
    case OPCODE_AND:
    case OPCODE_AND_NOT:
    case OPCODE_OR:
    case OPCODE_XOR:
    case OPCODE_NOT:
    case OPCODE_SHL:
    case OPCODE_SHR:
    case OPCODE_SHA:
    case OPCODE_VECTOR_SHA:
    case OPCODE_VECTOR_SHL:
    case OPCODE_VECTOR_SHR:
    case OPCODE_VECTOR_ROTATE_LEFT:
    case OPCODE_VECTOR_AVERAGE:
    case OPCODE_BYTE_SWAP:
    case OPCODE_CNTLZ:
    case OPCODE_INSERT:
    case OPCODE_EXTRACT:
    case OPCODE_SPLAT:
    case OPCODE_PERMUTE:
    case OPCODE_SWIZZLE:
    case OPCODE_PACK:
    case OPCODE_UNPACK:
    case OPCODE_ATOMIC_EXCHANGE:
    case OPCODE_ATOMIC_COMPARE_EXCHANGE:
    case OPCODE_SET_ROUNDING_MODE:
    case OPCODE_VECTOR_DENORMFLUSH:
    case OPCODE_ROTATE_LEFT:
    case OPCODE_TO_SINGLE:
    case OPCODE_SET_NJM:
    case OPCODE_DELAY_EXECUTION:
    case OPCODE_RESERVED_LOAD:
    case OPCODE_RESERVED_STORE:
    case __OPCODE_MAX_VALUE:
#endif
      return true;
  }
  return false;
}

const Instr* Instr::GetNonFakePrev() const {
  const Instr* curr = prev;

  while (curr && curr->IsFake()) {
    curr = curr->prev;
  }
  return curr;
}

uint32_t Instr::GuestAddressFor() const {
  Instr* srch = prev;

  while (srch) {
    if (srch->GetOpcodeNum() == OPCODE_SOURCE_OFFSET) {
      return (uint32_t)srch->src1.offset;
    }
    srch = srch->prev;
  }

  return 0;  // eek.
}
bool Instr::AllScalarIntegral() {
  bool result = true;
  if (dest) {
    if (!IsScalarIntegralType(dest->type)) {
      return false;
    }
  }

  VisitValueOperands([&result](Value* v, uint32_t idx) {
    result = result && IsScalarIntegralType(v->type);
  });
  return result;
}
}  // namespace hir
}  // namespace cpu
}  // namespace xe
