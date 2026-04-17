/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#if XE_PLATFORM_MACOS
#include "xenia/cpu/ppc/ppc_context_mac.h"
#else
#include "xenia/cpu/ppc/ppc_context.h"
#endif
#if XE_PLATFORM_MACOS
#include "xenia/kernel/kernel_state_mac.h"
#else
#include "xenia/kernel/kernel_state.h"
#endif
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

void KeCertMonitorCallback(cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state) {
  auto id = ppc_context->r[3];
  auto arg = ppc_context->r[4];
  XELOGI("KeCertMonitorCallback({}, {:08X})", id, arg);
  auto xboxkrnl = kernel_state->GetKernelModule<XboxkrnlModule>("xboxkrnl.exe");
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
