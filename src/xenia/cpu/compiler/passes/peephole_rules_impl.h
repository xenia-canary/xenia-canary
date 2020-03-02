#pragma once

static uint64_t n_cvt_opts = 0, n_vec_shift_opts = 0,
              n_useless_ctx_store_opts = 0;

static bool do_cvt_opt(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_CONVERT)) {
      continue;
    }

    /*if (insn->dest->type != FLOAT32_TYPE ||
        insn->src1.value->type != FLOAT64_TYPE) {
      continue;
    }*/

    if (!is_rvalue(insn->dest) || !is_rvalue(insn->src1)) continue;

    Instr* target_next_use = find_next_use(insn->dest, block, insn);

    if (!target_next_use) {
      continue;
    }

    if (!is_op(target_next_use, OPCODE_CONVERT)) continue;

    if (target_next_use->src1.value != insn->dest) continue;

    if (target_next_use->dest->type != insn->src1.value->type ||
	!is_rvalue(target_next_use->dest)) continue;

    ++n_cvt_opts;
    make_assignment(target_next_use, insn->src1);

    did_change = true;
  }
  return did_change;
}

static bool do_vector_shift_opt(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_LOAD_VECTOR_SHR) &&
        !is_op(insn, OPCODE_LOAD_VECTOR_SHL)) {
      continue;
    }

    if (!is_rvalue(insn->dest) || !is_rvalue(insn->src1)) continue;

    for (Instr* nextu = find_next_use(insn->src1, block, insn); nextu;
         nextu = find_next_use(insn->src1, block, nextu)) {
      if (!nextu || nextu->opcode != insn->opcode) continue;
      ++n_vec_shift_opts;
      make_assignment(nextu, insn->dest);
      did_change = true;
    }
  }
  return did_change;
}
template <bool creg_only = false>
static bool elim_useless_ctx_stores(HIRBuilder* builder, Block* block) {
  bool did_change = false;

  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_STORE_CONTEXT)) continue;
    ppc_ctx_vset_t trace{};
    add_store_to_bitset(&trace, insn);

    if (creg_only && !has_any_creg(&trace)) {
      continue;
    }
    contextuse_res_t redefed = USED;
    auto next =
        find_next_context_use<creg_only>(&trace, block, insn->next, &redefed);

    if (redefed == USED && !next) {
      bool all_redef = true;
      for (auto succ = block->outgoing_edge_head; succ;
           succ = succ->outgoing_next) {
        if (find_next_context_use<creg_only>(&trace, succ->dest, nullptr,
                                             &redefed) &&
            redefed == REDEFED) {
        } else {
          all_redef = false;
          break;
        }
      }

      if (!all_redef) {
        continue;
      }

      // continue;
    } else if (redefed == INDET || redefed == USED) {
      continue;
    }
    ++n_useless_ctx_store_opts;

    make_nop(insn);
    did_change = true;
  }
  return did_change;
}
static uint64_t n_replaced_assignments = 0;

static bool replace_assignment_uses(HIRBuilder* builder, Block* block) {
  bool did_change = false;

  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_ASSIGN)) continue;
    if (!is_rvalue(insn->dest) || !is_rvalue(insn->src1)) continue;

    replace_uses(builder, insn->dest, insn->src1.value);
    n_replaced_assignments++;

    make_nop(insn);
    did_change = true;
  }
  return did_change;
}
static uint64_t n_replaced_loads = 0;
static bool optimize_repeated_loads(HIRBuilder* builder, Block* block) {
  bool did_change = false;

  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_LOAD)) {
      continue;
    }

    if (!is_rvalue(insn->src1)) continue;

    auto n = find_next_use(insn->src1, block, insn);

    if (!n) continue;

    if (!is_op(n, OPCODE_LOAD)) continue;

    if (insn->dest->type != n->dest->type) continue;

    if (has_any_intervening_store(insn, n)) continue;

    make_assignment(n, insn->dest);
    n_replaced_loads++;
    did_change = true;
  }
  return did_change;
}
static uint64_t n_and1s_optimized = 0;

static bool and1_optimizer(HIRBuilder* builder, Block* block) {
  bool did_change = false;

  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_IS_TRUE) &&
        !(is_op(insn, OPCODE_COMPARE_EQ) && is_const_x(insn->src2, 1ULL)) &&
        !(is_op(insn, OPCODE_COMPARE_NE) && is_const_x(insn->src2, 0ULL))) {
      continue;
    }

    auto def = insn->src1.value->def;

    if (def_is_onebit(def)) {
      if (insn->src1.value->type == insn->dest->type) {
        make_assignment(insn, insn->src1);
      } else {
        insn->Replace(&OPCODE_TRUNCATE_info, 0);
        insn->set_src1(def->dest);
      }
      n_and1s_optimized++;
      did_change = true;
    }
  }
  return did_change;
}
static uint64_t n_trunc_exts = 0;

