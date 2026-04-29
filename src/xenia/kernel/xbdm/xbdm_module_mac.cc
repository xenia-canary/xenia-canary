/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xbdm/xbdm_module.h"

#include <cstdio>

#include "xenia/base/cvar.h"
#include "xenia/base/math_mac.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/xbdm/xbdm_private.h"

namespace xe {
namespace kernel {
namespace xbdm {

DEFINE_bool(
    xbdm_trace_export_registration, false,
    "Trace verbose XBDM export registration diagnostics (very noisy).",
    "Kernel");

#define XBDM_TRACE(...)                                      \
  do {                                                       \
    if (cvars::xbdm_trace_export_registration) {             \
      std::printf(__VA_ARGS__);                              \
    }                                                        \
  } while (0)

XbdmModule::XbdmModule(Emulator* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xbdm.xex") {
  XBDM_TRACE("[DEBUG] ===== XbdmModule constructor START =====\n");

  try {
    XBDM_TRACE("[DEBUG] Step 1: Calling RegisterExportTable...\n");
    RegisterExportTable(export_resolver_);
    XBDM_TRACE("[DEBUG] Step 1: RegisterExportTable completed successfully\n");

    // Register all exported functions.
    XBDM_TRACE("[DEBUG] Step 2: Registering individual export groups...\n");
#define XE_MODULE_EXPORT_GROUP(m, n) \
    XBDM_TRACE("[DEBUG]   About to call Register" #n "Exports\n"); \
    try { \
      Register##n##Exports(export_resolver_, kernel_state_); \
      XBDM_TRACE("[DEBUG]   Register" #n "Exports completed successfully\n"); \
    } catch (const std::exception& e) { \
      XBDM_TRACE("[DEBUG]   EXCEPTION in Register" #n "Exports: %s\n", e.what()); \
      throw; \
    }
#include "xbdm_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

    XBDM_TRACE("[DEBUG] ===== XbdmModule constructor COMPLETE =====\n");
  } catch (const std::bad_array_new_length& e) {
    XBDM_TRACE("[DEBUG] CRITICAL: std::bad_array_new_length exception: %s\n", e.what());
    XBDM_TRACE("[DEBUG] This is a memory allocation error - likely new[] with bad size\n");
    throw;
  } catch (const std::exception& e) {
    XBDM_TRACE("[DEBUG] Exception in XbdmModule constructor: %s\n", e.what());
    throw;
  } catch (...) {
    XBDM_TRACE("[DEBUG] Unknown exception in XbdmModule constructor\n");
    throw;
  }
}

// Use a static function to return the exports vector to avoid static initialization order issues
// This pattern is used in xboxkrnl_module.cc and xam_module.cc
static auto& get_xbdm_exports() {
  static std::vector<xe::cpu::Export*> xbdm_exports(4096);
  XBDM_TRACE("[DEBUG] get_xbdm_exports() called, vector size: %zu\n", xbdm_exports.size());
  return xbdm_exports;
}

xe::cpu::Export* RegisterExport_xbdm(xe::cpu::Export* export_entry) {
  XBDM_TRACE("[DEBUG] RegisterExport_xbdm called\n");
  XBDM_TRACE("[DEBUG] export_entry: %p\n", static_cast<void*>(export_entry));
  XBDM_TRACE("[DEBUG] export_entry->name: %s\n", export_entry->name);
  XBDM_TRACE("[DEBUG] export_entry->ordinal: %u\n", export_entry->ordinal);

  auto& xbdm_exports = get_xbdm_exports();

  XBDM_TRACE("[DEBUG] xbdm_exports.size(): %zu\n", xbdm_exports.size());
  XBDM_TRACE("[DEBUG] Assertion check: ordinal (%u) < size (%zu)? %s\n",
         export_entry->ordinal, xbdm_exports.size(),
         export_entry->ordinal < xbdm_exports.size() ? "YES" : "NO");

  assert_true(export_entry->ordinal < xbdm_exports.size());

  XBDM_TRACE("[DEBUG] Assertion passed, proceeding...\n");
  XBDM_TRACE("[DEBUG] Current slot at ordinal %u: %p\n", export_entry->ordinal,
         static_cast<void*>(xbdm_exports[export_entry->ordinal]));

  if (!xbdm_exports[export_entry->ordinal]) {
    XBDM_TRACE("[DEBUG] Slot empty, registering new export\n");
    xbdm_exports[export_entry->ordinal] = export_entry;
    XBDM_TRACE("Registered export %s at ordinal %d\n", export_entry->name,
           export_entry->ordinal);
  }
  else if (xbdm_exports[export_entry->ordinal] != export_entry) {
    XBDM_TRACE("[DEBUG] Slot occupied with different export\n");
    XBDM_TRACE("[DEBUG] Existing: %p, New: %p\n",
           static_cast<void*>(xbdm_exports[export_entry->ordinal]),
           static_cast<void*>(export_entry));
    XBDM_TRACE("Duplicate export %s at ordinal %d\n", export_entry->name,
           export_entry->ordinal);
  }
  else {
    XBDM_TRACE("[DEBUG] Slot occupied with same export (re-registering)\n");
    XBDM_TRACE("Re-registering export %s at ordinal %d\n", export_entry->name,
           export_entry->ordinal);
  }

  XBDM_TRACE("[DEBUG] Final assignment: ordinal %u = %p\n", export_entry->ordinal,
         static_cast<void*>(export_entry));
  xbdm_exports[export_entry->ordinal] = export_entry;
  XBDM_TRACE("[DEBUG] RegisterExport_xbdm returning: %p\n\n",
             static_cast<void*>(export_entry));
  return export_entry;
}

void XbdmModule::RegisterExportTable(xe::cpu::ExportResolver* export_resolver) {
  XBDM_TRACE("[DEBUG] ===== RegisterExportTable ENTER =====\n");
  XBDM_TRACE("[DEBUG] export_resolver pointer: %p\n",
             static_cast<void*>(export_resolver));
  assert_not_null(export_resolver);

  XBDM_TRACE("[DEBUG] RegisterExportTable starting\n");

  auto& xbdm_exports = get_xbdm_exports();

  XBDM_TRACE("[DEBUG] After get_xbdm_exports(), xbdm_exports.size(): %zu\n", xbdm_exports.size());
  XBDM_TRACE("[DEBUG] xbdm_exports vector address: %p\n",
             static_cast<void*>(&xbdm_exports));

  XBDM_TRACE("[DEBUG] ===== PHASE 1: Creating static array =====\n");
  XBDM_TRACE("[DEBUG] About to create xbdm_export_table array\n");

  // Build the export table used for resolution.
  #include "xenia/kernel/util/export_table_pre.inc"
  static xe::cpu::Export xbdm_export_table[] = {
  #include "xenia/kernel/xbdm/xbdm_table.inc"
  };
  #include "xenia/kernel/util/export_table_post.inc"

  XBDM_TRACE("[DEBUG] xbdm_export_table created, count: %zu\n", xe::countof(xbdm_export_table));

  if (xe::countof(xbdm_export_table) > 0) {
    XBDM_TRACE("[DEBUG] First export ordinal: %u, name: %s\n",
           xbdm_export_table[0].ordinal, xbdm_export_table[0].name ? xbdm_export_table[0].name : "NULL");
  } else {
    XBDM_TRACE("[DEBUG] WARNING: xbdm_export_table is EMPTY!\n");
  }

  XBDM_TRACE("[DEBUG] xbdm_export_table populated with %zu entries\n",
         xe::countof(xbdm_export_table));
  XBDM_TRACE("[DEBUG] xbdm_export_table address: %p\n",
             static_cast<void*>(xbdm_export_table));

  XBDM_TRACE("[DEBUG] ===== PHASE 2: Registering exports =====\n");
  for (size_t i = 0; i < xe::countof(xbdm_export_table); ++i) {
    auto& export_entry = xbdm_export_table[i];
    XBDM_TRACE("[DEBUG] Loop %zu: name='%s', ordinal=%u\n",
           i, export_entry.name ? export_entry.name : "NULL", export_entry.ordinal);

    // Add extra validation
    if (!export_entry.name) {
      XBDM_TRACE("[DEBUG] ERROR: export_entry.name is NULL at index %zu!\n", i);
    }

    if (export_entry.ordinal >= xbdm_exports.size()) {
      XBDM_TRACE("[DEBUG] ERROR: ordinal %u >= vector size %zu!\n",
             export_entry.ordinal, xbdm_exports.size());
      assert_true(export_entry.ordinal < xbdm_exports.size());
    }

    if (!xbdm_exports[export_entry.ordinal]) {
      XBDM_TRACE("[DEBUG]   Slot empty, storing export pointer at ordinal %u\n", export_entry.ordinal);
      xbdm_exports[export_entry.ordinal] = &export_entry;
      XBDM_TRACE("[DEBUG]   Stored export %p at slot %u\n",
                 static_cast<void*>(&export_entry), export_entry.ordinal);
    }
    else {
      XBDM_TRACE("[DEBUG]   Slot %u already occupied with %p\n",
             export_entry.ordinal,
             static_cast<void*>(xbdm_exports[export_entry.ordinal]));
    }
  }

  XBDM_TRACE("[DEBUG] ===== PHASE 3: Final registration =====\n");
  XBDM_TRACE("[DEBUG] About to call RegisterTable with xbdm_exports\n");
  XBDM_TRACE("[DEBUG] xbdm_exports reference address: %p\n",
             static_cast<void*>(&xbdm_exports));

  try {
    export_resolver->RegisterTable("xbdm.xex", &xbdm_exports);
    XBDM_TRACE("[DEBUG] RegisterTable call completed successfully\n");
  } catch (const std::exception& e) {
    XBDM_TRACE("[DEBUG] EXCEPTION in export_resolver->RegisterTable: %s\n", e.what());
    throw;
  }

  XBDM_TRACE("[DEBUG] ===== RegisterExportTable EXIT =====\n");
}

XbdmModule::~XbdmModule() {}

}  // namespace xbdm
}  // namespace kernel
}  // namespace xe
