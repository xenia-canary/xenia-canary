/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/object_table.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace util {

ObjectTable::ObjectTable() {}

ObjectTable::~ObjectTable() { Reset(); }

void ObjectTable::Reset() {
  std::lock_guard<xe::spinlock> lock(spinlock_);

  host_object_table_.Reset();
  for (auto& [_, table] : guest_object_table_) {
    table.Reset();
  }
}

uint32_t ObjectTable::GetFirstFreeSlot(
    ObjectTable::ObjectTableInfo* const table) {
  // Check if
  if (!table->freed_table_slots_.empty()) {
    uint32_t slot = table->freed_table_slots_.front();
    table->freed_table_slots_.erase(table->freed_table_slots_.begin());
    return slot;
  }
  // Check if latest used slot is free again.
  ObjectTableEntry& entry = table->table_[table->previous_free_slot_];
  if (!entry.object) {
    return table->previous_free_slot_;
  }

  if (++table->previous_free_slot_ >= table->table_.size()) {
    table->table_.reserve(table->table_.size() * 2);
  }

  return table->previous_free_slot_;
}

X_STATUS ObjectTable::FindFreeSlot(const XObject* const object,
                                   uint32_t* out_slot) {
  auto object_table = GetTableForObject(object);

  *out_slot = GetFirstFreeSlot(object_table);
  return X_STATUS_SUCCESS;
}

ObjectTable::ObjectTableInfo* const ObjectTable::GetTableForObject(
    const XObject* const obj) {
  if (obj->is_host_object()) {
    return &host_object_table_;
  }

  if (obj->type() == XObject::Type::Thread) {
    // Switcharoo for title/system thread!
    return &guest_object_table_[kGuestHandleTitleThreadBase];
  }

  return &guest_object_table_[kGuestHandleBase];
}

ObjectTable::ObjectTableInfo* const ObjectTable::GetTableForObject(
    const X_HANDLE handle) {
  if (!handle) {
    return nullptr;
  }

  if (handle & 0x00FF0000) {
    return &guest_object_table_[kGuestHandleBase];
  }

  const uint32_t handle_mask = handle & 0xFF000000;

  if (handle_mask == kGuestHandleTitleThreadBase) {
    return &guest_object_table_[kGuestHandleTitleThreadBase];
  }
  if (handle_mask == kGuestHandleSystemThreadBase) {
    return &guest_object_table_[kGuestHandleSystemThreadBase];
  }

  // Only host check remains
  return &host_object_table_;
}

X_STATUS ObjectTable::AddHandle(XObject* object, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  uint32_t handle = 0;

  std::lock_guard<xe::spinlock> lock(spinlock_);

  auto table = GetTableForObject(object);
  uint32_t slot = GetFirstFreeSlot(table);

  ObjectTableEntry& entry = table->table_[slot];

  entry.object = object;
  entry.handle_ref_count = 1;
  handle = table->GetSlotHandle(slot);
  object->handles().push_back(handle);

  // Retain so long as the object is in the table.
  object->Retain();

  XELOGI("Added handle:{:08X} for {}", handle, typeid(*object).name());

  if (out_handle) {
    *out_handle = handle;
  }

  return result;
}

X_STATUS ObjectTable::DuplicateHandle(X_HANDLE handle, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;
  handle = TranslateHandle(handle);

  std::lock_guard<xe::spinlock> lock(spinlock_);
  // For whatever reason all duplicates are going into base mask even threads.
  auto table = &guest_object_table_[kGuestHandleBase];
  uint32_t slot = GetFirstFreeSlot(table);

  XObject* object = LookupObject(handle, false);

  if (object) {
    ObjectTableEntry& entry = table->table_[slot];
    entry.object = object;
    entry.handle_ref_count = 1;
    *out_handle = table->GetSlotHandle(slot);
    object->handles().push_back(*out_handle);

    // Retain so long as the object is in the table.
    object->Retain();

    XELOGI("Duplicated handle:{:08X} to {:08X} for {}", handle, *out_handle,
           typeid(*object).name());
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  return result;
}

X_STATUS ObjectTable::RetainHandle(X_HANDLE handle) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return X_STATUS_INVALID_HANDLE;
  }

  ObjectTableEntry* entry = LookupTable(handle);

  std::lock_guard<xe::spinlock> lock(spinlock_);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  entry->handle_ref_count++;
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::ReleaseHandle(X_HANDLE handle) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return X_STATUS_INVALID_HANDLE;
  }

  std::lock_guard<xe::spinlock> lock(spinlock_);
  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (--entry->handle_ref_count == 0) {
    lock.~lock_guard();
    // No more references. Remove it from the table.
    return RemoveHandle(handle);
  }

  // FIXME: Return a status code telling the caller it wasn't released
  // (but not a failure code)
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::RemoveHandle(X_HANDLE handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  handle = TranslateHandle(handle);
  if (!handle) {
    return X_STATUS_INVALID_HANDLE;
  }

  std::lock_guard<xe::spinlock> lock(spinlock_);
  ObjectTableEntry* entry = LookupTable(handle);

  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (entry->object) {
    auto object = entry->object;
    entry->object = nullptr;
    assert_zero(entry->handle_ref_count);
    entry->handle_ref_count = 0;

    // Walk the object's handles and remove this one.
    auto handle_entry =
        std::find(object->handles().begin(), object->handles().end(), handle);
    if (handle_entry != object->handles().end()) {
      object->handles().erase(handle_entry);
    }

    auto table = GetTableForObject(handle);
    const uint32_t slot = table->GetHandleSlot(handle);
    table->freed_table_slots_.push_back(slot);

    XELOGI("Removed handle:{:08X} for {}", handle, typeid(*object).name());

    // Remove object name from mapping to prevent naming collision.
    if (!object->name().empty()) {
      RemoveNameMapping(object->name());
    }
    // Release now that the object has been removed from the table.
    object->Release();
  }
  return X_STATUS_SUCCESS;
}