static bool trunc_ext_optimizer(HIRBuilder* builder, Block* block) {
  bool did_change = false;

  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_TRUNCATE)) continue;

    auto def = insn->src1.value->def;

    if (!is_op(def, OPCODE_ZERO_EXTEND) && !is_op(def, OPCODE_SIGN_EXTEND))
      continue;

    if (def->src1.value->type != insn->dest->type) continue;

    make_assignment(insn, def->src1);
    ++n_trunc_exts;
    did_change = true;
    // __debugbreak();
  }
  return did_change;
}
static uint64_t n_eliminated_unused_conversion_results = 0;
static bool unused_conversion_eliminator(HIRBuilder* builder, Block* block) {
  bool did_change = false;

  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_elimable(insn)) continue;

    if (!insn->dest->use_head || insn->dest->use_head->instr == nullptr) {
      make_nop(insn);
      did_change = true;
      n_eliminated_unused_conversion_results++;
    }
  }
  return did_change;
}
static uint64_t n_useless_operations = 0;
static bool useless_operation_eliminator(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  auto signal_change = [&did_change](auto insn, auto& src) {
    make_assignment(insn, insn->src1);
    n_useless_operations++;
    did_change = true;
  };
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (is_op(insn, OPCODE_ADD) || is_op(insn, OPCODE_SUB) ||
        is_op(insn, OPCODE_OR) || is_op(insn, OPCODE_XOR)) {
      if (is_const_x(insn->src2, 0)) {
        signal_change(insn, insn->src1);

        continue;
      }
    }

    if (is_op(insn, OPCODE_AND) &&
        is_const_x(insn->src2, mask_for_typename(insn->src1.value->type))) {
      signal_change(insn, insn->src1);
      continue;
    }

    if ((is_op(insn, OPCODE_DIV) || is_op(insn, OPCODE_MUL)) &&
        is_const_x(insn->src2, 1ULL)) {
      signal_change(insn, insn->src1);
      continue;
    }
  }
  return did_change;
}
static uint64_t n_rotates_lowered_to_shifts = 0;
static bool rlwinm_sanitizer(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_ROTATE_LEFT)) continue;

    if (!insn->src2.value->IsConstant()) continue;

    auto defed_value = insn->dest;

    auto nxtuse = get_solo_use(defed_value);
    if (!nxtuse) continue;

    if (!is_op(nxtuse, OPCODE_AND)) continue;

    if (!nxtuse->src2.value->IsConstant()) continue;

    uint64_t v = extract_constant(nxtuse->src2.value);

    uint64_t rotamt = extract_constant(insn->src2.value);

    if ((v & ((1ULL << rotamt) - 1)) == 0ULL) {
      // the bits rotated in to the left are discarded, can be shl
      insn->opcode = &OPCODE_SHL_info;
      n_rotates_lowered_to_shifts++;
      did_change = true;
    }
  }

  return did_change;
}
static uint64_t n_useless_rlwinm_ors_removed = 0;
static bool rlwinm_sanitizer_useless_or(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_SHL) || !insn->src2.value->IsConstant()) continue;

    auto solo_use_shl_result = get_solo_use(insn->dest);

    if (!solo_use_shl_result) continue;

    if (!is_op(solo_use_shl_result, OPCODE_OR) ||
        solo_use_shl_result->src1.value != insn->dest) {
      continue;
    }

    auto solo_use_or_result = get_solo_use(solo_use_shl_result->dest);
    if (!solo_use_or_result) continue;

    auto mask = mask_for_typename(insn->dest->type);

    mask <<= extract_constant(insn->src2.value);

    /* if (is_op(solo_use_or_result, OPCODE_TRUNCATE)) {
       auto newmask = mask_for_typename(solo_use_or_result->dest->type);
       if ((newmask & mask) == 0) {
         // none of our shl bits survive
         make_nop(insn);
         make_assignment(solo_use_or_result, solo_use_shl_result->src2);
         n_useless_rlwinm_ors_removed++;
         did_change = true;
       }
     }

     else*/
    if (is_op(solo_use_or_result, OPCODE_AND) &&
        solo_use_or_result->src2.value->IsConstant()) {
      auto newmask = extract_constant(solo_use_or_result->src2.value);

      if ((newmask & mask) == 0) {
        // none of our shl bits survive
        make_nop(insn);
        auto newandoper = solo_use_shl_result->src2.value;

        // make_nop(solo_use_shl_result);
        make_assignment(solo_use_shl_result, solo_use_shl_result->src2);
        n_useless_rlwinm_ors_removed++;
        did_change = true;
      }
    }
  }

  return did_change;
}

