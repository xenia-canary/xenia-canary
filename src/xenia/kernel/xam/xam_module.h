/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_MODULE_H_
#define XENIA_KERNEL_XAM_XAM_MODULE_H_

#include <string>

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/kernel_module.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {
namespace xam {

static constexpr std::string_view kXamModuleLoaderDataFileName =
    "launch_data.bin";

class XamModule : public KernelModule {
 public:
  XamModule(Emulator* emulator, KernelState* kernel_state);
  virtual ~XamModule();

  static void RegisterExportTable(xe::cpu::ExportResolver* export_resolver);

  struct LoaderData {
    std::string host_path;  // Full host path to next title to load
    std::string
        launch_path;  // Full guest path to next xex. might be default.xex

    uint32_t launch_flags = 0;
    std::vector<uint8_t> launch_data;
  };

  void LoadLoaderData();
  void SaveLoaderData();

  void SetLaunchCallback(uint32_t callback) { callback_ = callback; }

  const LoaderData& loader_data() const { return loader_data_; }
  LoaderData& loader_data() { return loader_data_; }

 private:
  uint32_t callback_ = 0;
  LoaderData loader_data_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_MODULE_H_
