#include "repetitive_computation_merger_pass.h"
#include <bitset>
#include "xenia/cpu/ppc/ppc_context.h"
#if !XE_PLATFORM_WIN32
#include <climits>
#endif

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {
using namespace xe::cpu::hir;
static bool didit = false;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::hir::Value;

using ppc_ctx_vset_underlying_t = std::bitset<sizeof(ppc::PPCContext)>;

struct alignas(__m256i) ppc_ctx_vset_t : public ppc_ctx_vset_underlying_t {
  constexpr ppc_ctx_vset_t() : ppc_ctx_vset_underlying_t(), pad(){};

  char pad[sizeof(ppc_ctx_vset_underlying_t) % sizeof(__m256i)];
};

#ifdef _MSC_VER

#define add_ppc_offset(field)                         \
  __builtin_offsetof(ppc::PPCContext, field),         \
      __builtin_offsetof(ppc::PPCContext, field) + 1, \
      __builtin_offsetof(ppc::PPCContext, field) + 2, \
      __builtin_offsetof(ppc::PPCContext, field) + 3
#else
#define add_ppc_offset(field) offsetof(ppc::PPCContext, field)
#endif

constexpr unsigned bitset_cregs[] = {add_ppc_offset(cr0), add_ppc_offset(cr1),
                                     add_ppc_offset(cr2), add_ppc_offset(cr3),
                                     add_ppc_offset(cr4), add_ppc_offset(cr5),
                                     add_ppc_offset(cr6), add_ppc_offset(cr7)};

RepetitiveComputationMergerPass::RepetitiveComputationMergerPass() {}
RepetitiveComputationMergerPass::~RepetitiveComputationMergerPass() {}

static Instr* create_instr(hir::HIRBuilder* builder, hir::Block* blk,
                           const hir::OpcodeInfo* opcode, unsigned flags,
                           Value* dest) {
  Instr* instr = builder->arena()->Alloc<Instr>();
  instr->next = NULL;
  instr->prev = nullptr;

  instr->ordinal = UINT32_MAX;
  instr->block = blk;
  instr->opcode = opcode;
  instr->flags = flags;
  instr->dest = dest;
  instr->src1.value = instr->src2.value = instr->src3.value = NULL;
  instr->src1_use = instr->src2_use = instr->src3_use = NULL;
  if (dest) {
    dest->def = instr;
  }
  return instr;
}

static uint64_t extract_constant(Value* v) {
  switch (v->type) {
    case INT8_TYPE:
      return v->constant.u8;
    case INT16_TYPE:
      return v->constant.u16;
    case INT32_TYPE:
      return v->constant.u32;
    case INT64_TYPE:
      return v->constant.u64;
    default:
      return 0;
  }
}

static Instr* find_next_use(Value* val, hir::Block* blk, Instr* after,
                            Instr* end = nullptr) {
  if (!after || after->next == end) {
    return nullptr;
  }

  for (Instr* pos = after->next; pos != end; pos = pos->next) {
    if (pos->opcode->flags & OPCODE_FLAG_VOLATILE) return nullptr;
    if (pos->src1.value == val || pos->src2.value == val ||
        pos->src3.value == val) {
      return pos;
    }
  }
  return nullptr;
}

static Instr* find_next_use(Instr::Op& val, hir::Block* blk, Instr* after,
                            Instr* end = nullptr) {
  return find_next_use(val.value, blk, after, end);
}

static Instr* get_solo_use(Value* val) {
  if (!val || !val->use_head || val->use_head->next) {
    return nullptr;
  }
  return val->use_head->instr;
}

static void insert_after(Instr* i, Instr* after) {
  after->prev = i;
  after->next = i->next;
  i->next = after;
  if (after->next) {
    after->next->prev = after;
  }
}

static bool is_before_in_block(Instr* first, Instr* second, hir::Block* blk) {
  for (auto s = blk->instr_head; s; s = s->next) {
    if (s == first) {
      return true;
    } else if (s == second) {
      return false;
    }
  }
  return false;
}

static unsigned size_for_typename(TypeName type) {
  switch (type) {
    case INT8_TYPE:
      return 1;
    case INT16_TYPE:
      return 2;
    case INT32_TYPE:
      return 4;
    case INT64_TYPE:
      return 8;
    case FLOAT32_TYPE:
      return 4;
    case FLOAT64_TYPE:
      return 8;
    case VEC128_TYPE:
      return 16;
    default:
      xenia_assert(false);
      return 0;
  }
}

static Value* constant_for_type(HIRBuilder* builder, uint64_t v,
                                TypeName type) {
  switch (type) {
    case INT8_TYPE:
      return builder->LoadConstantUint8((uint8_t)v);
    case INT16_TYPE:
      return builder->LoadConstantUint16((uint16_t)v);
    case INT32_TYPE:
      return builder->LoadConstantUint32((uint32_t)v);
    case INT64_TYPE:
      return builder->LoadConstantUint64((uint64_t)v);
    default:
      return nullptr;
  }
}

static unsigned bitsize_for_typename(TypeName type) {
  return size_for_typename(type) * CHAR_BIT;
}

static unsigned highbit_for_typename(TypeName type) {
  return bitsize_for_typename(type) - 1;
}

static uint64_t mask_for_typename(TypeName type) {
  unsigned v = size_for_typename(type);
  xenia_assert(v <= 8);

  return (1ULL << ((CHAR_BIT * v))) - 1;
}

static bool is_const_x(Value* v, uint64_t val) {
  return v->IsConstant() && extract_constant(v) == val;
}

static bool is_const_x(const Instr::Op& v, uint64_t val) {
  return is_const_x(v.value, val);
}

static bool is_op(Instr* insn, Opcode opcode) {
  return insn && insn->opcode && insn->opcode->num == opcode;
}

static bool has_any_intervening_memop(Instr* start, Instr* end) {
  for (auto pos = start->next; pos != end; pos = pos->next) {
    if (pos->opcode->flags & OPCODE_FLAG_MEMORY) return true;
  }
  return false;
}
static bool has_any_intervening_store(Instr* start, Instr* end) {
  for (auto pos = start->next; pos != end; pos = pos->next) {
    if (is_op(pos, OPCODE_STORE)) return true;
  }
  return false;
}
static bool is_rvalue(const Value* v) { return v && v->local_slot == nullptr; }
static bool is_rvalue(const Instr::Op& v) {
  return v.value && v.value->local_slot == nullptr;
}

static void make_assignment(Instr* insn, Value* from) {
  insn->Replace(&OPCODE_ASSIGN_info, 0);
  insn->set_src1(from);
}

static void make_assignment(Instr* insn, Instr::Op& from) {
  make_assignment(insn, from.value);
}

static void add_store_to_bitset(ppc_ctx_vset_t* out, Instr* insn) {
  out->set(insn->src1.offset);
}

static bool load_target_in_bitset(const ppc_ctx_vset_t* to_test, Instr* insn) {
  if (!is_op(insn, OPCODE_LOAD_CONTEXT)) {
    return false;
  }
  return to_test->test(insn->src1.offset);
}

static bool store_target_in_bitset(const ppc_ctx_vset_t* to_test, Instr* insn) {
  if (!is_op(insn, OPCODE_STORE_CONTEXT)) {
    return false;
  }
  return to_test->test(insn->src1.offset);
}
static bool has_any_creg(const ppc_ctx_vset_t* b) {
  for (auto&& bt : bitset_cregs) {
    if (b->test(bt)) return true;
  }
  return false;
}
static Instr* find_next_local_store(Value* slot, hir::Block* blk, Instr* after,
                                    Instr* end = nullptr) {
  if (!after || after->next == end) {
    return nullptr;
  }

  for (Instr* pos = after->next; pos != end; pos = pos->next) {
    if (is_op(pos, OPCODE_STORE_LOCAL) && pos->src1.value == slot) {
      return pos;
    }
  }

  return nullptr;
}

static bool is_like_assign(Instr* insn) {
  return is_op(insn, OPCODE_ASSIGN) || is_op(insn, OPCODE_SIGN_EXTEND) ||
         is_op(insn, OPCODE_ZERO_EXTEND);
}

static bool produces_boolean(Instr* i) {
  constexpr Opcode opcodes[] = {
      OPCODE_IS_FALSE,     OPCODE_IS_TRUE,     OPCODE_COMPARE_EQ,
      OPCODE_COMPARE_NE,   OPCODE_COMPARE_SLT, OPCODE_COMPARE_SLE,
      OPCODE_COMPARE_SGT,  OPCODE_COMPARE_SGE, OPCODE_COMPARE_ULT,
      OPCODE_COMPARE_ULE,  OPCODE_COMPARE_UGT, OPCODE_COMPARE_UGE,
      OPCODE_DID_SATURATE,

  };
  for (auto&& op : opcodes) {
    if (is_op(i, op)) return true;
  }
  return false;
}

static void replace_uses(HIRBuilder* builder, Value* to_replace, Value* with) {
  for (auto start = to_replace->use_head; start; start = start->next) {
    auto i = start->instr;

    if (i->src1.value == to_replace) {
      i->set_src1(with);
    }
    if (i->src2.value == to_replace) {
      i->set_src2(with);
    }
    if (i->src3.value == to_replace) {
      i->set_src3(with);
    }
  }
}

static bool is_any_call(Instr* i) {
  return is_op(i, OPCODE_CALL) || is_op(i, OPCODE_CALL_EXTERN) ||
         is_op(i, OPCODE_CALL_INDIRECT) ||
         is_op(i, OPCODE_CALL_INDIRECT_TRUE) || is_op(i, OPCODE_CALL_TRUE) ||
         is_op(i, OPCODE_DEBUG_BREAK) || is_op(i, OPCODE_DEBUG_BREAK_TRUE) ||
         is_op(i, OPCODE_RETURN);
}

static void make_nop(Instr* i) {
  i->Replace(&OPCODE_NOP_info, 0);
  // i->Remove();
}

static Instr* find_next_local_load(Value* slot, hir::Block* blk, Instr* after,
                                   Instr* end = nullptr) {
  if (!after || after->next == end) {
    return nullptr;
  }

  for (Instr* pos = after->next; pos != end; pos = pos->next) {
    if (is_op(pos, OPCODE_LOAD_LOCAL) && pos->src1.value == slot) {
      return pos;
    } else if (is_op(pos, OPCODE_STORE_LOCAL) && pos->src1.value == slot) {
      return nullptr;  // redefed    b
    }
  }

  return nullptr;
}

enum contextuse_res_t { USED, REDEFED, INDET };

template <bool break_on_call = false>
static Instr* find_next_context_use(const ppc_ctx_vset_t* val, hir::Block* blk,
                                    Instr* start, contextuse_res_t* redefed,
                                    Instr* end = nullptr) {
  if (!start) {
    start = blk->instr_head;
  }
  *redefed = USED;
  for (Instr* pos = start; pos != end; pos = pos->next) {
    if (pos->opcode->flags & OPCODE_FLAG_VOLATILE) {
      *redefed = INDET;

      return nullptr;
    }
    if (break_on_call && is_any_call(pos)) {
      *redefed = REDEFED;
      return nullptr;
    }
    if (load_target_in_bitset(val, pos)) {
      return pos;
    } else if (store_target_in_bitset(val, pos)) {
      *redefed = REDEFED;
      return nullptr;
    }
  }
  return nullptr;
}

static bool def_is_onebit(Instr* def) {
  if (((is_op(def, OPCODE_AND) || is_op(def, OPCODE_XOR)) &&
       def->src2.value->IsConstant() && def->src2.value->constant.u64 == 1) ||
      produces_boolean(def)) {
    return true;
  }

  if (is_op(def, OPCODE_ASSIGN)) {
    return def_is_onebit(def->src1.value->def);
  }

  if (is_op(def, OPCODE_AND) || is_op(def, OPCODE_OR) ||
      is_op(def, OPCODE_XOR) || is_op(def, OPCODE_SHR) ||
      is_op(def, OPCODE_MUL)) {
    return def_is_onebit(def->src1.value->def) &&
           def_is_onebit(def->src2.value->def);
  }

  if (is_op(def, OPCODE_ZERO_EXTEND)) {
    return def_is_onebit(def->src1.value->def);
  }

  if (is_op(def, OPCODE_SIGN_EXTEND)) {
    return def_is_onebit(def->src1.value->def);
  }

  if (is_op(def, OPCODE_LOAD_CONTEXT) && def->dest->type == INT8_TYPE) {
    ppc_ctx_vset_t bset{};

    bset.set(def->src1.offset);
    if (has_any_creg(&bset)) {
      return true;
    }
  }
  return false;
}

static constexpr Opcode elimable_results[] = {
    OPCODE_ADD,          OPCODE_SUB,         OPCODE_ASSIGN,
    OPCODE_BYTE_SWAP,    OPCODE_OR,          OPCODE_XOR,
    OPCODE_AND,          OPCODE_NOT,         OPCODE_NEG,
    OPCODE_MUL,          OPCODE_CONVERT,     OPCODE_IS_FALSE,
    OPCODE_IS_TRUE,      OPCODE_COMPARE_EQ,  OPCODE_COMPARE_NE,
    OPCODE_COMPARE_SLT,  OPCODE_COMPARE_SLE, OPCODE_COMPARE_SGT,
    OPCODE_COMPARE_SGE,  OPCODE_COMPARE_ULT, OPCODE_COMPARE_ULE,
    OPCODE_COMPARE_UGT,  OPCODE_COMPARE_UGE, OPCODE_DID_SATURATE,
    OPCODE_DIV,          OPCODE_SHL,         OPCODE_SHR,
    OPCODE_SHA,          OPCODE_ABS,         OPCODE_ADD_CARRY,
    OPCODE_ZERO_EXTEND,  OPCODE_SIGN_EXTEND, OPCODE_TRUNCATE,
    OPCODE_CNTLZ,        OPCODE_LOAD_LOCAL,  OPCODE_LOAD,
    OPCODE_LOAD_CONTEXT, OPCODE_LOG2,        OPCODE_SQRT,
    OPCODE_ROTATE_LEFT,  OPCODE_ROUND

};
static bool is_elimable(Instr* i) {
  for (auto&& opcode : elimable_results) {
    if (is_op(i, opcode)) return true;
  }
  return false;
}
static bool nop_deleter(HIRBuilder* builder, Block* block) {
    #if 0
  Instr* pos = block->instr_head;
  bool did_change = false;
  while (pos) {
    Instr* nextdude = pos->next;

    if (is_op(pos, OPCODE_NOP)) {
      pos->Remove();
      did_change = true;
    }
    pos = nextdude;
  }
  return did_change;
 #else
  return false;
    #endif
}

class optrule_t {
 protected:
  HIRBuilder* builder;
  Block* block;
  uint64_t n_runs;

 public:
  optrule_t() : builder(nullptr), block(nullptr), n_runs(0) {}
  virtual const char* name() = 0;
  virtual bool run(Instr* insn) = 0;
  uint64_t get_execs() const { return n_runs; }
  bool exec(HIRBuilder* blder, Block* blk);
};

bool optrule_t::exec(HIRBuilder* blder, Block* blk) {
  bool did_change = false;
  block = blk;
  builder = blder;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (run(insn)) {
      did_change = true;
      ++n_runs;
    }
  }
  return did_change;
}

