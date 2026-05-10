/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/profile_editor_dialog_wx.h"

#include <array>
#include <cstring>
#include <fstream>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/app/emulator_window.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace app {

using namespace kernel::xam;

namespace {

constexpr const char* kLanguageNames[] = {nullptr,
                                          wxTRANSLATE("English"),
                                          wxTRANSLATE("Japanese"),
                                          wxTRANSLATE("German"),
                                          wxTRANSLATE("French"),
                                          wxTRANSLATE("Spanish"),
                                          wxTRANSLATE("Italian"),
                                          wxTRANSLATE("Korean"),
                                          wxTRANSLATE("Traditional Chinese"),
                                          wxTRANSLATE("Portuguese"),
                                          wxTRANSLATE("Simplified Chinese"),
                                          wxTRANSLATE("Polish"),
                                          wxTRANSLATE("Russian")};

constexpr const char* kCountryNames[] = {nullptr,
                                         wxTRANSLATE("United Arab Emirates"),
                                         wxTRANSLATE("Albania"),
                                         wxTRANSLATE("Armenia"),
                                         wxTRANSLATE("Argentina"),
                                         wxTRANSLATE("Austria"),
                                         wxTRANSLATE("Australia"),
                                         wxTRANSLATE("Azerbaijan"),
                                         wxTRANSLATE("Belgium"),
                                         wxTRANSLATE("Bulgaria"),
                                         wxTRANSLATE("Bahrain"),
                                         wxTRANSLATE("Brunei Darussalam"),
                                         wxTRANSLATE("Bolivia"),
                                         wxTRANSLATE("Brazil"),
                                         wxTRANSLATE("Belarus"),
                                         wxTRANSLATE("Belize"),
                                         wxTRANSLATE("Canada"),
                                         nullptr,
                                         wxTRANSLATE("Switzerland"),
                                         wxTRANSLATE("Chile"),
                                         wxTRANSLATE("China"),
                                         wxTRANSLATE("Colombia"),
                                         wxTRANSLATE("Costa Rica"),
                                         wxTRANSLATE("Czech Republic"),
                                         wxTRANSLATE("Germany"),
                                         wxTRANSLATE("Denmark"),
                                         wxTRANSLATE("Dominican Republic"),
                                         wxTRANSLATE("Algeria"),
                                         wxTRANSLATE("Ecuador"),
                                         wxTRANSLATE("Estonia"),
                                         wxTRANSLATE("Egypt"),
                                         wxTRANSLATE("Spain"),
                                         wxTRANSLATE("Finland"),
                                         wxTRANSLATE("Faroe Islands"),
                                         wxTRANSLATE("France"),
                                         wxTRANSLATE("Great Britain"),
                                         wxTRANSLATE("Georgia"),
                                         wxTRANSLATE("Greece"),
                                         wxTRANSLATE("Guatemala"),
                                         wxTRANSLATE("Hong Kong"),
                                         wxTRANSLATE("Honduras"),
                                         wxTRANSLATE("Croatia"),
                                         wxTRANSLATE("Hungary"),
                                         wxTRANSLATE("Indonesia"),
                                         wxTRANSLATE("Ireland"),
                                         wxTRANSLATE("Israel"),
                                         wxTRANSLATE("India"),
                                         wxTRANSLATE("Iraq"),
                                         wxTRANSLATE("Iran"),
                                         wxTRANSLATE("Iceland"),
                                         wxTRANSLATE("Italy"),
                                         wxTRANSLATE("Jamaica"),
                                         wxTRANSLATE("Jordan"),
                                         wxTRANSLATE("Japan"),
                                         wxTRANSLATE("Kenya"),
                                         wxTRANSLATE("Kyrgyzstan"),
                                         wxTRANSLATE("Korea"),
                                         wxTRANSLATE("Kuwait"),
                                         wxTRANSLATE("Kazakhstan"),
                                         wxTRANSLATE("Lebanon"),
                                         wxTRANSLATE("Liechtenstein"),
                                         wxTRANSLATE("Lithuania"),
                                         wxTRANSLATE("Luxembourg"),
                                         wxTRANSLATE("Latvia"),
                                         wxTRANSLATE("Libya"),
                                         wxTRANSLATE("Morocco"),
                                         wxTRANSLATE("Monaco"),
                                         wxTRANSLATE("Macedonia"),
                                         wxTRANSLATE("Mongolia"),
                                         wxTRANSLATE("Macau"),
                                         wxTRANSLATE("Maldives"),
                                         wxTRANSLATE("Mexico"),
                                         wxTRANSLATE("Malaysia"),
                                         wxTRANSLATE("Nicaragua"),
                                         wxTRANSLATE("Netherlands"),
                                         wxTRANSLATE("Norway"),
                                         wxTRANSLATE("New Zealand"),
                                         wxTRANSLATE("Oman"),
                                         wxTRANSLATE("Panama"),
                                         wxTRANSLATE("Peru"),
                                         wxTRANSLATE("Philippines"),
                                         wxTRANSLATE("Pakistan"),
                                         wxTRANSLATE("Poland"),
                                         wxTRANSLATE("Puerto Rico"),
                                         wxTRANSLATE("Portugal"),
                                         wxTRANSLATE("Paraguay"),
                                         wxTRANSLATE("Qatar"),
                                         wxTRANSLATE("Romania"),
                                         wxTRANSLATE("Russian Federation"),
                                         wxTRANSLATE("Saudi Arabia"),
                                         wxTRANSLATE("Sweden"),
                                         wxTRANSLATE("Singapore"),
                                         wxTRANSLATE("Slovenia"),
                                         wxTRANSLATE("Slovak Republic"),
                                         nullptr,
                                         wxTRANSLATE("El Salvador"),
                                         wxTRANSLATE("Syria"),
                                         wxTRANSLATE("Thailand"),
                                         wxTRANSLATE("Tunisia"),
                                         wxTRANSLATE("Turkey"),
                                         wxTRANSLATE("Trinidad And Tobago"),
                                         wxTRANSLATE("Taiwan"),
                                         wxTRANSLATE("Ukraine"),
                                         wxTRANSLATE("United States"),
                                         wxTRANSLATE("Uruguay"),
                                         wxTRANSLATE("Uzbekistan"),
                                         wxTRANSLATE("Venezuela"),
                                         wxTRANSLATE("Viet Nam"),
                                         wxTRANSLATE("Yemen"),
                                         wxTRANSLATE("South Africa"),
                                         wxTRANSLATE("Zimbabwe")};

constexpr const char* kSubscriptionTierNames[] = {
    wxTRANSLATE("None"),   nullptr, nullptr,
    wxTRANSLATE("Silver"), nullptr, nullptr,
    wxTRANSLATE("Gold"),   nullptr, nullptr,
    wxTRANSLATE("Family")};

constexpr const char* kGamerZoneNames[] = {
    wxTRANSLATE("None"), wxTRANSLATE("Recreation"), wxTRANSLATE("Pro"),
    wxTRANSLATE("Family"), wxTRANSLATE("Underground")};

// Indices of selectable entries in a sparse name table. Index 0 is always
// included (rendered as a blank entry when names[0] is null) so the user can
// unset the value; other null entries are skipped.
std::vector<int> SparseValues(const char* const* names, size_t count) {
  std::vector<int> values;
  for (size_t i = 0; i < count; ++i) {
    if (i == 0 || names[i]) {
      values.push_back(static_cast<int>(i));
    }
  }
  return values;
}

void PopulateSparseChoice(wxChoice* combo, const char* const* names,
                          const std::vector<int>& values) {
  for (int v : values) {
    combo->Append(names[v] ? wxGetTranslation(names[v]) : wxString());
  }
}

const std::vector<int>& LanguageValues() {
  static const auto v = SparseValues(kLanguageNames, std::size(kLanguageNames));
  return v;
}
const std::vector<int>& CountryValues() {
  static const auto v = SparseValues(kCountryNames, std::size(kCountryNames));
  return v;
}
const std::vector<int>& SubscriptionTierValues() {
  static const auto v =
      SparseValues(kSubscriptionTierNames, std::size(kSubscriptionTierNames));
  return v;
}
const std::vector<int>& GamerZoneValues() {
  static const auto v =
      SparseValues(kGamerZoneNames, std::size(kGamerZoneNames));
  return v;
}

void SelectByValue(wxChoice* combo, const std::vector<int>& values, int v) {
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i] == v) {
      combo->SetSelection(static_cast<int>(i));
      return;
    }
  }
}

