/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_module.h"

#include <cstdio>

#include "xenia/base/cvar.h"
#include "xenia/base/math_mac.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/xam/xam_private.h"

namespace xe {
namespace kernel {
namespace xam {

DEFINE_bool(
    xam_log_export_registration_breadcrumbs, false,
    "Verbose breadcrumb logging for XAM export registration and table setup. "
    "Very high volume; intended only for focused diagnostics.",
    "Kernel");

#define XAM_BREADCRUMB(...)                                              \
  do {                                                                   \
    if (cvars::xam_log_export_registration_breadcrumbs) {                \
      std::printf(__VA_ARGS__);                                          \
    }                                                                    \
  } while (0)

XamModule::XamModule(Emulator* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xam.xex"), loader_data_() {
  XAM_BREADCRUMB("BREADCRUMB: XamModule constructor entered\n");
  XAM_BREADCRUMB("BREADCRUMB: XamModule calling RegisterExportTable\n");
  RegisterExportTable(export_resolver_);
  XAM_BREADCRUMB("BREADCRUMB: XamModule RegisterExportTable done\n");

  // Register all exported functions.
  XAM_BREADCRUMB("BREADCRUMB: XamModule registering export groups\n");
#define XE_MODULE_EXPORT_GROUP(m, n)                               \
  XAM_BREADCRUMB("BREADCRUMB: XamModule calling Register" #n "Exports\n"); \
  Register##n##Exports(export_resolver_, kernel_state_);           \
  XAM_BREADCRUMB("BREADCRUMB: XamModule Register" #n "Exports done\n");
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP
  XAM_BREADCRUMB("BREADCRUMB: XamModule constructor done\n");
}

static auto& get_xam_exports() {
  XAM_BREADCRUMB("BREADCRUMB: get_xam_exports called\n");
  static std::vector<xe::cpu::Export*> xam_exports(4096);
  XAM_BREADCRUMB("BREADCRUMB: get_xam_exports returning vector size %zu\n",
                 xam_exports.size());
  return xam_exports;
}

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry) {
  XAM_BREADCRUMB("BREADCRUMB: RegisterExport_xam called for ordinal %u name %s\n",
                 export_entry->ordinal,
                 export_entry->name ? export_entry->name : "NULL");
  auto& xam_exports = get_xam_exports();
  assert_true(export_entry->ordinal < xam_exports.size());
  xam_exports[export_entry->ordinal] = export_entry;
  XAM_BREADCRUMB("BREADCRUMB: RegisterExport_xam done\n");
  return export_entry;
}
// Build the export table used for resolution.
#include "xenia/kernel/util/export_table_pre.inc"
static constexpr xe::cpu::Export xam_export_table[] = {
#include "xenia/kernel/xam/xam_table.inc"
};
#include "xenia/kernel/util/export_table_post.inc"
void XamModule::RegisterExportTable(xe::cpu::ExportResolver* export_resolver) {
  XAM_BREADCRUMB("BREADCRUMB: XamModule::RegisterExportTable entered\n");
  assert_not_null(export_resolver);
  auto& xam_exports = get_xam_exports();
  XAM_BREADCRUMB(
      "BREADCRUMB: XamModule::RegisterExportTable iterating %zu exports\n",
      xe::countof(xam_export_table));

  for (size_t i = 0; i < xe::countof(xam_export_table); ++i) {
    auto& export_entry = xam_export_table[i];
    XAM_BREADCRUMB(
        "BREADCRUMB: XamModule::RegisterExportTable loop i=%zu ordinal=%u "
        "name=%s\n",
        i, export_entry.ordinal,
        export_entry.name ? export_entry.name : "NULL");
    assert_true(export_entry.ordinal < xam_exports.size());
    if (!xam_exports[export_entry.ordinal]) {
      xam_exports[export_entry.ordinal] =
          const_cast<xe::cpu::Export*>(&export_entry);
      XAM_BREADCRUMB(
          "BREADCRUMB: XamModule::RegisterExportTable stored export at ordinal "
          "%u\n",
          export_entry.ordinal);
    }
  }
  XAM_BREADCRUMB(
      "BREADCRUMB: XamModule::RegisterExportTable calling RegisterTable\n");
  export_resolver->RegisterTable("xam.xex", &get_xam_exports());
  XAM_BREADCRUMB("BREADCRUMB: XamModule::RegisterExportTable done\n");
}

XamModule::~XamModule() {}

void XamModule::LoadLoaderData() {
  FILE* file = xe::filesystem::OpenFile(kXamModuleLoaderDataFileName, "rb");

  if (!file) {
    loader_data_.launch_data_present = false;
    return;
  }

  loader_data_.launch_data_present = true;

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

void XamModule::SaveLoaderData() {
  FILE* file = xe::filesystem::OpenFile(kXamModuleLoaderDataFileName, "wb");

  if (!file) {
    return;
  }

  std::filesystem::path host_path = loader_data_.host_path;
  std::string launch_path = loader_data_.launch_path;

  auto remove_prefix = [&launch_path](std::string_view prefix) {
    if (launch_path.compare(0, prefix.length(), prefix) == 0) {
      launch_path = launch_path.substr(prefix.length());
    }
  };

  remove_prefix("game:\\");
  remove_prefix("d:\\");

  if (host_path.extension() == ".xex") {
    host_path.remove_filename();
    host_path = host_path / launch_path;
    launch_path = "";
  }

  const std::string host_path_as_string = xe::path_to_utf8(host_path);
  const uint16_t host_path_length =
      static_cast<uint16_t>(host_path_as_string.size());

  fwrite(&host_path_length, sizeof(host_path_length), 1, file);
  fwrite(host_path_as_string.c_str(), host_path_length, 1, file);

  const uint16_t launch_path_length = static_cast<uint16_t>(launch_path.size());
  fwrite(&launch_path_length, sizeof(launch_path_length), 1, file);
  fwrite(launch_path.c_str(), launch_path_length, 1, file);

  fwrite(&loader_data_.launch_flags, sizeof(loader_data_.launch_flags), 1,
         file);

  const uint16_t launch_data_size =
      static_cast<uint16_t>(loader_data_.launch_data.size());
  fwrite(&launch_data_size, sizeof(launch_data_size), 1, file);

  fwrite(loader_data_.launch_data.data(), launch_data_size, 1, file);

  fclose(file);
}

#undef XAM_BREADCRUMB

}  // namespace xam
}  // namespace kernel
}  // namespace xe
