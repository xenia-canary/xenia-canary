/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdio>
#include <vector>
#include "xenia/cpu/export_resolver.h"

namespace xe {
namespace kernel {
namespace xbdm {

// Test function to validate the export table BEFORE using it
bool ValidateXbdmExportTable() {
  printf("[VALIDATOR] Validating xbdm export table \n");
  
  // Include the table just like in the real code
  #include "xenia/kernel/util/export_table_pre.inc"
  
  printf("[VALIDATOR] Creating test export array...\n");
  
  // Create a test array
  static xe::cpu::Export test_export_table[] = {
  #include "xenia/kernel/xbdm/xbdm_table.inc"
  };
  
  #include "xenia/kernel/util/export_table_post.inc"
  
  size_t count = 0;
  for (size_t i = 0; i < sizeof(test_export_table) / sizeof(test_export_table[0]); ++i) {
    count++;
  }
  
  printf("[VALIDATOR] Table has %zu entries\n", count);
  
  // Check for issues
  for (size_t i = 0; i < count; ++i) {
    auto& entry = test_export_table[i];
    printf("[VALIDATOR] Entry %zu: ordinal=%u, name='%s'\n", 
           i, entry.ordinal, entry.name ? entry.name : "NULL");
    
    if (!entry.name) {
      printf("[VALIDATOR] ERROR: Entry %zu has NULL name!\n", i);
      return false;
    }
    
    if (entry.ordinal == 0) {
      printf("[VALIDATOR] WARNING: Entry %zu has ordinal 0\n", i);
    }
  }
  
  printf("[VALIDATOR] Validation complete \n");
  return true;
}

}  // namespace xbdm
}  // namespace kernel
}  // namespace xe