#define DECL_RULE(typenam, printname)                         \
  class typenam##_t : public optrule_t {                      \
    virtual const char* name() override { return printname; } \
    virtual bool run(Instr* insn);                            \
  };                                                          \
  static typenam##_t typenam{};

#define IMPL_RULE(typenam) bool typenam##_t ::run(Instr* insn)

#define RULE(typenam, printname) \
  DECL_RULE(typenam, printname); \
  IMPL_RULE(typenam)

using optblock_pass_t = bool (*)(HIRBuilder*, Block*);

#if 1
#include "peephole_rules_impl.h"
#else
RULE(shr_shl_mask,
     "Convert shift-left followed by shift-right to and constant") {
  if (!is_op(insn, OPCODE_SHL) || !insn->src2.value->IsConstant()) return false;

  auto defuse = get_solo_use(insn->dest);

  if (!defuse || !is_op(defuse, OPCODE_SHR) ||
      !insn->src2.value->IsConstantEQ(defuse->src2.value))
    return false;

  uint64_t mask = mask_for_typename(insn->src1.value->type);
  uint64_t shfactor = extract_constant(insn->src2.value);
  uint64_t msk = mask;
  mask <<= shfactor;
  mask >>= shfactor;
  mask &= msk;
  auto oldsrc1 = insn->src1.value;

  auto maskval = constant_for_type(builder, mask, oldsrc1->type);
  if (!maskval) return false;

  make_nop(insn);

  defuse->Replace(&OPCODE_AND_info, 0);
  defuse->set_src1(oldsrc1);
  defuse->set_src2(maskval);
  return true;
}