static uint64_t n_ssa_merges = 0;
static bool ssa_merger(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_ASSIGN)) continue;

    auto solo = get_solo_use(insn->dest);
    if (!solo || !is_like_assign(solo)) continue;

    solo->set_src1(insn->src1.value);

    make_nop(insn);
    ++n_ssa_merges;

    did_change = true;
  }
  return did_change;
}
static uint64_t n_signbit_rols_optimized = 0;
static bool signbit_rol_optimizer(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_ROTATE_LEFT) || !is_const_x(insn->src2, 1ULL))
      continue;

    auto solo_use = get_solo_use(insn->dest);
    if (!solo_use) continue;

    if (!is_op(solo_use, OPCODE_AND) || !is_const_x(solo_use->src2, 1ULL))
      continue;

    Value* shift_constant = constant_for_type(
        builder, highbit_for_typename(insn->dest->type), INT8_TYPE);
    if (!shift_constant) continue;

    solo_use->Replace(&OPCODE_SHR_info, 0);

    solo_use->set_src1(insn->src1.value);
    solo_use->set_src2(shift_constant);
    make_nop(insn);
    did_change = true;
    n_signbit_rols_optimized++;
  }
  return did_change;
}

static uint64_t n_rol_bitextracts_optimized = 0;
static bool rol_bitextract_optimizer(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_ROTATE_LEFT) || !insn->src2.value->IsConstant())
      continue;

    auto defuse = get_solo_use(insn->dest);

    if (!defuse) continue;

    if (!is_op(defuse, OPCODE_AND) || !is_const_x(defuse->src2, 1)) continue;

    unsigned bitsz = highbit_for_typename(insn->dest->type);

    uint64_t realtarget =

        (bitsz + 1) - extract_constant(insn->src2.value);
    auto old_inp = insn->src1.value;

    Value* shrconst = constant_for_type(builder, realtarget, INT8_TYPE);

    if (!shrconst) continue;
    insn->Replace(&OPCODE_SHR_info, 0);

    insn->set_src1(old_inp);
    insn->set_src2(shrconst);
    n_rol_bitextracts_optimized++;
    did_change = true;
  }
  return did_change;
}

static uint64_t n_shl_shr_masks_gen = 0;

static bool shl_shr_mask_optimizer(HIRBuilder* builder, Block* block) {
  bool did_change = false;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_SHL) || !insn->src2.value->IsConstant()) continue;

    auto defuse = get_solo_use(insn->dest);

    if (!defuse || !is_op(defuse, OPCODE_SHR) ||
        !insn->src2.value->IsConstantEQ(defuse->src2.value))
      continue;

    uint64_t mask = mask_for_typename(insn->src1.value->type);
    uint64_t shfactor = extract_constant(insn->src2.value);
    uint64_t msk = mask;
    mask <<= shfactor;
    mask >>= shfactor;
    mask &= msk;
    auto oldsrc1 = insn->src1.value;

    auto maskval = constant_for_type(builder, mask, oldsrc1->type);
    if (!maskval) continue;

    make_nop(insn);

    defuse->Replace(&OPCODE_AND_info, 0);
    defuse->set_src1(oldsrc1);
    defuse->set_src2(maskval);
    n_shl_shr_masks_gen++;
    did_change = true;
  }
  return did_change;
}
static uint64_t n_redundant_loads_local = 0;
static bool redundant_local_slot_load_optimizer(HIRBuilder* builder,
                                                Block* block) {
  bool did_change = false;
  for (auto* insn = block->instr_head; insn; insn = insn->next) {
    if (!is_op(insn, OPCODE_LOAD_LOCAL)) {
      continue;
    }

    auto next_load = find_next_local_load(insn->src1.value, block, insn);

    if (!next_load) continue;

    make_assignment(next_load, insn->dest);

    ++n_redundant_loads_local;
    did_change = true;
  }
  return did_change;
}
/*
Re-enable these one by one or add flags to control them until each is proven to be stable

*/
#if 0
static const optblock_pass_t g_passes[] = {do_cvt_opt, // idk
                                           elim_useless_ctx_stores<false>, // idk
                                           elim_useless_ctx_stores<true>, // idk
                                           redundant_local_slot_load_optimizer, //ok
                                           do_vector_shift_opt,//
                                           replace_assignment_uses, //
                                           optimize_repeated_loads, //ok
                                           and1_optimizer, // instability?
                                           trunc_ext_optimizer, // ok
                                           unused_conversion_eliminator, //ok
                                           useless_operation_eliminator, //bad
                                           rlwinm_sanitizer, // maybe
                                           rlwinm_sanitizer_useless_or, //ok
                                           ssa_merger, //bad
                                           signbit_rol_optimizer, //artifacts?
                                           rol_bitextract_optimizer, //artifacts?
                                           shl_shr_mask_optimizer, //ok
                                           nop_deleter}; //ok