ObjectTable::ObjectTableEntry* ObjectTable::LookupTable(X_HANDLE handle) {
  auto table = GetTableForObject(handle);
  if (!table) {
    return nullptr;
  }

  auto* entry = &table->table_[table->GetHandleSlot(handle)];

  if (!entry->object) {
    return nullptr;
  }
  return entry;
}

// Generic lookup
template <>
object_ref<XObject> ObjectTable::LookupObject<XObject>(X_HANDLE handle,
                                                       bool already_locked) {
  auto object = ObjectTable::LookupObject(handle, already_locked);
  auto result = object_ref<XObject>(reinterpret_cast<XObject*>(object));
  return result;
}

XObject* ObjectTable::LookupObject(X_HANDLE handle, bool already_locked) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return nullptr;
  }

  XObject* object = nullptr;
  auto entry = LookupTable(handle);
  if (!entry) {
    return nullptr;
  }

  // Retain the object pointer.
  if (entry->object) {
    entry->object->Retain();
  }

  return entry->object;
}

std::vector<object_ref<XObject>> ObjectTable::GetAllObjects() {
  std::vector<object_ref<XObject>> results;

  std::lock_guard<xe::spinlock> lock(spinlock_);
  for (auto& [_, table] : guest_object_table_) {
    for (uint32_t i = 0; i < table.previous_free_slot_ + 1; i++) {
      auto& object = table.table_.at(i).object;

      if (!object) {
        continue;
      }

      object->Retain();
      results.push_back(object_ref<XObject>(object));
    }
  }
  return results;
}

void ObjectTable::PurgeAllObjects() {
  std::lock_guard<xe::spinlock> lock(spinlock_);

  for (auto& [_, table] : guest_object_table_) {
    for (auto& [_, entry] : table.table_) {
      if (!entry.object) {
        continue;
      }

      entry.handle_ref_count = 0;
      entry.object->Release();
      entry.object = nullptr;
    }
  }
}

