/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_module.h"

#include "xenia/base/math.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_private.h"

namespace xe {
namespace kernel {
namespace xam {

XamModule::XamModule(Emulator* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xam.xex"), loader_data_() {
  RegisterExportTable(export_resolver_);

  // Register all exported functions.
#define XE_MODULE_EXPORT_GROUP(m, n) \
  Register##n##Exports(export_resolver_, kernel_state_);
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP
}

static auto& get_xam_exports() {
  static std::vector<xe::cpu::Export*> xam_exports(4096);
  return xam_exports;
}

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry) {
  auto& xam_exports = get_xam_exports();
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
  auto& xam_exports = get_xam_exports();

  for (size_t i = 0; i < xe::countof(xam_export_table); ++i) {
    auto& export_entry = xam_export_table[i];
    assert_true(export_entry.ordinal < xam_exports.size());
    if (!xam_exports[export_entry.ordinal]) {
      xam_exports[export_entry.ordinal] =
          const_cast<xe::cpu::Export*>(&export_entry);
    }
  }
  export_resolver->RegisterTable("xam.xex", &get_xam_exports());
}

XamModule::~XamModule() {}

void XamModule::LoadLoaderData() {
  FILE* file = xe::filesystem::OpenFile(kXamModuleLoaderDataFileName, "rb");

  if (!file) {
    loader_data_.launch_data.clear();
    return;
  }

  auto string_read = [file]() {
    uint16_t string_size = 0;
    fread(&string_size, sizeof(string_size), 1, file);

    std::string result_string;
    result_string.resize(string_size);
    fread(result_string.data(), string_size, 1, file);
    return result_string;
  };

  loader_data_.host_path = string_read();
  loader_data_.launch_path = string_read();

  fread(&loader_data_.launch_flags, sizeof(loader_data_.launch_flags), 1, file);

  uint16_t launch_data_size = 0;
  fread(&launch_data_size, sizeof(launch_data_size), 1, file);

  if (launch_data_size > 0) {
    loader_data_.launch_data.resize(launch_data_size);
    fread(loader_data_.launch_data.data(), launch_data_size, 1, file);
  }

  fclose(file);
  // We read launch data. Let's remove it till next request.
  std::filesystem::remove(kXamModuleLoaderDataFileName);
}

void XamModule::ResolveNextTitlePath() {
  std::filesystem::path host_path = loader_data_.host_path;
  std::string& launch_path = loader_data_.launch_path;

  auto remove_prefix = [&launch_path](std::string_view prefix) {
    if (xe::utf8::lower_ascii(launch_path)
            .starts_with(xe::utf8::lower_ascii(prefix))) {
      launch_path = launch_path.substr(prefix.length());
    }
  };

  remove_prefix(fmt::format("{}\\", kDefaultGameSymbolicLink));
  remove_prefix(fmt::format("{}\\", kDefaultPartitionSymbolicLink));

  // Titles launched from a xex are mounted from its folder, so launch the
  // sibling xex directly.
  if (host_path.extension() == ".xex") {
    host_path.remove_filename();
    host_path = host_path / launch_path;
    launch_path.clear();
  }

  loader_data_.host_path = xe::path_to_utf8(host_path);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
