/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_compat_db.h"

#include <string_view>
#include <unordered_map>

#include "rapidjson/document.h"

#include "xenia/base/logging.h"
#include "xenia/patcher/embedded_bundle.h"

#include "embedded_bundle_game_compat.h"

namespace xe {
namespace app {

namespace {

CompatState ParseState(std::string_view s) {
  if (s == "Playable") {
    return CompatState::kPlayable;
  }
  if (s == "Gameplay") {
    return CompatState::kGameplay;
  }
  if (s == "Loads") {
    return CompatState::kLoads;
  }
  if (s == "Unplayable") {
    return CompatState::kUnplayable;
  }
  return CompatState::kUnknown;
}

uint32_t ParseTitleId(const char* hex, size_t len) {
  if (len != 8) {
    return 0;
  }
  uint32_t v = 0;
  for (size_t i = 0; i < len; ++i) {
    char c = hex[i];
    uint32_t d;
    if (c >= '0' && c <= '9') {
      d = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return 0;
    }
    v = (v << 4) | d;
  }
  return v;
}

const std::unordered_map<uint32_t, CompatState>& GetCompatIndex() {
  static const std::unordered_map<uint32_t, CompatState> index = []() {
    std::unordered_map<uint32_t, CompatState> map;
    xe::patcher::EmbeddedBundle bundle(
        xe::embedded_bundle_game_compat::kBundleData,
        xe::embedded_bundle_game_compat::kBundleSize);
    if (!bundle.ok()) {
      XELOGE("CompatDb: bundle decompress failed");
      return map;
    }
    // Merge canary and stable: most-optimistic state across all sources wins.
    bundle.ForEach([&](std::string_view name, std::string_view data) {
      if (name != "canary.json" && name != "stable.json") {
        return;
      }
      rapidjson::Document doc;
      doc.Parse(data.data(), data.size());
      if (doc.HasParseError() || !doc.IsArray()) {
        XELOGE("CompatDb: {} parse failed", name);
        return;
      }
      for (const auto& entry : doc.GetArray()) {
        if (!entry.IsObject()) {
          continue;
        }
        auto id_it = entry.FindMember("id");
        auto state_it = entry.FindMember("state");
        if (id_it == entry.MemberEnd() || state_it == entry.MemberEnd()) {
          continue;
        }
        if (!id_it->value.IsString() || !state_it->value.IsString()) {
          continue;
        }
        uint32_t title_id = ParseTitleId(id_it->value.GetString(),
                                         id_it->value.GetStringLength());
        if (title_id == 0) {
          continue;
        }
        CompatState s = ParseState(std::string_view(
            state_it->value.GetString(), state_it->value.GetStringLength()));
        auto& slot = map[title_id];
        if (static_cast<uint8_t>(s) > static_cast<uint8_t>(slot)) {
          slot = s;
        }
      }
    });
    return map;
  }();
  return index;
}

}  // namespace

CompatState GetCompatState(uint32_t title_id) {
  if (title_id == 0) {
    return CompatState::kUnknown;
  }
  const auto& idx = GetCompatIndex();
  auto it = idx.find(title_id);
  if (it == idx.end()) {
    return CompatState::kUnknown;
  }
  return it->second;
}

}  // namespace app
}  // namespace xe
