/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/preempt_check_injection_pass.h"

#include "xenia/base/cvar.h"
#include "xenia/cpu/hir/hir_builder.h"

DECLARE_bool(guest_scheduler);

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

PreemptCheckInjectionPass::PreemptCheckInjectionPass() : CompilerPass() {}

PreemptCheckInjectionPass::~PreemptCheckInjectionPass() {}

bool PreemptCheckInjectionPass::Run(HIRBuilder* builder) {
  // The bool return is pass success, not whether anything changed, and Compile
  // aborts the whole function on false.
  //
  // Read the cvar here, not in the ctor, so a per-title override applies.
  if (cvars::guest_scheduler) {
    for (auto block = builder->first_block(); block != nullptr;
         block = block->next) {
      // Skip leading fake instructions so the check lands before real code.
      auto first = block->instr_head;
      for (; first && first->IsFake(); first = first->next) {
      }
      if (first && first->GetOpcodeNum() != OPCODE_CHECK_PREEMPT) {
        builder->CheckPreempt()->MoveBefore(first);
      }
    }
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
