/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_GAME_INFO_DATABASE_H_
#define XENIA_KERNEL_UTIL_GAME_INFO_DATABASE_H_

#include <set>
#include <string>
#include <vector>

#include "xenia/kernel/util/xlast.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"

namespace xe {
namespace kernel {
namespace util {

class GameInfoDatabase {
 public:
  struct PropertyBag {
    std::set<uint32_t> contexts;
    std::set<uint32_t> properties;
  };

  struct Context {
    uint32_t id;
    uint32_t max_value;
    uint32_t default_value;
    bool is_system;
    bool is_presence;
    std::string description;
  };

  struct Property {
    uint32_t id;
    uint32_t data_size;
    bool is_system;
    bool is_presence;
    std::string description;
  };

  struct Achievement {
    uint32_t id;
    std::string label;
    std::string description;
    std::string unachieved_description;
    uint32_t image_id;
    uint32_t gamerscore;
    uint32_t flags;
  };

  struct PresenceMode {
    uint32_t context_value;
    PropertyBag property_bag;
  };

  struct Presence {
    PropertyBag property_bag;
    std::vector<PresenceMode> presence_modes;
  };

  struct Query {
    uint32_t id;
    std::string name;
    std::vector<uint32_t> input_parameters;
    std::vector<uint32_t> filters;
    std::vector<uint32_t> expected_return;
  };

  struct Filter {
    uint32_t left_id;
    uint32_t right_id;
    std::string comparation_operator;
  };

  struct Field {
    uint32_t property_id;
    uint32_t flags;
    uint16_t attribute_id;
    uint16_t aggregation_type;
    uint8_t ordinal;
    uint8_t field_type;
    uint32_t format_type;
    std::string name;
  };

  struct SharedView {
    std::vector<Field> column_entries;
    std::vector<Field> row_entries;
    PropertyBag properties;
  };

  struct View {
    uint32_t id;
    bool arbitrated;
    bool hidden;
    bool team_view;
    bool online_only;
    bool skilled;
    kernel::xam::ViewType view_type;
    uint16_t shared_index;
    std::string name;
  };

  struct StatsView {
    View view;
    SharedView shared_view;
  };

  struct ProductInformation {
    uint32_t max_offline_players_count;
    uint32_t max_systemlink_players_count;
    uint32_t max_live_players_count;
    std::string publisher_name;
    std::string developer_name;
    std::string marketing_info;
    std::string genre_description;
    std::vector<std::string> features;
  };

  // Normally titles have at least XDBF file embedded into xex. There are
  // certain exceptions and that's why we need to check if it is even valid.
  GameInfoDatabase(const xam::SpaInfo* data);
  ~GameInfoDatabase() = default;

  bool IsValid() const { return is_valid_; }

  void Update(const xam::SpaInfo* new_spa);
  // This is mostly extracted from XDBF.
  std::string GetTitleName(
      const XLanguage language = XLanguage::kInvalid) const;

  XLanguage GetDefaultLanguage() const;
  std::string GetLocalizedString(
      const uint32_t id, XLanguage language = XLanguage::kInvalid) const;

  std::vector<uint8_t> GetIcon() const;

  Context GetContext(const uint32_t id) const;
  Property GetProperty(const uint32_t id) const;
  Achievement GetAchievement(const uint32_t id) const;
  PropertyBag GetPropertyBag(const xam::PropertyBag& property_bag) const;
  Field GetField(const xam::ViewFieldEntry& field_entry) const;
  StatsView GetStatsView(const uint32_t id) const;
  Presence GetPresence() const;
  PresenceMode GetPresenceMode(const uint32_t context_value) const;
  std::vector<PresenceMode> GetPresenceModes(
      const std::vector<xam::PropertyBag> property_bags) const;

  // TODO: Implement it in the future.
  std::vector<uint32_t> GetMatchmakingAttributes(const uint32_t id) const;

  // This is extracted from XLast.
  Query GetQueryData(const uint32_t id) const;
  std::vector<XLanguage> GetSupportedLanguages() const;
  ProductInformation GetProductInformation() const;

  // Aggregators for specific usecases
  std::vector<Context> GetContexts() const;
  std::vector<Property> GetProperties() const;
  std::vector<Achievement> GetAchievements() const;
  std::vector<PresenceMode> GetPresenceModes() const;
  std::vector<StatsView> GetStatsViews() const;

 private:
  void Init(const xam::SpaInfo* data);

  bool is_valid_ = false;
  std::unique_ptr<xam::SpaInfo> spa_gamedata_;
  std::unique_ptr<XLast> xlast_gamedata_;
};

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_GAME_INFO_DATABASE_H_