RULE(cvt_op_optimizer, "Remove redundant conversion pair") {
  if (!is_op(insn, OPCODE_CONVERT)) {
    return false;
  }
  if (!is_rvalue(insn->dest) || !is_rvalue(insn->src1)) return false;

  Instr* target_next_use = find_next_use(insn->dest, block, insn);

  if (!target_next_use) {
    return false;
  }

  if (!is_op(target_next_use, OPCODE_CONVERT)) return false;

  if (target_next_use->src1.value != insn->dest) return false;

  if (target_next_use->dest->type != insn->src1.value->type ||
      !is_rvalue(target_next_use->dest))
    return false;

  make_assignment(target_next_use, insn->src1);
  return true;
}

RULE(vector_shift_op, "Reuse load vector shift results") {
  if (!is_op(insn, OPCODE_LOAD_VECTOR_SHR) &&
      !is_op(insn, OPCODE_LOAD_VECTOR_SHL)) {
    return false;
  }

  if (!is_rvalue(insn->dest) || !is_rvalue(insn->src1)) return false;
  bool did_change = false;
  for (Instr* nextu = find_next_use(insn->src1, block, insn); nextu;
       nextu = find_next_use(insn->src1, block, nextu)) {
    if (!nextu || nextu->opcode != insn->opcode) return false;
    make_assignment(nextu, insn->dest);
    did_change = true;
  }
  return did_change;
}