int CurrentValue(wxChoice* combo, const std::vector<int>& values,
                 int fallback) {
  int sel = combo->GetSelection();
  if (sel < 0 || sel >= static_cast<int>(values.size())) {
    return fallback;
  }
  return values[sel];
}

bool IsPng(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    return false;
  }
  unsigned char sig[8];
  f.read(reinterpret_cast<char*>(sig), 8);
  if (f.gcount() != 8) {
    return false;
  }
  static const unsigned char kPngSig[8] = {0x89, 0x50, 0x4E, 0x47,
                                           0x0D, 0x0A, 0x1A, 0x0A};
  return std::memcmp(sig, kPngSig, 8) == 0;
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    return {};
  }
  f.seekg(0, std::ios::end);
  std::vector<uint8_t> data(f.tellg());
  f.seekg(0, std::ios::beg);
  f.read(reinterpret_cast<char*>(data.data()), data.size());
  return data;
}

}  // namespace

ProfileEditorDialog::ProfileEditorDialog(wxWindow* parent,
                                         EmulatorWindow* emulator_window,
                                         uint64_t xuid)
    : wxDialog(parent, wxID_ANY, _("Gamercard Editor"), wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      emulator_window_(emulator_window),
      xuid_(xuid) {
  LoadProfileData();
  SetTitle(wxString::Format(_("Gamercard Editor — %s"),
                            wxString::FromUTF8(current_data_.gamertag)));
  Build();
}

void ProfileEditorDialog::LoadProfileData() {
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return;
  }
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    return;
  }
  auto* xam_state = kernel_state->xam_state();
  if (!xam_state) {
    return;
  }
  auto* pm = xam_state->profile_manager();
  if (!pm) {
    return;
  }

  const auto* account_data = pm->GetAccount(xuid_);
  if (!account_data) {
    return;
  }

  original_data_.gamertag = account_data->GetGamertagString();
  original_data_.country = account_data->GetCountry();
  original_data_.language = account_data->GetLanguage();
  original_data_.is_live_enabled = account_data->IsLiveEnabled();
  original_data_.online_xuid =
      string_util::to_hex_string(account_data->GetOnlineXUID());
  original_data_.online_domain =
      std::string(account_data->GetOnlineDomain().data(),
                  account_data->GetOnlineDomain().size());
  original_data_.account_subscription_tier =
      account_data->GetSubscriptionTier();

  auto* profile = xam_state->GetUserProfile(xuid_);
  if (profile) {
    static const std::array<UserSettingId, 4> kSettingsToLoad = {
        UserSettingId::XPROFILE_GAMERCARD_USER_NAME,
        UserSettingId::XPROFILE_GAMERCARD_USER_BIO,
        UserSettingId::XPROFILE_GAMERCARD_MOTTO,
        UserSettingId::XPROFILE_GAMERCARD_ZONE,
    };
    // Collect first, then call get_host_data() — calling it inline between
    // GetSetting() calls trips up some GPD internal state.
    std::vector<std::pair<UserSettingId, UserSetting>> loaded;
    for (auto id : kSettingsToLoad) {
      auto setting = xam_state->user_tracker()->GetSetting(
          profile, kernel::kDashboardID, static_cast<uint32_t>(id));
      if (setting) {
        loaded.emplace_back(id, *setting);
      }
    }
    for (auto& [id, setting] : loaded) {
      original_data_.gpd_settings[id] = setting.get_host_data();
    }
    auto load_string = [&](UserSettingId id, std::string& target) {
      auto it = original_data_.gpd_settings.find(id);
      if (it == original_data_.gpd_settings.end()) {
        return;
      }
      if (auto* s = std::get_if<std::u16string>(&it->second)) {
        target = xe::to_utf8(*s);
      }
    };
    load_string(UserSettingId::XPROFILE_GAMERCARD_USER_NAME,
                original_data_.gamer_name);
    load_string(UserSettingId::XPROFILE_GAMERCARD_MOTTO,
                original_data_.gamer_motto);
    load_string(UserSettingId::XPROFILE_GAMERCARD_USER_BIO,
                original_data_.gamer_bio);

    for (auto tile :
         {XTileType::kPersonalGamerTile, XTileType::kGamerTile,
          XTileType::kPersonalGamerTileSmall, XTileType::kGamerTileSmall}) {
      auto data = profile->GetProfileIcon(tile);
      if (!data.empty()) {
        original_data_.profile_icon.assign(data.begin(), data.end());
        break;
      }
    }
  }

  current_data_ = original_data_;
}

