/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_APPS_XAM_APP_H_
#define XENIA_KERNEL_XAM_APPS_XAM_APP_H_

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/app_manager.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

#pragma pack(push, 4)
struct X_2B003_UNK {
  xe::be<uint64_t> unk1;
  xe::be<uint64_t> unk2;
  xe::be<uint32_t> flags;
};
static_assert_size(X_2B003_UNK, 0x14);
#pragma pack(pop)

class XamApp : public App {
 public:
  explicit XamApp(KernelState* kernel_state);

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_APPS_XAM_APP_H_