RULE(useless_ctx_store_elim, "Eliminate useless context stores") {
  if (!is_op(insn, OPCODE_STORE_CONTEXT)) return false;
  ppc_ctx_vset_t trace{};
  add_store_to_bitset(&trace, insn);

  if (true && !has_any_creg(&trace)) {
    return false;
  }
  contextuse_res_t redefed = USED;
  auto next =
      find_next_context_use<true>(&trace, block, insn->next, &redefed);

  if (redefed == USED && !next) {
    bool all_redef = true;
    for (auto succ = block->outgoing_edge_head; succ;
         succ = succ->outgoing_next) {
      if (find_next_context_use<true>(&trace, succ->dest, nullptr,
                                           &redefed) &&
          redefed == REDEFED) {
      } else {
        all_redef = false;
        break;
      }
    }

    if (!all_redef) {
      return false;
    }

    // continue;
  } else if (redefed == INDET || redefed == USED) {
    return false;
  }

  make_nop(insn);
  return true;
}
RULE(replace_assignment_uses, "Replace rvalue assignment uses") {
  if (!is_op(insn, OPCODE_ASSIGN)) return false;
  if (!is_rvalue(insn->dest) || !is_rvalue(insn->src1)) return false;

  replace_uses(builder, insn->dest, insn->src1.value);

  make_nop(insn);
  return true;
}

