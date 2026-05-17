/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xconfig.h"

#include "xenia/base/logging.h"

#include "xenia/base/filesystem.h"

#include "xenia/base/byte_order.h"
#include "xenia/xbox.h"

#include <ranges>

namespace xe {
namespace kernel {

XConfig::XConfig(const std::filesystem::path& xconfig_path)
    : file_path_(xconfig_path) {
  if (!std::filesystem::exists(xconfig_path)) {
    SetDefaults();

    if (xe::filesystem::CreateEmptyFile(xconfig_path)) {
      FlushToFile();
    }
  }

  FILE* file = xe::filesystem::OpenFile(xconfig_path, "rb");

  if (!file) {
    SetDefaults();
    return;
  }

  fread(&xconfig_data_, sizeof(XConfigData), 1, file);
  fclose(file);
}

void XConfig::ReadSetting(const X_CONFIG_CATEGORY category,
                          const uint16_t setting_id, void* buffer) {
  const auto setting = FindField(category, setting_id);

  if (!setting) {
    return;
  }

  std::lock_guard<xe_mutex> lock(lock_);
  std::memcpy(buffer, CategoryBase(category) + setting->block_offset,
              setting->size);
}

void XConfig::WriteSetting(const X_CONFIG_CATEGORY category,
                           const uint16_t setting_id, const void* buffer) {
  const auto setting = FindField(category, setting_id);

  if (!setting) {
    return;
  }

  std::lock_guard<xe_mutex> lock(lock_);
  std::memcpy(CategoryBase(category) + setting->block_offset, buffer,
              setting->size);

  FlushToFile();
}

uint16_t XConfig::GetSettingSize(const X_CONFIG_CATEGORY category,
                                 const uint16_t setting_id) {
  const auto setting = FindField(category, setting_id);

  if (!setting) {
    return 0;
  }

  return setting->size;
}

void XConfig::WriteXConfig(const XConfigData* data) {
  xconfig_data_ = *data;
  FlushToFile();
}

void XConfig::SetDefaults() {
  xconfig_data_ = {};

  xconfig_data_.secured.av_region = X_AV_REGION::NTSCM;
  xconfig_data_.user.language = static_cast<uint32_t>(XLanguage::kEnglish);
  xconfig_data_.user.country =
      static_cast<uint8_t>(XOnlineCountry::kUnitedStates);
  xconfig_data_.user.audio_flags = DolbyDigital | DolbyProLogic;
  xconfig_data_.user.av_pack_hdmi_sz = XHDTVResolution.at(1).to_host();
  xconfig_data_.user.av_pack_component_sz = XHDTVResolution.at(1).to_host();
  xconfig_data_.user.av_pack_vga_sz = XVGAResolution.at(3).to_host();
  xconfig_data_.user.retail_flags = DashboardInitialized;
  xconfig_data_.user.video_flags = RatioNormal;
  xconfig_data_.user.parental_control_flags =
      XBLAllowed | XBLMembershipCreationAllowed;
  xconfig_data_.user.parental_control_game = NoGameRestrictions;
  xconfig_data_.user.music_volume = 0.7f;

  const auto& tz = kTimezones[0x19];

  xconfig_data_.user.time_zone_bias = tz.timezone_bias;
  memcpy(xconfig_data_.user.tz_std_name.data(), tz.tz_std_name.data(), 4);
  memcpy(xconfig_data_.user.tz_dlt_name.data(), tz.tz_dlt_name.data(), 4);
  memcpy(xconfig_data_.user.tz_std_date.data(), tz.tz_std_date.data(), 4);
  memcpy(xconfig_data_.user.tz_dlt_date.data(), tz.tz_dlt_date.data(), 4);
  xconfig_data_.user.tz_std_bias = tz.tz_std_bias;
  xconfig_data_.user.tz_dlt_bias = tz.tz_dlt_bias;
}

void XConfig::FlushToFile() {
  if (!std::filesystem::exists(file_path_)) {
    return;
  }

  FILE* file = xe::filesystem::OpenFile(file_path_, "wb");
  if (!file) {
    return;
  }

  fwrite(&xconfig_data_, 1, sizeof(XConfigData), file);
  fclose(file);
}

const XConfig::FieldDescriptor* XConfig::FindField(X_CONFIG_CATEGORY category,
                                                   uint16_t setting) {
  auto it = std::ranges::find_if(kFields, [&](const FieldDescriptor& f) {
    return f.category == category && f.setting == setting;
  });

  return it == std::ranges::end(kFields) ? nullptr : &*it;
}

uint8_t* XConfig::CategoryBase(X_CONFIG_CATEGORY category) {
  switch (category) {
    case X_CONFIG_CATEGORY::XCONFIG_STATIC_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.static_settings);
    case X_CONFIG_CATEGORY::XCONFIG_STATISTIC_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.statistic);
    case X_CONFIG_CATEGORY::XCONFIG_SECURED_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.secured);
    case X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.user);
    case X_CONFIG_CATEGORY::XCONFIG_XNET_MACHINE_ACCOUNT_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.xnet_machine_account);
    case X_CONFIG_CATEGORY::XCONFIG_XNET_PARAMETERS_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.xnet_parameters);
    case X_CONFIG_CATEGORY::XCONFIG_MEDIA_CENTER_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.media_center);
    case X_CONFIG_CATEGORY::XCONFIG_CONSOLE_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.console);
    case X_CONFIG_CATEGORY::XCONFIG_DVD_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.dvd);
    case X_CONFIG_CATEGORY::XCONFIG_IPTV_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.iptv);
    case X_CONFIG_CATEGORY::XCONFIG_SYSTEM_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.system);
  }
  return nullptr;
}

const uint8_t* XConfig::CategoryBase(X_CONFIG_CATEGORY category) const {
  return const_cast<XConfig*>(this)->CategoryBase(category);
}

}  // namespace kernel
}  // namespace xe
