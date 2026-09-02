/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/entry_table.h"

#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"

namespace xe {
namespace cpu {

EntryTable::EntryTable() = default;

EntryTable::~EntryTable() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& pair : map_) {
    delete pair.second;
  }
}

Entry* EntryTable::Get(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = map_.find(address);
  if (it == map_.end()) {
    return nullptr;
  }
  Entry* entry = it->second;
  if (entry) {
    if (entry->status != Entry::STATUS_READY) {
      entry = nullptr;
    }
  }
  return entry;
}

Entry::Status EntryTable::GetOrCreate(uint32_t address, Entry** out_entry) {
  auto global_lock = global_critical_region_.Acquire();

  auto it = map_.find(address);
  Entry* entry = (it != map_.end()) ? it->second : nullptr;
  Entry::Status status;
  if (entry) {
    // If we aren't ready yet spin and wait.
    if (entry->status == Entry::STATUS_COMPILING) {
      do {
        global_lock.unlock();
        xe::threading::MaybeYield();
        global_lock.lock();
      } while (entry->status == Entry::STATUS_COMPILING);
    }
    status = entry->status;
  } else {
    // Create and return for initialization.
    entry = new Entry();
    entry->address = address;
    entry->end_address = 0;
    entry->status = Entry::STATUS_COMPILING;
    entry->function = nullptr;
    map_[address] = entry;
    status = Entry::STATUS_NEW;
  }
  global_lock.unlock();
  *out_entry = entry;
  return status;
}

void EntryTable::MarkReady(Entry* entry, Function* function,
                           uint32_t end_address) {
  auto global_lock = global_critical_region_.Acquire();
  entry->function = function;
  entry->end_address = end_address;
  entry->status = Entry::STATUS_READY;
}

void EntryTable::MarkFailed(Entry* entry) {
  auto global_lock = global_critical_region_.Acquire();
  entry->status = Entry::STATUS_FAILED;
}

void EntryTable::Delete(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = map_.find(address);
  if (it != map_.end()) {
    delete it->second;
    map_.erase(it);
  }
}

std::vector<Function*> EntryTable::FindWithAddress(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<Function*> fns;
  for (const auto& pair : map_) {
    Entry* entry = pair.second;
    if (address >= entry->address && address <= entry->end_address) {
      if (entry->status == Entry::STATUS_READY && entry->function) {
        fns.push_back(entry->function);
      }
    }
  }
  return fns;
}
}  // namespace cpu
}  // namespace xe
