/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/control_flow_simplification_pass.h"

#include "xenia/base/profiling.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Edge;
using xe::cpu::hir::HIRBuilder;

ControlFlowSimplificationPass::ControlFlowSimplificationPass()
    : CompilerPass() {}

ControlFlowSimplificationPass::~ControlFlowSimplificationPass() {}

bool ControlFlowSimplificationPass::Run(HIRBuilder* builder) {
  // Walk forwards and kill any unreachable blocks.
  // Do this before merging.
  auto block = builder->first_block();
  while (block) {
    auto next_block = block->next;
    if (!block->incoming_edge_head && block->prev) {
      // Block is in the interior and has no incoming edges - kill it.
      builder->RemoveBlock(block);
    }
    block = next_block;
  }

  // Walk forwards and merge each block into the one before it if possible.
  // Merging moves the right block's instructions, so walking forwards moves
  // each instruction once. Walking backwards would move the growing merged
  // block again at every step, which is quadratic in functions with thousands
  // of calls (each call ends a block).
  bool merged_any = false;
  block = builder->first_block();
  while (block && block->next) {
    auto next_block = block->next;
    constexpr uint32_t expected = Edge::DOMINATES | Edge::UNCONDITIONAL;
    if (next_block->incoming_edge_head &&
        (next_block->incoming_edge_head->flags & expected) == expected &&
        next_block->incoming_edge_head->src == block && block->instr_tail &&
        !(block->instr_tail->opcode->flags & OPCODE_FLAG_VOLATILE)) {
      // The next block is dominated by this one and follows it, so merge them
      // (unless this one ends in a volatile instruction like Trap). Stay on
      // this block, which may merge with its new next block too.
      builder->MergeAdjacentBlocks(block, next_block);
      merged_any = true;
      continue;
    }
    block = next_block;
  }

  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
