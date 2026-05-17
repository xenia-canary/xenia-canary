/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/app/emulator_window.h"
#include "xenia/app/profile_dialogs.h"
#include "xenia/base/png_utils.h"
#include "xenia/base/system.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/ui/file_picker.h"

#include "xenia/app/console_settings_dialog.h"

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace app {

static const std::map<uint32_t, std::string> kLanguageMap = {
    {1, "English"},    {2, "Japanese"},
    {3, "German"},     {4, "French"},
    {5, "Spanish"},    {6, "Italian"},
    {7, "Korean"},     {8, "Traditional Chinese"},
    {9, "Portuguese"}, {10, "Simplified Chinese"},
    {11, "Polish"},    {12, "Russian"}};

static const std::map<uint8_t, std::string> kCountryMap = {
    {1, "AE"},   {2, "AL"},   {3, "AM"},   {4, "AR"},   {5, "AT"},
    {6, "AU"},   {7, "AZ"},   {8, "BE"},   {9, "BG"},   {10, "BH"},
    {11, "BN"},  {12, "BO"},  {13, "BR"},  {14, "BY"},  {15, "BZ"},
    {16, "CA"},  {18, "CH"},  {19, "CL"},  {20, "CN"},  {21, "CO"},
    {22, "CR"},  {23, "CZ"},  {24, "DE"},  {25, "DK"},  {26, "DO"},
    {27, "DZ"},  {28, "EC"},  {29, "EE"},  {30, "EG"},  {31, "ES"},
    {32, "FI"},  {33, "FO"},  {34, "FR"},  {35, "GB"},  {36, "GE"},
    {37, "GR"},  {38, "GT"},  {39, "HK"},  {40, "HN"},  {41, "HR"},
    {42, "HU"},  {43, "ID"},  {44, "IE"},  {45, "IL"},  {46, "IN"},
    {47, "IQ"},  {48, "IR"},  {49, "IS"},  {50, "IT"},  {51, "JM"},
    {52, "JO"},  {53, "JP"},  {54, "KE"},  {55, "KG"},  {56, "KR"},
    {57, "KW"},  {58, "KZ"},  {59, "LB"},  {60, "LI"},  {61, "LT"},
    {62, "LU"},  {63, "LV"},  {64, "LY"},  {65, "MA"},  {66, "MC"},
    {67, "MK"},  {68, "MN"},  {69, "MO"},  {70, "MV"},  {71, "MX"},
    {72, "MY"},  {73, "NI"},  {74, "NL"},  {75, "NO"},  {76, "NZ"},
    {77, "OM"},  {78, "PA"},  {79, "PE"},  {80, "PH"},  {81, "PK"},
    {82, "PL"},  {83, "PR"},  {84, "PT"},  {85, "PY"},  {86, "QA"},
    {87, "RO"},  {88, "RU"},  {89, "SA"},  {90, "SE"},  {91, "SG"},
    {92, "SI"},  {93, "SK"},  {95, "SV"},  {96, "SY"},  {97, "TH"},
    {98, "TN"},  {99, "TR"},  {100, "TT"}, {101, "TW"}, {102, "UA"},
    {103, "US"}, {104, "UY"}, {105, "UZ"}, {106, "VE"}, {107, "VN"},
    {108, "YE"}, {109, "ZA"},
};

static const std::map<uint32_t, std::string> kAVRegion = {
    {0x00400100, "NTSC"},
    {0x00400200, "NTSC-J"},
    {0x00400400, "PAL"},
    {0x00800300, "PAL 50Hz"}};

template <typename T>
void DrawCombobox(ImGuiIO& io, std::string_view combobox_name,
                  const std::map<T, std::string>& options, xe::be<T>& value) {
  const char* preview = "Unknown";
  for (const auto& opt : options) {
    if (opt.first == value.get()) {
      preview = opt.second.data();
      break;
    }
  }

  if (ImGui::BeginCombo(combobox_name.data(), preview)) {
    for (const auto& opt : options) {
      const bool selected = (opt.first == value.get());
      if (ImGui::Selectable(opt.second.data(), selected)) {
        value = opt.first;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
}

void DrawResolutionCombobox(ImGuiIO& io,
                            const std::span<const kernel::Resolution> options,
                            xe::be<int32_t>& resolution,
                            xe::be<uint32_t>& video_flags) {
  const char* preview = "Unknown";
  for (const auto& opt : options) {
    if (opt.to_host() == resolution.get()) {
      preview = opt.name_.c_str();
      break;
    }
  }

  if (ImGui::BeginCombo("Resolution", preview)) {
    for (const auto& opt : options) {
      const bool selected = (opt.to_host() == resolution.get());
      if (ImGui::Selectable(opt.name_.c_str(), selected)) {
        resolution = opt.to_host();

        if (opt.is_widescreen()) {
          video_flags = video_flags | static_cast<uint32_t>(
                                          kernel::X_VIDEO_FLAGS::Widescreen);
        } else {
          video_flags = video_flags & ~static_cast<uint32_t>(
                                          kernel::X_VIDEO_FLAGS::Widescreen);
        }
      }

      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
}

void DrawTimezoneCombobox(ImGuiIO& io, kernel::XConfigData* xdata) {
  const kernel::TimeZone current_tz = {
      xdata->user.time_zone_bias, xdata->user.tz_std_name,
      xdata->user.tz_dlt_name,    xdata->user.tz_std_date,
      xdata->user.tz_dlt_date,    xdata->user.tz_std_bias,
      xdata->user.tz_dlt_bias};

  size_t index = 0x19;  // Index of GMT+00

  auto it = std::find(kernel::kTimezones.cbegin(), kernel::kTimezones.cend(),
                      current_tz);
  if (it != kernel::kTimezones.cend()) {
    index = std::distance(kernel::kTimezones.cbegin(), it);
  }

  if (ImGui::BeginCombo("Timezone", kernel::kTimezones[index].name.c_str())) {
    for (size_t i = 0; i < kernel::kTimezones.size(); ++i) {
      const bool is_selected =
          (kernel::kTimezones[i] == kernel::kTimezones[index]);

      if (ImGui::Selectable(kernel::kTimezones[i].name.c_str(), is_selected)) {
        index = i;

        // Update timezone values
        const auto& tz = kernel::kTimezones[index];
        xdata->user.time_zone_bias = tz.timezone_bias;
        memcpy(xdata->user.tz_std_name.data(), tz.tz_std_name.data(), 4);
        memcpy(xdata->user.tz_dlt_name.data(), tz.tz_dlt_name.data(), 4);
        memcpy(xdata->user.tz_std_date.data(), tz.tz_std_date.data(), 4);
        memcpy(xdata->user.tz_dlt_date.data(), tz.tz_dlt_date.data(), 4);
        xdata->user.tz_std_bias = tz.tz_std_bias;
        xdata->user.tz_dlt_bias = tz.tz_dlt_bias;
      }
      if (is_selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
}

template <typename T>
static bool FlagCheckbox(const char* label, xe::be<T>& field, T value) {
  bool set = (field.get() & value) != 0;
  if (ImGui::Checkbox(label, &set)) {
    if (set) {
      field = field.get() | value;
    } else {
      field = field.get() & ~value;
    }
    return true;
  }
  return false;
}

template <typename T>
static void ByteArray(const char* label, T* array, size_t count) {
  for (size_t i = 0; i < count; i++) {
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("CC_").x);
    ImGui::InputScalar(fmt::format("##{}{}", label, i).c_str(),
                       ImGuiDataType_U8, &array[i], nullptr, nullptr, "%02X");

    if (i + 1 < count) {
      ImGui::SameLine();
    }
  }
}

void GroupBox(const char* label, auto draw_fn) {
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  ImVec2 text_size = ImGui::CalcTextSize(label);
  float border_offset = text_size.y * 0.5f;
  ImGuiStyle& style = ImGui::GetStyle();

  // Push content down so it doesn't overlap the label
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + border_offset);
  ImGui::Indent(8.0f);

  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - 16.0f, 0));
  ImGui::Spacing();
  draw_fn();
  ImGui::Spacing();
  ImGui::EndGroup();

  ImGui::Unindent(8.0f);

  // NOW we know the rect
  ImVec2 rect_min = ImGui::GetItemRectMin();
  ImVec2 rect_max = ImGui::GetItemRectMax();

  // Expand rect a bit for padding
  rect_min.x -= 8.0f;
  rect_max.x += 8.0f;
  rect_max.y += style.ItemSpacing.y;

  // Move the top of the border up to account for label offset
  rect_min.y -= border_offset;

  ImU32 border_color = ImGui::GetColorU32(ImGuiCol_Border);
  float label_start = rect_min.x + 8.0f;
  float label_end = label_start + text_size.x + 4.0f;

  // Top edge with gap for label
  draw_list->AddLine(ImVec2(rect_min.x, rect_min.y),
                     ImVec2(label_start, rect_min.y), border_color);
  draw_list->AddLine(ImVec2(label_end, rect_min.y),
                     ImVec2(rect_max.x, rect_min.y), border_color);
  // Left, right, bottom
  draw_list->AddLine(rect_min, ImVec2(rect_min.x, rect_max.y), border_color);
  draw_list->AddLine(ImVec2(rect_max.x, rect_min.y),
                     ImVec2(rect_max.x, rect_max.y), border_color);
  draw_list->AddLine(ImVec2(rect_min.x, rect_max.y), rect_max, border_color);

  // Label
  draw_list->AddText(ImVec2(label_start + 2.0f, rect_min.y - border_offset),
                     ImGui::GetColorU32(ImGuiCol_Text), label);

  ImGui::SetCursorPosY(rect_max.y + style.ItemSpacing.y);
}

void ConsoleSettingsDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.90f);
  bool dialog_open = true;
  if (!ImGui::Begin("Console settings", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoMove)) {
    ImGui::End();
    return;
  }

  if (!dialog_open) {
    Close();
    ImGui::End();
    emulator_window_.ToggleConsoleSettingsDialog();
    return;
  }

  if (ImGui::BeginTabBar("##Categories")) {
    if (ImGui::BeginTabItem("User")) {
      ImGui::BeginDisabled(emulator_window_.emulator()->is_title_open());
      ImGui::Dummy(ImVec2(2.f, 2.f));

      GroupBox("Time", [&]() {
        DrawTimezoneCombobox(io, &xconfig_data_);

        FlagCheckbox("Disable Daylight-Saving Time",
                     xconfig_data_.user.retail_flags,
                     static_cast<uint32_t>(kernel::X_RETAIL_FLAGS::DSTOff));

        FlagCheckbox(
            "24H Time", xconfig_data_.user.retail_flags,
            static_cast<uint32_t>(kernel::X_RETAIL_FLAGS::TwentyFourHourClock));
      });

      GroupBox("Locale", [&]() {
        DrawCombobox(io, "Language", kLanguageMap, xconfig_data_.user.language);
        DrawCombobox(io, "Country", kCountryMap, xconfig_data_.user.country);
      });

      GroupBox("Profile", [&]() {
        DrawCombobox(io, "Default Profile", profiles_,
                     xconfig_data_.user.default_profile);

        FlagCheckbox("Parental Control",
                     xconfig_data_.user.parental_control_flags,
                     static_cast<uint8_t>(kernel::X_PC_FLAGS::PCEnabled));
      });

      ImGui::Dummy(ImVec2(2.f, 2.f));

      GroupBox("Retail Options", [&]() {
        FlagCheckbox("Dashboard Initialized", xconfig_data_.user.retail_flags,
                     static_cast<uint32_t>(
                         kernel::X_RETAIL_FLAGS::DashboardInitialized));
        FlagCheckbox(
            "IPTV Initialized", xconfig_data_.user.retail_flags,
            static_cast<uint32_t>(kernel::X_RETAIL_FLAGS::IPTVEnabled));
        FlagCheckbox(
            "DVR Initialized", xconfig_data_.user.retail_flags,
            static_cast<uint32_t>(kernel::X_RETAIL_FLAGS::IPTVDVREnabled));
        FlagCheckbox(
            "Kinect Initialized", xconfig_data_.user.retail_flags,
            static_cast<uint32_t>(kernel::X_RETAIL_FLAGS::KinectInitialized));
      });
      ImGui::EndDisabled();
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("System")) {
      ImGui::BeginDisabled(emulator_window_.emulator()->is_title_open());
      ImGui::Dummy(ImVec2(2.f, 2.f));

      GroupBox("Video Options", [&]() {
        DrawCombobox(io, "AV Region", kAVRegion,
                     xconfig_data_.secured.av_region);

        DrawResolutionCombobox(
            io, {kernel::XVGAResolution.data(), kernel::XVGAResolution.size()},
            xconfig_data_.user.av_pack_hdmi_sz, xconfig_data_.user.video_flags);

        const auto res =
            kernel::Resolution(xconfig_data_.user.av_pack_hdmi_sz.get());

        ImGui::BeginDisabled(res.is_widescreen());
        FlagCheckbox("Widescreen", xconfig_data_.user.video_flags,
                     static_cast<uint32_t>(kernel::X_VIDEO_FLAGS::Widescreen));
        ImGui::EndDisabled();
      });

      GroupBox("Audio Options", [&]() {
        FlagCheckbox("Mono", xconfig_data_.user.audio_flags,
                     static_cast<uint32_t>(kernel::X_AUDIO_FLAGS::AnalogMono));

        FlagCheckbox(
            "Dolby Pro Logic", xconfig_data_.user.audio_flags,
            static_cast<uint32_t>(kernel::X_AUDIO_FLAGS::DolbyProLogic));

        FlagCheckbox(
            "Dolby Digital", xconfig_data_.user.audio_flags,
            static_cast<uint32_t>(kernel::X_AUDIO_FLAGS::DolbyDigital));

        FlagCheckbox("Dolby Digital WMA PRO", xconfig_data_.user.audio_flags,
                     static_cast<uint32_t>(
                         kernel::X_AUDIO_FLAGS::DolbyDigitalWithWMAPRO));

        FlagCheckbox("Low Latency (unsupported)",
                     xconfig_data_.user.audio_flags,
                     static_cast<uint32_t>(kernel::X_AUDIO_FLAGS::LowLatency));

        float volume = xconfig_data_.user.music_volume.get();
        if (ImGui::SliderFloat("Audio player volume", &volume, 0.0f, 1.0f,
                               "%.2f")) {
          xconfig_data_.user.music_volume = volume;
        }
      });

      GroupBox("Network", [&]() {
        if (ImGui::BeginTable("##NetworkTable", 2)) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();

          ImGui::Text("MAC Address: ");
          ImGui::TableNextColumn();

          ByteArray("mac", xconfig_data_.secured.mac_address.data(),
                    xconfig_data_.secured.mac_address.size());

          ImGui::TableNextRow();
          ImGui::TableNextColumn();

          ImGui::Text("Network ID: ");
          ImGui::TableNextColumn();

          ByteArray("netid", xconfig_data_.secured.online_network_id.data(),
                    xconfig_data_.secured.online_network_id.size());

          ImGui::EndTable();
        }
      });
      ImGui::EndDisabled();
      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::Dummy(ImVec2(2.f, 2.f));
  // Bottom
  ImGui::Separator();

  ImGui::BeginDisabled(emulator_window_.emulator()->is_title_open());
  if (ImGui::Button("Save")) {
    SaveConfig();
    save_confirmation_disappearance_ = ImGui::GetTime() + 3.0;
  }

  if (save_confirmation_disappearance_ > ImGui::GetTime()) {
    ImGui::SameLine();
    ImGui::Text("Settings Saved!");
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                       ImGui::GetContentRegionAvail().x - 55.f);

  if (ImGui::Button("Reset", ImVec2(55.f, 0.0f))) {
    xconfig_->SetDefaults();
    xconfig_data_ = *xconfig_->GetXConfig();
    save_confirmation_disappearance_ = ImGui::GetTime() + 3.0;
  }
  ImGui::EndDisabled();
  ImGui::End();
}

void ConsoleSettingsDialog::SaveConfig() {
  xconfig_->WriteXConfig(&xconfig_data_);
}

}  // namespace app
}  // namespace xe