RULE(optimize_rep_loads, "Remove repeated loads") {
  if (!is_op(insn, OPCODE_LOAD)) {
    return false;
  }

  if (!is_rvalue(insn->src1)) return false;

  auto n = find_next_use(insn->src1, block, insn);

  if (!n) return false;

  if (!is_op(n, OPCODE_LOAD)) return false;

  if (insn->dest->type != n->dest->type) return false;

  if (has_any_intervening_store(insn, n)) return false;

  make_assignment(n, insn->dest);
  return true;
}

RULE(optimize_and1, "Replace onebit istrue evaluation with evaluated term") {
  if (!is_op(insn, OPCODE_IS_TRUE) &&
      !(is_op(insn, OPCODE_COMPARE_EQ) && is_const_x(insn->src2, 1ULL)) &&
      !(is_op(insn, OPCODE_COMPARE_NE) && is_const_x(insn->src2, 0ULL))) {
    return false;
  }

  auto def = insn->src1.value->def;

  if (def_is_onebit(def)) {
    if (insn->src1.value->type == insn->dest->type) {
      make_assignment(insn, insn->src1);
    } else {
      insn->Replace(&OPCODE_TRUNCATE_info, 0);
      insn->set_src1(def->dest);
    }
    return true;
  }
  return false;
}

