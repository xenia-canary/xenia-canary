/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/stfs_metadata.h"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/string.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

namespace xe {
namespace vfs {

namespace {

std::string U16ToUtf8(const std::u16string& s) {
  if (s.empty()) {
    return {};
  }
  return xe::to_utf8(s);
}

}  // namespace

toml::table StfsMetadata::ToToml() const {
  toml::table root;

  toml::table content_table;
  content_table.insert("title_id", fmt::format("{:08X}", title_id));
  content_table.insert("media_id", fmt::format("{:08X}", media_id));
  content_table.insert("savegame_id", fmt::format("{:08X}", savegame_id));
  content_table.insert("content_type", fmt::format("{:08X}", content_type));
  content_table.insert("content_size", static_cast<int64_t>(content_size));
  content_table.insert("disc_number", static_cast<int64_t>(disc_number));
  content_table.insert("disc_count", static_cast<int64_t>(disc_count));
  root.insert("content_info", std::move(content_table));

  toml::table display_table;
  if (!display_name.empty()) {
    display_table.insert("display_name", display_name);
  }
  if (!description.empty()) {
    display_table.insert("description", description);
  }
  if (!publisher.empty()) {
    display_table.insert("publisher", publisher);
  }
  if (!title_name.empty()) {
    display_table.insert("title_name", title_name);
  }
  if (!display_table.empty()) {
    root.insert("display_info", std::move(display_table));
  }

  return root;
}

std::optional<StfsMetadata> ExtractStfsMetadata(
    const std::filesystem::path& path, XLanguage language) {
  auto header = XContentContainerDevice::ReadContainerHeader(path);
  if (!header) {
    return std::nullopt;
  }

  if (!header->content_header.is_magic_valid()) {
    return std::nullopt;
  }

  StfsMetadata metadata;
  const auto& content_meta = header->content_metadata;
  const auto& exec_info = content_meta.execution_info;

  metadata.title_id = exec_info.title_id;
  metadata.media_id = exec_info.media_id;
  metadata.savegame_id = exec_info.savegame_id;
  metadata.disc_number = exec_info.disc_number;
  metadata.disc_count = exec_info.disc_count;

  metadata.content_type = static_cast<uint32_t>(
      static_cast<XContentType>(content_meta.content_type));
  metadata.content_size = content_meta.content_size;

  // Tiered: requested language → English → first populated slot. Publishers
  // routinely misuse slots (e.g. Japanese text in the English slot for
  // region-locked games), so this is best-effort.
  auto pick_localized = [&content_meta, language](auto getter) {
    auto str = getter(content_meta, language);
    if (str.empty() && language != XLanguage::kEnglish) {
      str = getter(content_meta, XLanguage::kEnglish);
    }
    if (str.empty()) {
      for (uint32_t i = uint32_t(XLanguage::kEnglish);
           i < uint32_t(XLanguage::kMaxLanguages); ++i) {
        auto candidate = static_cast<XLanguage>(i);
        if (candidate == language || candidate == XLanguage::kEnglish) {
          continue;
        }
        auto alt = getter(content_meta, candidate);
        if (!alt.empty()) {
          str = std::move(alt);
          break;
        }
      }
    }
    return U16ToUtf8(str);
  };
  metadata.display_name = pick_localized(
      [](const auto& m, XLanguage l) { return m.display_name(l); });
  metadata.description = pick_localized(
      [](const auto& m, XLanguage l) { return m.description(l); });
  metadata.publisher = U16ToUtf8(content_meta.publisher());
  metadata.title_name = U16ToUtf8(content_meta.title_name());

  const uint32_t thumb_size = content_meta.title_thumbnail_size;
  if (thumb_size > 0 && thumb_size <= XContentMetadata::kThumbLengthV1) {
    metadata.icon_data.assign(content_meta.title_thumbnail,
                              content_meta.title_thumbnail + thumb_size);
  }

  return metadata;
}

}  // namespace vfs
}  // namespace xe
