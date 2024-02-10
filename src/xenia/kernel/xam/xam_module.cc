/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_module.h"

#include <vector>

#include "xenia/base/math.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_private.h"

namespace xe {
namespace kernel {
namespace xam {

std::atomic<int> xam_dialogs_shown_ = {0};

bool xeXamIsUIActive() { return xam_dialogs_shown_ > 0; }

XamModule::XamModule(Emulator* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xam.xex"), loader_data_() {
  RegisterExportTable(export_resolver_);

  // Register all exported functions.
#define XE_MODULE_EXPORT_GROUP(m, n) \
  Register##n##Exports(export_resolver_, kernel_state_);
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP
}

std::vector<xe::cpu::Export*> xam_exports(4096);

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry) {
  assert_true(export_entry->ordinal < xam_exports.size());
  xam_exports[export_entry->ordinal] = export_entry;
  return export_entry;
}
// Build the export table used for resolution.
#include "xenia/kernel/util/export_table_pre.inc"
static constexpr xe::cpu::Export xam_export_table[] = {
#include "xenia/kernel/xam/xam_table.inc"
};
#include "xenia/kernel/util/export_table_post.inc"
void XamModule::RegisterExportTable(xe::cpu::ExportResolver* export_resolver) {
  assert_not_null(export_resolver);

  for (size_t i = 0; i < xe::countof(xam_export_table); ++i) {
    auto& export_entry = xam_export_table[i];
    assert_true(export_entry.ordinal < xam_exports.size());
    if (!xam_exports[export_entry.ordinal]) {
      xam_exports[export_entry.ordinal] =
          const_cast<xe::cpu::Export*>(&export_entry);
    }
  }
  export_resolver->RegisterTable("xam.xex", &xam_exports);
}

XamModule::~XamModule() {}

void XamModule::LoadLoaderData() {
  std::ifstream file("launch_data.bin");

  if (!file.is_open()) {
    loader_data_.launch_data_present = false;
    return;
  }

  loader_data_.launch_data_present = true;

  std::getline(file, loader_data_.host_path);
  std::getline(file, loader_data_.launch_path);

  std::string launch_flags_str = "";
  std::getline(file, launch_flags_str);
  loader_data_.launch_flags =
      xe::string_util::from_string<uint32_t>(launch_flags_str);

  std::string data_size_str = "";
  std::getline(file, data_size_str);

  const uint32_t data_size =
      xe::string_util::from_string<uint32_t>(data_size_str);

  // Highest possible value for launch data is 4kb
  if (data_size > 0 && data_size < 0x1000) {
    loader_data_.launch_data.resize(data_size);

    file.read(reinterpret_cast<char*>(loader_data_.launch_data.data()),
              data_size);
  }

  file.close();

  // We read launch data. Let's remove it till next request.
  std::filesystem::remove("launch_data.bin");
}

void XamModule::SaveLoaderData() {
  std::ofstream file("launch_data.bin");

  if (!file.is_open()) {
    return;
  }

  std::filesystem::path host_path = loader_data_.host_path;
  if (host_path.extension() == ".xex") {
    host_path.remove_filename();
    host_path = host_path / loader_data_.launch_path;
    loader_data_.launch_path = "";
  }

  file << xe::path_to_utf8(host_path) << std::endl;
  file << loader_data_.launch_path << std::endl;
  file << loader_data_.launch_flags << std::endl;
  file << loader_data_.launch_data.size() << std::endl;
  if (!loader_data_.launch_data.empty()) {
    file.write((const char*)(loader_data_.launch_data.data()),
               loader_data_.launch_data.size());
  }

  file.close();
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
