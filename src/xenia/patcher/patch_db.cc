/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/memory.h"

#include "xenia/patcher/patch_db.h"

namespace xe {
namespace patcher {

PatchDB::PatchDB(const std::filesystem::path patches_root) {
  patches_root_ = patches_root;
  LoadPatches();
}

PatchDB::~PatchDB() {}

void PatchDB::LoadPatches() {
  const std::filesystem::path patches_directory = patches_root_ / "patches";
  const std::vector<xe::filesystem::FileInfo> patch_files =
      filesystem::ListFiles(patches_directory);

  for (const xe::filesystem::FileInfo& patch_file : patch_files) {
    // Skip files that doesn't have only title_id as name and .patch as
    // extension
    if (!std::regex_match(path_to_utf8(patch_file.name),
                          patch_filename_regex_)) {
      XELOGE("PatchDB: Skipped loading file {} due to incorrect filename",
             patch_file.name);
      continue;
    }

    const PatchFileEntry loaded_title_patches =
        ReadPatchFile(patch_file.path / patch_file.name);
    if (loaded_title_patches.title_id != -1) {
      loaded_patches_.push_back(loaded_title_patches);
    }
  }
  XELOGI("PatchDB: Loaded patches for {} titles", loaded_patches_.size());
}

PatchFileEntry PatchDB::ReadPatchFile(
    const std::filesystem::path& file_path) const {
  PatchFileEntry patch_file;
  toml::parse_result patch_toml_fields;

  try {
    patch_toml_fields = ParseFile(file_path);
  } catch (...) {
    XELOGE("PatchDB: Cannot load patch file: {}", file_path.filename());
    patch_file.title_id = -1;
    return patch_file;
  };

  auto title_name = patch_toml_fields.get_as<std::string>("title_name");
  auto title_id = patch_toml_fields.get_as<std::string>("title_id");
  auto hashes_node = patch_toml_fields.get("hash");

  if (!title_name || !title_id || !hashes_node) {
    XELOGE("PatchDB: Cannot load patch file: {}", file_path.filename());
    patch_file.title_id = -1;
    return patch_file;
  }

  patch_file.title_id = strtoul(title_id->get().c_str(), NULL, 16);
  patch_file.title_name = title_name->get();
  ReadHashes(patch_file, hashes_node);

  const auto patch_array = patch_toml_fields.get_as<toml::array>("patch");
  if (!patch_array) {
    return patch_file;
  }

  for (const auto& patch_entry : *patch_array) {
    if (!patch_entry.is_table()) {
      continue;
    }

    PatchInfoEntry patch = PatchInfoEntry();
    ReadPatchHeader(patch, patch_entry.as_table());
    patch_file.patch_info.push_back(patch);
  }
  return patch_file;
}

bool PatchDB::ReadPatchData(std::vector<PatchDataEntry>& patch_data,
                            const std::pair<std::string, PatchData> data_type,
                            const toml::table* patch_fields) const {
  auto patch_data_fields = patch_fields->get_as<toml::array>(data_type.first);
  if (!patch_data_fields) {
    return true;
  }

  for (const auto& patch_data_table : *patch_data_fields) {
    if (!patch_data_table.is_table()) {
      continue;
    }

    const auto table = patch_data_table.as_table();
    const auto address_field = table->get_as<int64_t>("address");
    if (!address_field) {
      XELOGW("PatchDB: {} entry has no integer address! Skipping",
             data_type.first);
      return false;
    }
    const auto address = static_cast<uint32_t>(address_field->get());
    size_t alloc_size = (size_t)data_type.second.size;

    const auto value_field = table->get("value");
    if (!value_field) {
      XELOGW("PatchDB: {} entry for address {:08X} has no value! Skipping",
             data_type.first, address);
      return false;
    }

    const auto int_value = value_field->value_exact<int64_t>();
    const auto string_value = value_field->value_exact<std::string>();

    switch (data_type.second.type) {
      case PatchDataType::kBE8:
      case PatchDataType::kBE16:
      case PatchDataType::kBE32:
      case PatchDataType::kBE64:
        if (!int_value) {
          XELOGW(
              "PatchDB: {} value for address {:08X} is not an integer! "
              "Skipping",
              data_type.first, address);
          return false;
        }
        break;
      case PatchDataType::kString:
      case PatchDataType::kU16String:
      case PatchDataType::kByteArray:
        if (!string_value) {
          XELOGW(
              "PatchDB: {} value for address {:08X} is not a string! Skipping",
              data_type.first, address);
          return false;
        }
        break;
      default:
        break;
    }

    switch (data_type.second.type) {
      case PatchDataType::kBE8: {
        uint16_t value = static_cast<uint16_t>(*int_value);
        patch_data.push_back({address, PatchDataValue(alloc_size, value)});
        break;
      }
      case PatchDataType::kBE16: {
        uint16_t value = static_cast<uint16_t>(*int_value);
        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kBE32: {
        uint32_t value = static_cast<uint32_t>(*int_value);
        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kBE64: {
        uint64_t value = static_cast<uint64_t>(*int_value);
        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kF64: {
        double value = 0.0;
        if (value_field->is_floating_point()) {
          value = value_field->as_floating_point()->get();
        }
        if (value_field->is_integer()) {
          value = static_cast<double>(value_field->as_integer()->get());
        }

        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kF32: {
        float value = 0.0f;
        if (value_field->is_floating_point()) {
          value = static_cast<float>(value_field->as_floating_point()->get());
        }
        if (value_field->is_integer()) {
          value = static_cast<float>(value_field->as_integer()->get());
        }

        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kString: {
        patch_data.push_back({address, PatchDataValue(*string_value)});
        break;
      }
      case PatchDataType::kU16String: {
        std::u16string value = xe::to_utf16(*string_value);
        patch_data.push_back({address, PatchDataValue(value)});
        break;
      }
      case PatchDataType::kByteArray: {
        std::vector<uint8_t> data;
        const std::string& value = *string_value;

        bool success = string_util::hex_string_to_array(data, value);
        if (!success) {
          XELOGW("PatchDB: Cannot convert hex string to byte array! Skipping",
                 address);
          return false;
        }
        patch_data.push_back({address, PatchDataValue(data)});
        break;
      }
      default: {
        XELOGW("PatchDB: Unknown patch data type for address {:08X}! Skipping",
               address);
        return false;
      }
    }
  }
  return true;
}

std::vector<PatchFileEntry> PatchDB::GetTitlePatches(
    const uint32_t title_id, const std::optional<uint64_t> hash) {
  std::vector<PatchFileEntry> title_patches;

  std::ranges::copy_if(
      std::as_const(loaded_patches_), std::back_inserter(title_patches),
      [=](const PatchFileEntry entry) {
        bool hash_exist =
            std::ranges::find(entry.hashes, hash) != entry.hashes.cend();

        return entry.title_id == title_id && hash_exist;
      });

  return title_patches;
}

void PatchDB::ReadHashes(PatchFileEntry& patch_entry,
                         const toml::node* hashes_node) const {
  auto add_hash = [&patch_entry](const toml::node* hash_node) {
    if (!hash_node->is_string()) {
      return;
    }

    const auto string_hash = hash_node->as_string()->get();
    if (string_hash.empty()) {
      return;
    }

    patch_entry.hashes.push_back(
        xe::string_util::from_string<uint64_t>(string_hash, true));
  };

  if (hashes_node->is_value()) {
    add_hash(hashes_node);
  }

  if (hashes_node->is_array()) {
    for (const auto& hash_entry : *hashes_node->as_array()) {
      add_hash(&hash_entry);
    }
  }
}

void PatchDB::ReadPatchHeader(PatchInfoEntry& patch_info,
                              const toml::table* patch_fields) const {
  const toml::table& fields = *patch_fields;

  patch_info.id = 0;  // Todo(Gliniak): Implement id for future GUI stuff
  patch_info.patch_name =
      fields["name"].value_exact<std::string>().value_or("");
  patch_info.patch_desc =
      fields["desc"].value_exact<std::string>().value_or("");
  patch_info.patch_author =
      fields["author"].value_exact<std::string>().value_or("");
  patch_info.is_enabled =
      fields["is_enabled"].value_exact<bool>().value_or(false);

  // Iterate through all available data sizes
  for (const auto& patch_data_type : patch_data_types_size_) {
    if (!ReadPatchData(patch_info.patch_data, patch_data_type, patch_fields)) {
      XELOGE("PatchDB: Cannot read patch {}", patch_info.patch_name);
      break;
    }
  }
}

}  // namespace patcher
}  // namespace xe