RULE(truncate_extend_opt, "Remove useless truncate-extends") {
  if (!is_op(insn, OPCODE_TRUNCATE)) return false;

  auto def = insn->src1.value->def;

  if (!is_op(def, OPCODE_ZERO_EXTEND) && !is_op(def, OPCODE_SIGN_EXTEND))
    return false;

  if (def->src1.value->type != insn->dest->type) return false;

  make_assignment(insn, def->src1);
  return true;
}

RULE(elim_unused_opres, "Eliminate operations with unused results") {
  if (!is_elimable(insn)) return false;

  if (!insn->dest->use_head || insn->dest->use_head->instr == nullptr) {
    make_nop(insn);
    return true;
  }
  return false;
}

RULE(elim_useless_op, "Eliminate useless operations") {
  if (is_op(insn, OPCODE_ADD) || is_op(insn, OPCODE_SUB) ||
      is_op(insn, OPCODE_OR) || is_op(insn, OPCODE_XOR)) {
    if (is_const_x(insn->src2, 0)) {
      make_assignment(insn, insn->src1);
      return true;
    }
  }

  if (is_op(insn, OPCODE_AND) &&
      is_const_x(insn->src2, mask_for_typename(insn->src1.value->type))) {
    make_assignment(insn, insn->src1);
    return true;
  }

  if ((is_op(insn, OPCODE_DIV) || is_op(insn, OPCODE_MUL)) &&
      is_const_x(insn->src2, 1ULL)) {
    make_assignment(insn, insn->src1);
    return true;
  }
  return false;
}

RULE(rol_to_shl, "Convert rotates to shifts") {
  if (!is_op(insn, OPCODE_ROTATE_LEFT)) return false;

  if (!insn->src2.value->IsConstant()) return false;

  auto defed_value = insn->dest;

  auto nxtuse = get_solo_use(defed_value);
  if (!nxtuse) return false;

  if (!is_op(nxtuse, OPCODE_AND)) return false;

  if (!nxtuse->src2.value->IsConstant()) return false;

  uint64_t v = extract_constant(nxtuse->src2.value);

  uint64_t rotamt = extract_constant(insn->src2.value);

  if ((v & ((1ULL << rotamt) - 1)) == 0ULL) {
    // the bits rotated in to the left are discarded, can be shl
    insn->opcode = &OPCODE_SHL_info;
    return true;
  }
  return false;
}

RULE(remove_useless_or, "Remove useless or if bits are not used") {
  if (!is_op(insn, OPCODE_SHL) || !insn->src2.value->IsConstant()) return false;

  auto solo_use_shl_result = get_solo_use(insn->dest);

  if (!solo_use_shl_result) return false;

  if (!is_op(solo_use_shl_result, OPCODE_OR) ||
      solo_use_shl_result->src1.value != insn->dest) {
    return false;
  }

  auto solo_use_or_result = get_solo_use(solo_use_shl_result->dest);
  if (!solo_use_or_result) return false;

  auto mask = mask_for_typename(insn->dest->type);

  mask <<= extract_constant(insn->src2.value);


  if (is_op(solo_use_or_result, OPCODE_AND) &&
      solo_use_or_result->src2.value->IsConstant()) {
    auto newmask = extract_constant(solo_use_or_result->src2.value);

    if ((newmask & mask) == 0) {
      // none of our shl bits survive
      make_nop(insn);
      auto newandoper = solo_use_shl_result->src2.value;

      // make_nop(solo_use_shl_result);
      make_assignment(solo_use_shl_result, solo_use_shl_result->src2);
      return true;
    }
  }
  return false;
}

