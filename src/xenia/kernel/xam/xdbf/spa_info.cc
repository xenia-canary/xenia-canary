/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xdbf/spa_info.h"

namespace xe {
namespace kernel {
namespace xam {

SpaInfo::SpaInfo(const std::span<uint8_t> buffer) : XdbfFile(buffer) {
  // On creation we only need to load basic info. This is to prevent unnecessary
  // load if we have updated SPA in TU/DLC.
  LoadTitleInformation();
}

void SpaInfo::Load() {
  LoadLanguageData();
  LoadAchievements();
  LoadProperties();
  LoadContexts();
  LoadPresenceModes();
  LoadMatchmaking();
  LoadStatsViews();
}

bool operator<(const SpaInfo& first, const SpaInfo& second) {
  return std::tie(first.title_header_.major, first.title_header_.minor,
                  first.title_header_.build, first.title_header_.revision) <
         std::tie(second.title_header_.major, second.title_header_.minor,
                  second.title_header_.build, second.title_header_.revision);
}

bool operator==(const SpaInfo& first, const SpaInfo& second) {
  return std::tie(first.title_header_.major, first.title_header_.minor,
                  first.title_header_.build, first.title_header_.revision) ==
         std::tie(second.title_header_.major, second.title_header_.minor,
                  second.title_header_.build, second.title_header_.revision);
}

bool operator<=(const SpaInfo& first, const SpaInfo& second) {
  return first < second || first == second;
}

void SpaInfo::LoadLanguageData() {
  for (uint64_t language = 1;
       language < static_cast<uint64_t>(XLanguage::kMaxLanguages); language++) {
    auto section =
        GetEntry(static_cast<uint16_t>(SpaSection::kStringTable), language);
    if (!section) {
      continue;
    }

    auto section_header =
        reinterpret_cast<const XdbfSectionHeaderEx*>(section->data.data());
    assert_true(section_header->magic == kXdbfSignatureXstr);
    assert_true(section_header->version == 1);

    const uint8_t* ptr = section->data.data() + sizeof(XdbfSectionHeaderEx);

    XdbfLanguageStrings strings;

    for (uint16_t i = 0; i < section_header->count; i++) {
      const XdbfStringTableEntry* entry =
          reinterpret_cast<const XdbfStringTableEntry*>(ptr);

      std::string string_data = std::string(
          reinterpret_cast<const char*>(ptr + sizeof(XdbfStringTableEntry)),
          entry->string_length);

      strings[entry->id] = string_data;

      ptr += entry->string_length + sizeof(XdbfStringTableEntry);
    }

    language_strings_[static_cast<XLanguage>(language)] = strings;
  }
}

void SpaInfo::LoadAchievements() {
  auto section =
      GetEntry(static_cast<uint16_t>(SpaSection::kMetadata), kXdbfIdXach);
  if (!section) {
    return;
  }

  auto section_header =
      reinterpret_cast<const XdbfSectionHeaderEx*>(section->data.data());
  assert_true(section_header->magic == kXdbfSignatureXach);
  assert_true(section_header->version == 1);

  AchievementTableEntry* ptr = reinterpret_cast<AchievementTableEntry*>(
      section->data.data() + sizeof(XdbfSectionHeaderEx));

  for (uint32_t i = 0; i < section_header->count; i++) {
    achievements_.push_back(&ptr[i]);
  }
}

void SpaInfo::LoadProperties() {
  auto property_table =
      GetEntry(static_cast<uint16_t>(SpaSection::kMetadata), kXdbfIdXprp);

  if (!property_table) {
    return;
  }

  auto xprp_head =
      reinterpret_cast<const XdbfSectionHeader*>(property_table->data.data());
  assert_true(xprp_head->magic == kXdbfSignatureXprp);
  assert_true(xprp_head->version == 1);

  const uint8_t* ptr = property_table->data.data() + sizeof(XdbfSectionHeader);
  const uint16_t properties_count =
      xe::byte_swap(*reinterpret_cast<const uint16_t*>(ptr));
  ptr += sizeof(uint16_t);

  for (uint16_t i = 0; i < properties_count; i++) {
    auto entry = reinterpret_cast<const XdbfPropertyTableEntry*>(ptr);
    ptr += sizeof(XdbfPropertyTableEntry);
    properties_.push_back(entry);
  }
}

void SpaInfo::LoadContexts() {
  auto contexts_table =
      GetEntry(static_cast<uint16_t>(SpaSection::kMetadata), kXdbfIdXctx);
  if (!contexts_table) {
    return;
  }

  auto xcxt_head =
      reinterpret_cast<const XdbfSectionHeader*>(contexts_table->data.data());
  assert_true(xcxt_head->magic == kXdbfSignatureXcxt);
  assert_true(xcxt_head->version == 1);

  const uint8_t* ptr = contexts_table->data.data() + sizeof(XdbfSectionHeader);
  const uint32_t contexts_count =
      xe::byte_swap(*reinterpret_cast<const uint32_t*>(ptr));
  ptr += sizeof(uint32_t);

  for (uint32_t i = 0; i < contexts_count; i++) {
    auto entry = reinterpret_cast<const XdbfContextTableEntry*>(ptr);
    ptr += sizeof(XdbfContextTableEntry);
    contexts_.push_back(entry);
  }
}

void SpaInfo::LoadStatsViews() {
  auto stats_metadata =
      GetEntry(static_cast<uint16_t>(SpaSection::kMetadata), kXdbfIdXvc2);
  if (!stats_metadata) {
    return;
  }

  auto xvc2_head =
      reinterpret_cast<const XdbfSectionHeader*>(stats_metadata->data.data());
  assert_true(xvc2_head->magic == kXdbfSignatureXvc2);
  assert_true(xvc2_head->version == 1);

  std::vector<SharedView> shared_views = {};

  const uint16_t shared_view_metadata_count =
      xe::load_and_swap<uint16_t>(xvc2_head + 1);

  auto shared_view_meta_table_entry_ptr =
      reinterpret_cast<const uint8_t*>(xvc2_head + 1);

  shared_view_meta_table_entry_ptr += sizeof(uint16_t);

  for (uint32_t i = 0; i < shared_view_metadata_count; i++) {
    SharedView shared_view = {};

    auto shared_view_entry_ptr =
        reinterpret_cast<const SharedViewMetaTableEntry*>(
            shared_view_meta_table_entry_ptr);

    auto view_field_ptr =
        reinterpret_cast<const ViewFieldEntry*>(shared_view_entry_ptr + 1);

    for (uint32_t i = 0; i < shared_view_entry_ptr->column_count; i++) {
      shared_view.column_entries.push_back(
          *reinterpret_cast<const ViewFieldEntry*>(view_field_ptr + i));
    }

    for (uint32_t i = 0; i < shared_view_entry_ptr->row_count; i++) {
      shared_view.row_entries.push_back(
          *reinterpret_cast<const ViewFieldEntry*>(
              view_field_ptr + shared_view_entry_ptr->column_count + i));
    }

    const uint32_t entries_count =
        shared_view_entry_ptr->column_count + shared_view_entry_ptr->row_count;

    auto xpbm_head = reinterpret_cast<const XdbfSectionHeader*>(view_field_ptr +
                                                                entries_count);

    assert_true(xpbm_head->magic == kXdbfSignatureXpbm);
    assert_true(xpbm_head->version == 1);

    const PropertyBagEntry* property_bag_header_ptr =
        reinterpret_cast<const PropertyBagEntry*>(xpbm_head + 1);

    auto contexts_ptr =
        reinterpret_cast<const xe::be<uint32_t>*>(property_bag_header_ptr + 1);

    auto properties_ptr = reinterpret_cast<const xe::be<uint32_t>*>(
        contexts_ptr + property_bag_header_ptr->contexts_count);

    for (uint32_t i = 0; i < property_bag_header_ptr->contexts_count; i++) {
      shared_view.property_bag.contexts.insert(contexts_ptr[i]);
    }

    for (uint32_t i = 0; i < property_bag_header_ptr->properties_count; i++) {
      shared_view.property_bag.properties.insert(properties_ptr[i]);
    }

    const uint32_t xpbm_size = xpbm_head->size + sizeof(uint32_t);

    shared_view_meta_table_entry_ptr =
        reinterpret_cast<const uint8_t*>(xpbm_head) + xpbm_size;

    shared_views.push_back(shared_view);
  }

  const uint16_t tables_count =
      xe::load_and_swap<uint16_t>(shared_view_meta_table_entry_ptr);

  const StatsViewTableEntry* stats_views_ptr =
      reinterpret_cast<const StatsViewTableEntry*>(
          shared_view_meta_table_entry_ptr + sizeof(uint16_t));

  const auto views = std::vector<StatsViewTableEntry>(
      stats_views_ptr, stats_views_ptr + tables_count);

  for (const auto& view : views) {
    ViewTable view_table = {};

    // What are kContextByProperty and kContextByContext views?
    assert_zero(static_cast<uint32_t>(GetViewType(view.flags)));

    if (view.shared_index < shared_views.size()) {
      view_table.shared_view = shared_views.at(view.shared_index);
    }

    view_table.view_entry = view;

    stats_views_.push_back(view_table);
  }
}

void SpaInfo::LoadPresenceModes() {
  auto presence_modes_table =
      GetEntry(static_cast<uint16_t>(SpaSection::kMetadata), kXdbfIdXrpt);
  if (!presence_modes_table) {
    return;
  }

  auto xrpt_head = reinterpret_cast<const XdbfSectionHeader*>(
      presence_modes_table->data.data());
  assert_true(xrpt_head->magic == kXdbfSignatureXrpt);
  assert_true(xrpt_head->version == 1);

  auto xpbm_head_start_ptr = reinterpret_cast<const uint8_t*>(xrpt_head + 1);

  auto xpbm_head =
      reinterpret_cast<const XdbfSectionHeader*>(xpbm_head_start_ptr);

  assert_true(xpbm_head->magic == kXdbfSignatureXpbm);
  assert_true(xpbm_head->version == 1);

  const PropertyBagEntry* property_bag_header_ptr =
      reinterpret_cast<const PropertyBagEntry*>(xpbm_head + 1);

  auto contexts_ptr =
      reinterpret_cast<const xe::be<uint32_t>*>(property_bag_header_ptr + 1);

  auto properties_ptr = reinterpret_cast<const xe::be<uint32_t>*>(
      contexts_ptr + property_bag_header_ptr->contexts_count);

  for (uint32_t i = 0; i < property_bag_header_ptr->contexts_count; i++) {
    presence_.property_bag.contexts.insert(contexts_ptr[i]);
  }

  for (uint32_t i = 0; i < property_bag_header_ptr->properties_count; i++) {
    presence_.property_bag.properties.insert(properties_ptr[i]);
  }

  xpbm_head_start_ptr += xpbm_head->size + sizeof(uint32_t);

  const uint16_t presence_modes_count =
      xe::load_and_swap<uint16_t>(xpbm_head_start_ptr);

  xpbm_head_start_ptr += sizeof(uint16_t);

  for (size_t i = 0; i < presence_modes_count; i++) {
    PropertyBag property_bag = {};

    auto xpbm_head =
        reinterpret_cast<const XdbfSectionHeader*>(xpbm_head_start_ptr);

    assert_true(xpbm_head->magic == kXdbfSignatureXpbm);
    assert_true(xpbm_head->version == 1);

    const PropertyBagEntry* property_bag_header_ptr =
        reinterpret_cast<const PropertyBagEntry*>(xpbm_head + 1);

    auto contexts_ptr =
        reinterpret_cast<const xe::be<uint32_t>*>(property_bag_header_ptr + 1);

    auto properties_ptr = reinterpret_cast<const xe::be<uint32_t>*>(
        contexts_ptr + property_bag_header_ptr->contexts_count);

    for (uint32_t i = 0; i < property_bag_header_ptr->contexts_count; i++) {
      property_bag.contexts.insert(contexts_ptr[i]);
    }

    for (uint32_t i = 0; i < property_bag_header_ptr->properties_count; i++) {
      property_bag.properties.insert(properties_ptr[i]);
    }

    xpbm_head_start_ptr += xpbm_head->size + sizeof(uint32_t);

    presence_.presence_modes.push_back(property_bag);
  }
}

void SpaInfo::LoadMatchmaking() {
  auto matchmaking_schema =
      GetEntry(static_cast<uint16_t>(SpaSection::kMetadata), kXdbfIdXmat);
  if (!matchmaking_schema) {
    return;
  }

  auto xrpt_head = reinterpret_cast<const XdbfSectionHeader*>(
      matchmaking_schema->data.data());
  assert_true(xrpt_head->magic == kXdbfSignatureXmat);
  assert_true(xrpt_head->version == 1);

  auto xpbm_head = reinterpret_cast<const XdbfSectionHeader*>(xrpt_head + 1);

  assert_true(xpbm_head->magic == kXdbfSignatureXpbm);
  assert_true(xpbm_head->version == 1);

  const PropertyBagEntry* property_bag_header_ptr =
      reinterpret_cast<const PropertyBagEntry*>(xpbm_head + 1);

  auto contexts_ptr =
      reinterpret_cast<const xe::be<uint32_t>*>(property_bag_header_ptr + 1);

  auto properties_ptr = reinterpret_cast<const xe::be<uint32_t>*>(
      contexts_ptr + property_bag_header_ptr->contexts_count);

  for (uint32_t i = 0; i < property_bag_header_ptr->contexts_count; i++) {
    matchmaking_.contexts.insert(contexts_ptr[i]);
  }

  for (uint32_t i = 0; i < property_bag_header_ptr->properties_count; i++) {
    matchmaking_.properties.insert(properties_ptr[i]);
  }
}

const uint8_t* SpaInfo::ReadXLast(uint32_t& compressed_size,
                                  uint32_t& decompressed_size) {
  auto xlast_table =
      GetEntry(static_cast<uint16_t>(SpaSection::kMetadata), kXdbfIdXsrc);
  if (!xlast_table) {
    return nullptr;
  }

  auto xlast_head =
      reinterpret_cast<const XdbfSectionHeader*>(xlast_table->data.data());
  assert_true(xlast_head->magic == kXdbfSignatureXsrc);
  assert_true(xlast_head->version == 1);

  const uint8_t* ptr = xlast_table->data.data() + sizeof(XdbfSectionHeader);

  const uint32_t filename_length =
      xe::byte_swap(*reinterpret_cast<const uint32_t*>(ptr));
  ptr += sizeof(uint32_t) + filename_length;

  decompressed_size = xe::byte_swap(*reinterpret_cast<const uint32_t*>(ptr));
  ptr += sizeof(uint32_t);

  compressed_size = xe::byte_swap(*reinterpret_cast<const uint32_t*>(ptr));
  ptr += sizeof(uint32_t);

  return ptr;
}

XLanguage SpaInfo::GetExistingLanguage(XLanguage language_to_check) const {
  // A bit of a hack. Check if title in specific language exist.
  // If it doesn't then for sure language is not supported.
  return title_name(language_to_check).empty() ? default_language()
                                               : language_to_check;
}

std::span<const uint8_t> SpaInfo::title_icon() const {
  return GetIcon(kXdbfIdTitle);
}

XLanguage SpaInfo::default_language() const {
  auto block =
      GetEntry(static_cast<uint16_t>(SpaSection::kMetadata), kXdbfIdXstc);
  if (!block) {
    return XLanguage::kEnglish;
  }

  auto xstc = reinterpret_cast<const XdbfXstc*>(block->data.data());
  return static_cast<XLanguage>(static_cast<uint32_t>(xstc->default_language));
}

bool SpaInfo::is_system_app() const {
  return title_header_.title_type == TitleType::kSystem;
}

bool SpaInfo::is_demo() const {
  return title_header_.title_type == TitleType::kDemo;
}

bool SpaInfo::include_in_profile() const {
  if (title_header_.flags &
      static_cast<uint32_t>(TitleFlags::kAlwaysIncludeInProfile)) {
    return true;
  }

  if (title_header_.flags &
      static_cast<uint32_t>(TitleFlags::kNeverIncludeInProfile)) {
    return false;
  }

  return !is_demo();
}

uint32_t SpaInfo::title_id() const { return title_header_.title_id; }

std::string SpaInfo::title_name() const {
  return GetStringTableEntry(default_language(), kXdbfIdTitle);
}

std::string SpaInfo::title_name(XLanguage language) const {
  return GetStringTableEntry(language, kXdbfIdTitle);
}

// PRIVATE
void SpaInfo::LoadTitleInformation() {
  auto section =
      GetEntry(static_cast<uint16_t>(SpaSection::kMetadata), kXdbfIdXthd);
  if (!section) {
    return;
  }

  auto section_header =
      reinterpret_cast<const XdbfSectionHeader*>(section->data.data());
  assert_true(section_header->magic == kXdbfSignatureXthd);
  assert_true(section_header->version == 1);

  TitleHeaderData* ptr = reinterpret_cast<TitleHeaderData*>(
      section->data.data() + sizeof(XdbfSectionHeader));

  title_header_ = *ptr;
}

std::string SpaInfo::GetStringTableEntry(XLanguage language,
                                         uint16_t string_id) const {
  auto language_table = language_strings_.find(language);
  if (language_table == language_strings_.cend()) {
    return "";
  }

  auto entry = language_table->second.find(string_id);
  if (entry == language_table->second.cend()) {
    return "";
  }

  return entry->second;
}

const AchievementTableEntry* SpaInfo::GetAchievement(uint32_t id) {
  return GetSpaEntry<const AchievementTableEntry*>(achievements_, id);
}

const XdbfContextTableEntry* SpaInfo::GetContext(uint32_t id) {
  return GetSpaEntry<const XdbfContextTableEntry*>(contexts_, id);
}

const XdbfPropertyTableEntry* SpaInfo::GetProperty(uint32_t id) {
  return GetSpaEntry<const XdbfPropertyTableEntry*>(properties_, id);
}

const std::optional<PropertyBag> SpaInfo::GetPresenceMode(
    uint32_t context_value) const {
  std::optional<PropertyBag> entry = std::nullopt;

  if (context_value < presence_.presence_modes.size()) {
    entry = presence_.presence_modes.at(context_value);
  }

  return entry;
}

const std::optional<ViewTable> SpaInfo::GetStatsView(uint32_t id) {
  const auto itr = std::ranges::find_if(
      stats_views_,
      [id](const ViewTable& view) { return view.view_entry.id == id; });

  return itr != stats_views_.cend() ? std::make_optional(*itr) : std::nullopt;
}

template <typename T>
T SpaInfo::GetSpaEntry(std::vector<T>& container, uint32_t id) {
  for (const auto& entry : container) {
    if (entry->id != id) {
      continue;
    }
    return entry;
  }
  return nullptr;
}

std::span<const uint8_t> SpaInfo::GetIcon(uint64_t id) const {
  auto entry = GetEntry(static_cast<uint16_t>(SpaSection::kImage), id);
  if (!entry) {
    return {};
  }
  return {entry->data.data(), entry->data.size()};
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