void ObjectTable::GetObjectsByType(XObject::Type type,
                                   std::vector<object_ref<XObject>>* results) {
  std::lock_guard<xe::spinlock> lock(spinlock_);

  if (type == XObject::Type::Thread) {
    for (auto& [_, entry] :
         guest_object_table_[kGuestHandleTitleThreadBase].table_) {
      if (entry.object) {
        entry.object->Retain();
        results->push_back(object_ref<XObject>(entry.object));
      }
    }
    return;
  }

  for (auto& [_, entry] : guest_object_table_[kGuestHandleBase].table_) {
    if (entry.object && entry.object->type() == type) {
      entry.object->Retain();
      results->push_back(object_ref<XObject>(entry.object));
    }
  }
}

X_HANDLE ObjectTable::TranslateHandle(X_HANDLE handle) const {
  // chrispy: reordered these by likelihood, most likely case is that handle is
  // not a special handle
  XE_LIKELY_IF(handle < 0xFFFFFFFE) { return handle; }
  else if (handle == 0xFFFFFFFF) {
    return 0;
  }
  else {
    return XThread::GetCurrentThreadHandle();
  }
}

// Name mapping is available only for guest objects!
X_STATUS ObjectTable::AddNameMapping(const std::string_view name,
                                     X_HANDLE handle) {
  std::lock_guard<xe::spinlock> lock(spinlock_);
  if (guest_name_table_.count(string_key_case(name))) {
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  guest_name_table_.insert({string_key_case::create(name), handle});
  return X_STATUS_SUCCESS;
}

void ObjectTable::RemoveNameMapping(const std::string_view name) {
  // Names are case-insensitive.
  auto it = guest_name_table_.find(string_key_case(name));
  if (it != guest_name_table_.end()) {
    guest_name_table_.erase(it);
  }
}

X_STATUS ObjectTable::GetObjectByName(const std::string_view name,
                                      X_HANDLE* out_handle) {
  // Names are case-insensitive.
  std::lock_guard<xe::spinlock> lock(spinlock_);
  auto it = guest_name_table_.find(string_key_case(name));
  if (it == guest_name_table_.end()) {
    *out_handle = X_INVALID_HANDLE_VALUE;
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  *out_handle = it->second;

  // We need to ref the handle. I think.
  auto obj = LookupObject(it->second, true);
  if (obj) {
    obj->RetainHandle();
    obj->Release();
  }

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Save(ByteStream* stream) {
  /*
  stream->Write<uint32_t>(host_table_capacity_);
  for (uint32_t i = 0; i < host_table_capacity_; i++) {
    auto& entry = host_table_[i];
    stream->Write<int32_t>(entry.handle_ref_count);
  }

  stream->Write<uint32_t>(table_capacity_);
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    stream->Write<int32_t>(entry.handle_ref_count);
  }
  */
  return true;
}

bool ObjectTable::Restore(ByteStream* stream) {
  /*
  Resize(stream->Read<uint32_t>(), true);
  for (uint32_t i = 0; i < host_table_capacity_; i++) {
    auto& entry = host_table_[i];
    // entry.object = nullptr;
    entry.handle_ref_count = stream->Read<int32_t>();
  }

  Resize(stream->Read<uint32_t>(), false);
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    // entry.object = nullptr;
    entry.handle_ref_count = stream->Read<int32_t>();
  }
  */
  return true;
}

X_STATUS ObjectTable::RestoreHandle(X_HANDLE handle, XObject* object) {
  /*
  const bool is_host_object = XObject::is_handle_host_object(handle);
  uint32_t slot = GetHandleSlot(handle, is_host_object);
  uint32_t capacity = is_host_object ? host_table_capacity_ : table_capacity_;
  assert_true(capacity >= slot);

  if (capacity >= slot) {
    auto& entry = is_host_object ? host_table_[slot] : table_[slot];
    entry.object = object;
    object->Retain();
  }
  */
  return X_STATUS_SUCCESS;
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