RULE(signbit_rol_opt, "Optimize rotates to extract signbit") {
  if (!is_op(insn, OPCODE_ROTATE_LEFT) || !is_const_x(insn->src2, 1ULL))
    return false;

  auto solo_use = get_solo_use(insn->dest);
  if (!solo_use) return false;

  if (!is_op(solo_use, OPCODE_AND) || !is_const_x(solo_use->src2, 1ULL))
    return false;

  Value* shift_constant = constant_for_type(
      builder, highbit_for_typename(insn->dest->type), INT8_TYPE);
  if (!shift_constant) return false;

  solo_use->Replace(&OPCODE_SHR_info, 0);

  solo_use->set_src1(insn->src1.value);
  solo_use->set_src2(shift_constant);
  make_nop(insn);
  return true;
}

RULE(bitextract_by_rol_opt, "Optimize rotates intended to extract one bit") {
  if (!is_op(insn, OPCODE_ROTATE_LEFT) || !insn->src2.value->IsConstant())
    return false;

  auto defuse = get_solo_use(insn->dest);

  if (!defuse) return false;

  if (!is_op(defuse, OPCODE_AND) || !is_const_x(defuse->src2, 1)) return false;

  unsigned bitsz = highbit_for_typename(insn->dest->type);

  uint64_t realtarget =

      (bitsz + 1) - extract_constant(insn->src2.value);
  auto old_inp = insn->src1.value;

  Value* shrconst = constant_for_type(builder, realtarget, INT8_TYPE);

  if (!shrconst) return false;
  insn->Replace(&OPCODE_SHR_info, 0);

  insn->set_src1(old_inp);
  insn->set_src2(shrconst);
  return true;
}

RULE(optimize_redundant_local_load, "Optimize away redundant local loads") {
  if (!is_op(insn, OPCODE_LOAD_LOCAL)) {
    return false;
  }

  auto next_load = find_next_local_load(insn->src1.value, block, insn);

  if (!next_load) return false;

  make_assignment(next_load, insn->dest);
  return true;
}
static optrule_t* g_optrules[] = {&shr_shl_mask, &cvt_op_optimizer,        &vector_shift_op,
    &useless_ctx_store_elim, &replace_assignment_uses, &optimize_rep_loads,
    &optimize_and1,          &truncate_extend_opt,     &elim_unused_opres,
    &elim_useless_op,        &remove_useless_or,       &signbit_rol_opt,
                                  &bitextract_by_rol_opt,
                                  &optimize_redundant_local_load};


static bool run_optrules(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  for (auto&& rule : g_optrules) {
    did_change |= rule->exec(builder, block);
  }
  return did_change;
}

void dump_opts() {
  FILE* lel = fopen("Optdump.txt", "w");

  for (auto&& rule : g_optrules) {
    fprintf(lel, "(%s) matched and optimized %lld times.\n", rule->name(),
            rule->get_execs());
  }
  fclose(lel);
}
static const optblock_pass_t g_passes[] = {run_optrules, nop_deleter};
#endif

static bool did_atexit = false;

bool RepetitiveComputationMergerPass::RunPerBlock(hir::HIRBuilder* builder,
                                                  hir::Block* block) {
  bool did_change = false;
  for (auto&& pass : g_passes) {
    did_change |= pass(builder, block);
  }
  return did_change;
}
bool RepetitiveComputationMergerPass::Run(hir::HIRBuilder* builder) {
  if (!did_atexit) {
   // atexit(dump_opts);
    did_atexit = true;
  }
  auto block = builder->first_block();

  while (block) {
    // rerun until no changes
    while (RunPerBlock(builder, block))
      ;

    block = block->next;
  }
  return true;
}
}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe