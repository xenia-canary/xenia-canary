/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/game_info_database.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/xam/user_data.h"

namespace xe {
namespace kernel {
namespace util {

GameInfoDatabase::GameInfoDatabase(const xam::SpaInfo* data) {
  if (!data) {
    return;
  }

  Init(data);
}

void GameInfoDatabase::Init(const xam::SpaInfo* data) {
  spa_gamedata_ = std::make_unique<xam::SpaInfo>(*data);
  spa_gamedata_->Load();
  is_valid_ = true;

  uint32_t compressed_size, decompressed_size = 0;
  const uint8_t* xlast_ptr =
      spa_gamedata_->ReadXLast(compressed_size, decompressed_size);

  if (!xlast_ptr) {
    XELOGW(
        "GameDatabase: Title doesn't contain XLAST data! Multiplayer "
        "functionality might be limited.");
    return;
  }

  xlast_gamedata_ =
      std::make_unique<XLast>(xlast_ptr, compressed_size, decompressed_size);

  if (!xlast_gamedata_) {
    XELOGW(
        "GameDatabase: Title XLAST data is corrupted! Multiplayer "
        "functionality might be limited.");
    return;
  }
}

void GameInfoDatabase::Update(const xam::SpaInfo* new_spa) {
  if (*spa_gamedata_ <= *new_spa) {
    return;
  }

  Init(new_spa);
}

std::string GameInfoDatabase::GetTitleName(const XLanguage language) const {
  return spa_gamedata_->title_name(
      spa_gamedata_->GetExistingLanguage(language));
}

std::vector<uint8_t> GameInfoDatabase::GetIcon() const {
  std::vector<uint8_t> data;

  const auto icon = spa_gamedata_->title_icon();
  data.insert(data.begin(), icon.begin(), icon.end());
  return data;
}

XLanguage GameInfoDatabase::GetDefaultLanguage() const {
  if (!is_valid_) {
    return XLanguage::kEnglish;
  }

  return spa_gamedata_->default_language();
}

std::string GameInfoDatabase::GetLocalizedString(const uint32_t id,
                                                 XLanguage language) const {
  return spa_gamedata_->GetStringTableEntry(
      spa_gamedata_->GetExistingLanguage(language), id);
}

std::optional<GameInfoDatabase::Context> GameInfoDatabase::GetContext(
    const uint32_t id) const {
  if (!is_valid_) {
    return std::nullopt;
  }

  const auto xdbf_context = spa_gamedata_->GetContext(id);
  if (!xdbf_context) {
    return std::nullopt;
  }

  Context context = {};

  context.id = xdbf_context->id;
  context.default_value = xdbf_context->default_value;
  context.max_value = xdbf_context->max_value;
  context.is_system = xam::UserData::is_system_property(xdbf_context->id);
  context.is_presence = GetPresence().property_bag.contexts.contains(id);
  context.is_matchmaking =
      GetMatchmakingCollection().contexts.contains(xdbf_context->id);
  context.description = GetLocalizedString(xdbf_context->string_id);
  return context;
}

std::optional<GameInfoDatabase::Property> GameInfoDatabase::GetProperty(
    const uint32_t id) const {
  if (!is_valid_) {
    return std::nullopt;
  }

  const auto xdbf_property = spa_gamedata_->GetProperty(id);
  if (!xdbf_property) {
    return std::nullopt;
  }

  Property property = {};

  property.id = xdbf_property->id;
  property.data_size = xdbf_property->data_size;
  property.is_system = xam::UserData::is_system_property(xdbf_property->id);
  property.is_presence = GetPresence().property_bag.properties.contains(id);
  property.is_matchmaking =
      GetMatchmakingCollection().properties.contains(xdbf_property->id);
  property.description = GetLocalizedString(xdbf_property->string_id);
  return property;
}

std::optional<GameInfoDatabase::Achievement> GameInfoDatabase::GetAchievement(
    const uint32_t id) const {
  if (!is_valid_) {
    return std::nullopt;
  }

  Achievement achievement = {};

  const auto xdbf_achievement = spa_gamedata_->GetAchievement(id);
  if (!xdbf_achievement) {
    return achievement;
  }

  achievement.id = xdbf_achievement->id;
  achievement.image_id = xdbf_achievement->id;
  achievement.gamerscore = xdbf_achievement->gamerscore;
  achievement.flags = xdbf_achievement->flags;

  achievement.label = GetLocalizedString(xdbf_achievement->label_id);
  achievement.description =
      GetLocalizedString(xdbf_achievement->description_id);
  achievement.unachieved_description =
      GetLocalizedString(xdbf_achievement->unachieved_id);
  return achievement;
}

GameInfoDatabase::PropertyBag GameInfoDatabase::GetPropertyBag(
    const xam::PropertyBag& property_bag) const {
  PropertyBag property_bag_native = {};

  property_bag_native.contexts = {property_bag.contexts.cbegin(),
                                  property_bag.contexts.cend()};
  property_bag_native.properties = {property_bag.properties.cbegin(),
                                    property_bag.properties.cend()};

  return property_bag_native;
}

GameInfoDatabase::Field GameInfoDatabase::GetField(
    const xam::ViewFieldEntry& field_entry) const {
  Field field = {};

  field.property_id = field_entry.property_id;
  field.flags = field_entry.flags;
  field.attribute_id = field_entry.attribute_id;
  field.aggregation_type = field_entry.aggregation_type;
  field.ordinal = field_entry.ordinal;
  field.field_type = field_entry.field_type;
  field.format_type = field_entry.format_type;
  field.name = GetLocalizedString(field_entry.string_id);

  if (field.name.empty()) {
    field.name = xam::AttributeIdToName(field.attribute_id);
  }

  return field;
}

std::optional<GameInfoDatabase::StatsView> GameInfoDatabase::GetStatsView(
    const uint32_t id) const {
  if (!is_valid_) {
    return std::nullopt;
  }

  const auto xdbf_stats_view = spa_gamedata_->GetStatsView(id);

  if (!xdbf_stats_view.has_value()) {
    return std::nullopt;
  }

  StatsView stats_view = {};

  stats_view.view.id = xdbf_stats_view->view_entry.id;

  stats_view.view.arbitrated =
      xam::IsArbitrated(xdbf_stats_view->view_entry.flags);
  stats_view.view.hidden = xam::IsHidden(xdbf_stats_view->view_entry.flags);
  stats_view.view.team_view =
      xam::IsTeamView(xdbf_stats_view->view_entry.flags);
  stats_view.view.online_only =
      xam::IsOnlineOnly(xdbf_stats_view->view_entry.flags);

  stats_view.view.view_type =
      xam::GetViewType(xdbf_stats_view->view_entry.flags);
  stats_view.view.skilled = xam::IsLeaderboardIdSkill(stats_view.view.id);

  stats_view.view.shared_index = xdbf_stats_view->view_entry.shared_index;

  stats_view.view.name =
      GetLocalizedString(xdbf_stats_view->view_entry.string_id);

  for (const auto& column : xdbf_stats_view->shared_view.column_entries) {
    stats_view.shared_view.column_entries.push_back(GetField(column));
  }

  for (const auto& row : xdbf_stats_view->shared_view.row_entries) {
    stats_view.shared_view.row_entries.push_back(GetField(row));
  }

  stats_view.shared_view.properties =
      GetPropertyBag(xdbf_stats_view->shared_view.property_bag);

  return stats_view;
}

GameInfoDatabase::Presence GameInfoDatabase::GetPresence() const {
  Presence presence;

  if (!is_valid_) {
    return presence;
  }

  const auto xdbf_presence = spa_gamedata_->GetPresence();

  presence.property_bag = GetPropertyBag(xdbf_presence->property_bag);
  presence.presence_modes = GetPresenceModes();

  return presence;
}

std::optional<GameInfoDatabase::PresenceMode> GameInfoDatabase::GetPresenceMode(
    const uint32_t context_value) const {
  if (!is_valid_) {
    return std::nullopt;
  }

  PresenceMode presence_mode = {};

  const auto xdbf_presence_mode = spa_gamedata_->GetPresenceMode(context_value);

  if (!xdbf_presence_mode.has_value()) {
    return presence_mode;
  }

  presence_mode.context_value = context_value;
  presence_mode.property_bag = GetPropertyBag(xdbf_presence_mode.value());

  return presence_mode;
}

std::vector<uint32_t> GameInfoDatabase::GetMatchmakingAttributes(
    const uint32_t id) const {
  // TODO(Gliniak): Implement when we will fully understand how to read it from
  // SPA.
  std::vector<uint32_t> result;
  return result;
}

// XLAST
GameInfoDatabase::Query GameInfoDatabase::GetQueryData(
    const uint32_t id) const {
  Query query = {};

  if (!xlast_gamedata_) {
    return query;
  }

  const auto xlast_query = xlast_gamedata_->GetMatchmakingQuery(id);
  if (!xlast_query) {
    return query;
  }

  query.id = id;
  query.name = xlast_query->GetName();
  query.input_parameters = xlast_query->GetParameters();
  query.filters = xlast_query->GetFilters();
  query.expected_return = xlast_query->GetReturns();
  return query;
}

std::vector<XLanguage> GameInfoDatabase::GetSupportedLanguages() const {
  if (!xlast_gamedata_) {
    return {};
  }

  return xlast_gamedata_->GetSupportedLanguages();
}

GameInfoDatabase::ProductInformation GameInfoDatabase::GetProductInformation()
    const {
  if (!xlast_gamedata_) {
    return {};
  }

  ProductInformation info = {};

  const auto attributes = xlast_gamedata_->GetProductInformationAttributes();
  for (const auto& attribute : attributes) {
    switch (attribute.first) {
      case ProductInformationEntry::MaxOfflinePlayers:
        info.max_offline_players_count = attribute.second;
        break;
      case ProductInformationEntry::MaxSystemLinkPlayers:
        info.max_systemlink_players_count = attribute.second;
        break;
      case ProductInformationEntry::MaxLivePlayers:
        info.max_live_players_count = attribute.second;
        break;
      case ProductInformationEntry::PublisherString:
        info.publisher_name = xe::to_utf8(xlast_gamedata_->GetLocalizedString(
            attribute.second, XLanguage::kEnglish));
        break;
      case ProductInformationEntry::DeveloperString:
        info.developer_name = xe::to_utf8(xlast_gamedata_->GetLocalizedString(
            attribute.second, XLanguage::kEnglish));
        break;
      case ProductInformationEntry::MarketingString:
        info.marketing_info = xe::to_utf8(xlast_gamedata_->GetLocalizedString(
            attribute.second, XLanguage::kEnglish));
        break;
      case ProductInformationEntry::GenreTypeString:
        info.genre_description =
            xe::to_utf8(xlast_gamedata_->GetLocalizedString(
                attribute.second, XLanguage::kEnglish));
        break;
      default:
        break;
    }
  }
  return info;
}

GameInfoDatabase::PropertyBag GameInfoDatabase::GetMatchmakingCollection()
    const {
  return GetPropertyBag(*spa_gamedata_->GetMatchCollection());
}

// Aggregators
std::vector<GameInfoDatabase::Context> GameInfoDatabase::GetContexts() const {
  std::vector<Context> contexts;

  if (!is_valid_) {
    return contexts;
  }

  const auto xdbf_contexts = spa_gamedata_->GetContexts();
  for (const auto& entry : xdbf_contexts) {
    const auto context = GetContext(entry->id);

    if (context.has_value()) {
      contexts.push_back(context.value());
    }
  }

  return contexts;
}

std::vector<GameInfoDatabase::Property> GameInfoDatabase::GetProperties()
    const {
  std::vector<Property> properties;

  if (!is_valid_) {
    return properties;
  }

  const auto xdbf_properties = spa_gamedata_->GetProperties();
  for (const auto& entry : xdbf_properties) {
    const auto property = GetProperty(entry->id);

    if (property.has_value()) {
      properties.push_back(property.value());
    }
  }

  return properties;
}

std::vector<GameInfoDatabase::Achievement> GameInfoDatabase::GetAchievements()
    const {
  std::vector<Achievement> achievements;

  if (!is_valid_) {
    return achievements;
  }

  const auto xdbf_achievements = spa_gamedata_->GetAchievements();
  for (const auto& entry : xdbf_achievements) {
    auto achievement = GetAchievement(entry->id);

    if (achievement.has_value()) {
      achievements.push_back(achievement.value());
    }
  }

  return achievements;
}

std::vector<GameInfoDatabase::StatsView> GameInfoDatabase::GetStatsViews()
    const {
  std::vector<StatsView> stats_views;

  if (!is_valid_) {
    return stats_views;
  }

  const auto xdbf_stats_views = spa_gamedata_->GetStatsViews();

  for (const auto& entry : *xdbf_stats_views) {
    auto stats_view = GetStatsView(entry.view_entry.id);

    if (stats_view.has_value()) {
      stats_views.push_back(stats_view.value());
    }
  }

  return stats_views;
}

std::vector<GameInfoDatabase::PresenceMode> GameInfoDatabase::GetPresenceModes()
    const {
  std::vector<PresenceMode> presence_modes;

  if (!is_valid_) {
    return presence_modes;
  }

  const auto& xdbf_presence_modes =
      spa_gamedata_->GetPresence()->presence_modes;

  for (uint32_t context_value = 0; context_value < xdbf_presence_modes.size();
       context_value++) {
    const auto presence_mode = GetPresenceMode(context_value);

    if (presence_mode.has_value()) {
      presence_modes.push_back(presence_mode.value());
    }
  }

  return presence_modes;
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