void dump_opts() {
  FILE* lel = fopen("Optdump.txt", "w");

  fprintf(
      lel,
      "Optimized %lld converts\nOptimized %lld vector shifts\nOptimized %lld "
      "context stores\nOptimized away %lld useless assignments\nOptimized "
      "away "
      "%lld redundant loads."
      "\nOptimized away %lld and 1 instructions\nOptimized away %lld useless "
      "truncate-extends\nEliminated %lld unused operation "
      "results.\nOptimized away %lld no-op operations.\nLowered %lld left "
      "rotates to left shifts.\nRemoved %lld useless rlwinm rotate-or "
      "sequences.\nShortened %lld assignment chains\nOptimized %lld signbit "
      "rotate lefts.\nOptimized %lld rol bitextracts.\nConverted %lld shift "
      "sequences to bitmasks.\n Optimized %lld redundant local loads.",
      n_cvt_opts, n_vec_shift_opts, n_useless_ctx_store_opts,
      n_replaced_assignments, n_replaced_loads, n_and1s_optimized, n_trunc_exts,
      n_eliminated_unused_conversion_results, n_useless_operations,
      n_rotates_lowered_to_shifts, n_useless_rlwinm_ors_removed, n_ssa_merges,
      n_signbit_rols_optimized, n_rol_bitextracts_optimized,
      n_shl_shr_masks_gen, n_redundant_loads_local);
  fclose(lel);
}
#else
static const optblock_pass_t g_passes[] = { redundant_local_slot_load_optimizer,
                                            do_vector_shift_opt,
                                            shl_shr_mask_optimizer,
                                            rlwinm_sanitizer_useless_or,
                                            and1_optimizer,
                                            trunc_ext_optimizer,
                                            unused_conversion_eliminator,
                                            optimize_repeated_loads,
                                            replace_assignment_uses,
                                            rlwinm_sanitizer,
                                            nop_deleter };

void dump_opts() {
  FILE* lel = fopen("Optdump.txt", "w");

  fprintf(
      lel,
#if XE_PLATFORM_WIN32
      "Optimized %lld converts\nOptimized %lld vector shifts\nOptimized %lld "
      "context stores\nOptimized away %lld useless assignments\nOptimized "
      "away "
      "%lld redundant loads."
      "\nOptimized away %lld and 1 instructions\nOptimized away %lld useless "
      "truncate-extends\nEliminated %lld unused operation "
      "results.\nOptimized away %lld no-op operations.\nLowered %lld left "
      "rotates to left shifts.\nRemoved %lld useless rlwinm rotate-or "
      "sequences.\nShortened %lld assignment chains\nOptimized %lld signbit "
      "rotate lefts.\nOptimized %lld rol bitextracts.\nConverted %lld shift "
      "sequences to bitmasks.\n Optimized %lld redundant local loads.",
#else
      "Optimized %ld converts\nOptimized %ld vector shifts\nOptimized %ld "
      "context stores\nOptimized away %ld useless assignments\nOptimized "
      "away "
      "%ld redundant loads."
      "\nOptimized away %ld and 1 instructions\nOptimized away %ld useless "
      "truncate-extends\nEliminated %ld unused operation "
      "results.\nOptimized away %ld no-op operations.\nLowered %ld left "
      "rotates to left shifts.\nRemoved %ld useless rlwinm rotate-or "
      "sequences.\nShortened %ld assignment chains\nOptimized %ld signbit "
      "rotate lefts.\nOptimized %ld rol bitextracts.\nConverted %ld shift "
      "sequences to bitmasks.\n Optimized %ld redundant local loads.",
#endif  // XE_PLATFORM_WIN32
      n_cvt_opts, n_vec_shift_opts, n_useless_ctx_store_opts,
      n_replaced_assignments, n_replaced_loads, n_and1s_optimized, n_trunc_exts,
      n_eliminated_unused_conversion_results, n_useless_operations,
      n_rotates_lowered_to_shifts, n_useless_rlwinm_ors_removed, n_ssa_merges,
      n_signbit_rols_optimized, n_rol_bitextracts_optimized,
      n_shl_shr_masks_gen, n_redundant_loads_local);
  fclose(lel);
}
#endif