void ProfileEditorDialog::Build() {
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  auto* xam_state = kernel_state ? kernel_state->xam_state() : nullptr;
  auto* profile = xam_state ? xam_state->GetUserProfile(xuid_) : nullptr;
  bool signed_in = profile != nullptr;

  auto* root = new wxBoxSizer(wxVERTICAL);

  // Profile Settings group.
  auto* profile_box = new wxStaticBox(this, wxID_ANY, _("Profile Settings"));
  auto* profile_sizer = new wxStaticBoxSizer(profile_box, wxVERTICAL);

  auto* icon_row = new wxBoxSizer(wxHORIZONTAL);
  icon_bitmap_ = new wxStaticBitmap(profile_box, wxID_ANY, wxBitmapBundle(),
                                    wxDefaultPosition, wxSize(64, 64));
  icon_row->Add(icon_bitmap_, wxSizerFlags().Border(wxALL, 4));
  // Pad to match the label-column width so buttons line up with the input
  // column on the field rows below.
  icon_row->AddSpacer(110 - 64 - 4);
  change_icon_button_ =
      new wxButton(profile_box, wxID_ANY, _("Change Icon..."));
  change_icon_button_->Enable(signed_in && kernel_state &&
                              !kernel_state->title_id());
  if (kernel_state && kernel_state->title_id()) {
    change_icon_button_->SetToolTip(
        _("Icon change is disabled while a title is running."));
  } else if (!signed_in) {
    change_icon_button_->SetToolTip(
        _("Profile must be signed in to change the icon."));
  } else {
    change_icon_button_->SetToolTip(
        _("PNG, 64x64 or 32x32. Refresh after re-login."));
  }
  change_icon_button_->Bind(wxEVT_BUTTON,
                            [this](wxCommandEvent&) { OnChangeIcon(); });
  icon_row->Add(change_icon_button_,
                wxSizerFlags().CenterVertical().Border(wxALL, 4));
  auto* clear_icon_button =
      new wxButton(profile_box, wxID_ANY, _("Clear Icon"));
  clear_icon_button->Enable(signed_in);
  clear_icon_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    current_data_.profile_icon.clear();
    RefreshIconBitmap();
  });
  icon_row->Add(clear_icon_button,
                wxSizerFlags().CenterVertical().Border(wxALL, 4));
  profile_sizer->Add(icon_row);
  RefreshIconBitmap();

  auto add_field = [&](const wxString& label, wxWindow* widget) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(profile_box, wxID_ANY, label, wxDefaultPosition,
                              wxSize(110, -1)),
             wxSizerFlags().CenterVertical().Border(wxALL, 4));
    row->Add(widget, wxSizerFlags(1).Expand().Border(wxALL, 4));
    profile_sizer->Add(row, wxSizerFlags().Expand());
  };

  gamertag_edit_ = new wxTextCtrl(profile_box, wxID_ANY,
                                  wxString::FromUTF8(current_data_.gamertag));
  gamertag_edit_->SetMaxLength(15);
  gamertag_edit_->Bind(wxEVT_TEXT,
                       [this](wxCommandEvent&) { UpdateGamertagValidation(); });
  add_field(_("Gamertag:"), gamertag_edit_);

  gamertag_validation_ = new wxStaticText(profile_box, wxID_ANY, "");
  gamertag_validation_->SetForegroundColour(*wxRED);
  profile_sizer->Add(gamertag_validation_,
                     wxSizerFlags().Border(wxLEFT | wxBOTTOM, 4));

  gamer_name_edit_ = new wxTextCtrl(
      profile_box, wxID_ANY, wxString::FromUTF8(current_data_.gamer_name));
  gamer_name_edit_->SetMaxLength(130);
  gamer_name_edit_->Enable(signed_in);
  add_field(_("Gamer Name:"), gamer_name_edit_);

  gamer_motto_edit_ = new wxTextCtrl(
      profile_box, wxID_ANY, wxString::FromUTF8(current_data_.gamer_motto));
  gamer_motto_edit_->SetMaxLength(22);
  gamer_motto_edit_->Enable(signed_in);
  add_field(_("Gamer Motto:"), gamer_motto_edit_);

  gamer_bio_edit_ = new wxTextCtrl(
      profile_box, wxID_ANY, wxString::FromUTF8(current_data_.gamer_bio),
      wxDefaultPosition, wxSize(-1, 100), wxTE_MULTILINE);
  gamer_bio_edit_->SetMaxLength(500);
  gamer_bio_edit_->Enable(signed_in);
  add_field(_("Gamer Bio:"), gamer_bio_edit_);

  language_combo_ = new wxChoice(profile_box, wxID_ANY);
  PopulateSparseChoice(language_combo_, kLanguageNames, LanguageValues());
  SelectByValue(language_combo_, LanguageValues(),
                static_cast<int>(current_data_.language));
  add_field(_("Language:"), language_combo_);

  country_combo_ = new wxChoice(profile_box, wxID_ANY);
  PopulateSparseChoice(country_combo_, kCountryNames, CountryValues());
  SelectByValue(country_combo_, CountryValues(),
                static_cast<int>(current_data_.country));
  add_field(_("Country:"), country_combo_);

  root->Add(profile_sizer, wxSizerFlags().Expand().Border(wxALL, 8));

  // Online Profile Settings group.
  auto* online_box =
      new wxStaticBox(this, wxID_ANY, _("Online Profile Settings"));
  auto* online_sizer = new wxStaticBoxSizer(online_box, wxVERTICAL);

  live_enabled_check_ = new wxCheckBox(online_box, wxID_ANY, _("Live Enabled"));
  live_enabled_check_->SetValue(current_data_.is_live_enabled);
  online_sizer->Add(live_enabled_check_, wxSizerFlags().Border(wxALL, 4));

  auto add_online_field = [&](const wxString& label, wxWindow* widget) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(online_box, wxID_ANY, label, wxDefaultPosition,
                              wxSize(110, -1)),
             wxSizerFlags().CenterVertical().Border(wxALL, 4));
    row->Add(widget, wxSizerFlags(1).Expand().Border(wxALL, 4));
    online_sizer->Add(row, wxSizerFlags().Expand());
  };

  online_xuid_edit_ = new wxTextCtrl(
      online_box, wxID_ANY, wxString::FromUTF8(current_data_.online_xuid),
      wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
  add_online_field(_("Online XUID:"), online_xuid_edit_);

  online_domain_edit_ = new wxTextCtrl(
      online_box, wxID_ANY, wxString::FromUTF8(current_data_.online_domain),
      wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
  add_online_field(_("Online Domain:"), online_domain_edit_);

  gamer_zone_combo_ = new wxChoice(online_box, wxID_ANY);
  PopulateSparseChoice(gamer_zone_combo_, kGamerZoneNames, GamerZoneValues());
  if (auto it = current_data_.gpd_settings.find(
          UserSettingId::XPROFILE_GAMERCARD_ZONE);
      it != current_data_.gpd_settings.end()) {
    if (auto* v = std::get_if<int32_t>(&it->second)) {
      SelectByValue(gamer_zone_combo_, GamerZoneValues(), *v);
    }
  }
  gamer_zone_combo_->Enable(current_data_.is_live_enabled && signed_in);
  add_online_field(_("Gamer Zone:"), gamer_zone_combo_);

  subscription_tier_combo_ = new wxChoice(online_box, wxID_ANY);
  PopulateSparseChoice(subscription_tier_combo_, kSubscriptionTierNames,
                       SubscriptionTierValues());
  SelectByValue(subscription_tier_combo_, SubscriptionTierValues(),
                static_cast<int>(current_data_.account_subscription_tier));
  subscription_tier_combo_->Enable(current_data_.is_live_enabled);
  add_online_field(_("Subscription Tier:"), subscription_tier_combo_);

  live_enabled_check_->Bind(wxEVT_CHECKBOX, [this, signed_in](wxCommandEvent&) {
    bool checked = live_enabled_check_->GetValue();
    gamer_zone_combo_->Enable(checked && signed_in);
    subscription_tier_combo_->Enable(checked);
  });

  root->Add(online_sizer,
            wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

  // Buttons.
  auto* button_row = new wxBoxSizer(wxHORIZONTAL);
  button_row->AddStretchSpacer(1);
  save_button_ = new wxButton(this, wxID_OK, _("Save"));
  save_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnSave(); });
  auto* cancel = new wxButton(this, wxID_CANCEL, _("Cancel"));
  button_row->Add(save_button_, wxSizerFlags().Border(wxRIGHT, 4));
  button_row->Add(cancel);
  root->Add(button_row,
            wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

  SetSizerAndFit(root);
  SetMinSize(GetSize());
  UpdateGamertagValidation();
}

void ProfileEditorDialog::RefreshIconBitmap() {
  if (!icon_bitmap_) {
    return;
  }
  if (current_data_.profile_icon.empty()) {
    icon_bitmap_->SetBitmap(wxBitmapBundle());
    return;
  }
  wxMemoryInputStream stream(current_data_.profile_icon.data(),
                             current_data_.profile_icon.size());
  wxImage img;
  if (!img.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
    icon_bitmap_->SetBitmap(wxBitmapBundle());
    return;
  }
  if (img.GetWidth() != 64 || img.GetHeight() != 64) {
    img.Rescale(64, 64, wxIMAGE_QUALITY_HIGH);
  }
  icon_bitmap_->SetBitmap(wxBitmapBundle::FromBitmap(wxBitmap(img)));
}

bool ProfileEditorDialog::IsGamertagValid() const {
  return ProfileManager::IsGamertagValid(
      gamertag_edit_->GetValue().utf8_string());
}

void ProfileEditorDialog::UpdateGamertagValidation() {
  if (!gamertag_edit_ || !gamertag_validation_ || !save_button_) {
    return;
  }
  bool valid = IsGamertagValid();
  bool empty = gamertag_edit_->IsEmpty();
  gamertag_validation_->SetLabel(valid || empty ? wxString()
                                                : _("Invalid gamertag"));
  save_button_->Enable(valid);
}

void ProfileEditorDialog::OnChangeIcon() {
  wxFileDialog dlg(this, _("Select PNG image"), "", "",
                   _("PNG Images (*.png)|*.png|All Files (*.*)|*.*"),
                   wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() != wxID_OK) {
    return;
  }
  std::filesystem::path path(dlg.GetPath().wc_str());
  if (!IsPng(path)) {
    wxMessageBox(_("Selected file is not a valid PNG image."),
                 _("Invalid file"), wxOK | wxICON_WARNING, this);
    return;
  }
  auto bytes = ReadFileBytes(path);
  // Quick dimension check: decode to validate 64x64 or 32x32.
  wxMemoryInputStream stream(bytes.data(), bytes.size());
  wxImage img;
  if (!img.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
    wxMessageBox(_("Could not decode the selected image."), _("Invalid file"),
                 wxOK | wxICON_WARNING, this);
    return;
  }
  if ((img.GetWidth() != 64 || img.GetHeight() != 64) &&
      (img.GetWidth() != 32 || img.GetHeight() != 32)) {
    wxMessageBox(wxString::Format(_("Profile icon must be 64x64 or 32x32. "
                                    "Selected image is %dx%d."),
                                  img.GetWidth(), img.GetHeight()),
                 _("Invalid resolution"), wxOK | wxICON_WARNING, this);
    return;
  }
  current_data_.profile_icon = std::move(bytes);
  RefreshIconBitmap();
}

void ProfileEditorDialog::OnSave() {
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return;
  }
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    return;
  }
  auto* xam_state = kernel_state->xam_state();
  if (!xam_state) {
    return;
  }
  auto* pm = xam_state->profile_manager();
  if (!pm) {
    return;
  }

  auto* profile = xam_state->GetUserProfile(xuid_);
  bool signed_in = profile != nullptr;

  // Account.
  const auto* account_ptr = pm->GetAccount(xuid_);
  if (!account_ptr) {
    XELOGE("ProfileEditorDialog: no account for {:016X}", xuid_);
    EndModal(wxID_CANCEL);
    return;
  }
  const auto account_original = *account_ptr;
  auto account = account_original;
  std::u16string gamertag =
      xe::to_utf16(gamertag_edit_->GetValue().utf8_string());
  string_util::copy_and_swap_truncating(account.gamertag, gamertag,
                                        sizeof(account.gamertag));
  account.SetLanguage(static_cast<XLanguage>(
      CurrentValue(language_combo_, LanguageValues(),
                   static_cast<int>(account.GetLanguage()))));
  account.SetCountry(static_cast<XOnlineCountry>(
      CurrentValue(country_combo_, CountryValues(),
                   static_cast<int>(account.GetCountry()))));
  account.ToggleLiveFlag(live_enabled_check_->GetValue());
  account.SetSubscriptionTier(
      static_cast<X_XAMACCOUNTINFO::AccountSubscriptionTier>(
          CurrentValue(subscription_tier_combo_, SubscriptionTierValues(),
                       static_cast<int>(account.GetSubscriptionTier()))));

  if (std::memcmp(&account, &account_original, sizeof(X_XAMACCOUNTINFO)) != 0) {
    if (!signed_in) {
      pm->MountProfile(xuid_);
    }
    pm->UpdateAccount(xuid_, &account);
    if (!signed_in) {
      pm->DismountProfile(xuid_);
    }
  }

  if (current_data_.profile_icon != original_data_.profile_icon) {
    // Drop both on-disk tiles and the cached image map first so a smaller
    // replacement icon doesn't get masked by a stale larger tile (the
    // toolbar prefers the 64-px file).
    auto* fs = kernel_state->file_system();
    fs->DeletePath(fmt::format("User_{:016X}:\\tile_64.png", xuid_));
    fs->DeletePath(fmt::format("User_{:016X}:\\tile_32.png", xuid_));
    if (profile) {
      profile->ClearProfileIcon(XTileType::kGamerTile);
      profile->ClearProfileIcon(XTileType::kGamerTileSmall);
      profile->ClearProfileIcon(XTileType::kPersonalGamerTile);
      profile->ClearProfileIcon(XTileType::kPersonalGamerTileSmall);
    }
    if (!current_data_.profile_icon.empty()) {
      xam_state->user_tracker()->UpdateUserIcon(
          xuid_, {current_data_.profile_icon.data(),
                  current_data_.profile_icon.size()});
    }
  }

  if (signed_in) {
    auto save_string_setting = [&](UserSettingId id, const wxString& text,
                                   size_t max_chars) {
      std::u16string value = xe::to_utf16(text.utf8_string());
      if (value.length() > max_chars) {
        value.resize(max_chars);
      }
      std::u16string swapped;
      swapped.reserve(value.size());
      for (auto ch : value) {
        swapped.push_back(xe::byte_swap(ch));
      }
      auto it = current_data_.gpd_settings.find(id);
      if (it != current_data_.gpd_settings.end()) {
        if (auto* prev = std::get_if<std::u16string>(&it->second)) {
          if (*prev == swapped) {
            return;
          }
        }
      }
      UserSetting updated(id, swapped);
      xam_state->user_tracker()->UpsertSetting(xuid_, kernel::kDashboardID,
                                               &updated);
    };
    save_string_setting(UserSettingId::XPROFILE_GAMERCARD_USER_NAME,
                        gamer_name_edit_->GetValue(), 130);
    save_string_setting(UserSettingId::XPROFILE_GAMERCARD_MOTTO,
                        gamer_motto_edit_->GetValue(), 22);
    save_string_setting(UserSettingId::XPROFILE_GAMERCARD_USER_BIO,
                        gamer_bio_edit_->GetValue(), 500);

    if (gamer_zone_combo_->IsEnabled()) {
      int32_t value = CurrentValue(gamer_zone_combo_, GamerZoneValues(), 0);
      auto it = current_data_.gpd_settings.find(
          UserSettingId::XPROFILE_GAMERCARD_ZONE);
      bool unchanged = it != current_data_.gpd_settings.end() &&
                       std::get_if<int32_t>(&it->second) &&
                       *std::get_if<int32_t>(&it->second) == value;
      if (!unchanged) {
        UserSetting updated(UserSettingId::XPROFILE_GAMERCARD_ZONE, value);
        xam_state->user_tracker()->UpsertSetting(xuid_, kernel::kDashboardID,
                                                 &updated);
      }
    }

    uint32_t user_index = pm->GetUserIndexAssignedToProfile(xuid_);
    if (user_index != XUserIndexAny) {
      kernel_state->BroadcastNotification(
          kXNotificationSystemProfileSettingChanged, 0xF);
    }
  }

  EndModal(wxID_OK);
}

}  // namespace app
}  // namespace xe
