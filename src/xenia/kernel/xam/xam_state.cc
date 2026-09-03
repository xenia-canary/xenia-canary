/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_state.h"
#include "xenia/emulator.h"

namespace xe {
namespace kernel {
namespace xam {

XamState::XamState(Emulator* emulator, KernelState* kernel_state)
    : kernel_state_(kernel_state),
      global_allocator_(kernel_state, 0x80D00000, 0x100000) {
  app_manager_ = std::make_unique<AppManager>();

  auto content_root = emulator->content_root();
  if (!content_root.empty()) {
    content_root = std::filesystem::absolute(content_root);
  }
  content_manager_ =
      std::make_unique<ContentManager>(kernel_state, content_root);

  user_tracker_ = std::make_unique<UserTracker>();
  profile_manager_ = std::make_unique<ProfileManager>(
      kernel_state, content_manager_.get(), user_tracker_.get());
  achievement_manager_ = std::make_unique<AchievementManager>();

  LoadLanguageLocaleFallback();
  LoadLanguageTypefacePatch();
  LoadIptvServiceName();

  AppManager::RegisterApps(kernel_state, app_manager_.get());
}

void XamState::LoadLanguageLocaleFallback() {
  if (!global_allocator_.is_valid()) {
    return;
  }

  const std::array<std::u16string_view, 18> locale_data = {
      u"",      u"",      u"ja-JP", u"de-DE", u"fr-FR",  u"es-ES",
      u"it-IT", u"ko-KR", u"zh-TW", u"pt-BR", u"zh-CHS", u"pl-PL",
      u"ru-RU", u"sv-SE", u"tr-TR", u"nb-NO", u"nl-NL",  u"zh-CHS"};

  for (size_t i = 1; i < locale_data.size(); i++) {
    language_fallback_address_[i] = global_allocator_.Write(locale_data.at(i));
  }
}

void XamState::LoadLanguageTypefacePatch() {
  if (!global_allocator_.is_valid()) {
    return;
  }

  const std::array<std::u16string_view, 7> patch_data = {
      u"",
      u"file://media:/XenonSCLatin.xttp2",
      u"file://media:/XenonCLatin.xttp2",
      u"file://media:/XenonJKLatin.xttp2",
      u"file://media:/XenonSCLatin.xttp1",
      u"file://media:/XenonCLatin.xttp1",
      u"file://media:/XenonJKLatin.xttp1"};

  for (size_t i = 0; i < patch_data.size(); i++) {
    language_type_face_patch_[i] = global_allocator_.Write(patch_data.at(i));
  }
}

uint32_t XamState::GetLanguageTypefacePatch(uint32_t language) const {
  // checks an out of bounds address to see if either a flag of 0x1000 or 0x2000
  // is set. If first then use xttp1, if other use xttp2, else just return empty
  // pointer. for now only usng xttp2
  uint32_t index = 0;
  if (language == 17) {  // kSChinese
    index = 1;
  } else if (language == 8) {  // kTChinese
    index = 2;
  } else {
    index = 3;
  }

  return language_type_face_patch_[index];
}

void XamState::LoadIptvServiceName() {
  if (!global_allocator_.is_valid()) {
    return;
  }
  constexpr size_t iptv_service_buffer_size = 0x78;
  iptv_name_address_ = global_allocator_.Allocate(iptv_service_buffer_size);
}

UserProfile* XamState::GetUserProfile(uint32_t user_index) const {
  if (user_index >= XUserMaxUserCount && user_index < XUserIndexLatest) {
    return nullptr;
  }

  return profile_manager_->GetProfile(static_cast<uint8_t>(user_index));
}

UserProfile* XamState::GetUserProfile(uint64_t xuid) const {
  return profile_manager_->GetProfile(xuid);
}

bool XamState::IsUserSignedIn(uint32_t user_index) const {
  return profile_manager_->GetProfile(static_cast<uint8_t>(user_index)) !=
         nullptr;
}

bool XamState::IsUserSignedIn(uint64_t xuid) const {
  return GetUserProfile(xuid) != nullptr;
}

void XamState::LoadSpaInfo(const SpaInfo* info) {
  if (!info) {
    return;
  }
  // Check if we have loaded SpaInfo already. If yes then check currently loaded
  // version.
  if (spa_info_) {
    // Trying to load spa with lower version, for whatever reason.
    if (*info <= *spa_info_) {
      return;
    }
  }

  spa_info_ = std::make_unique<SpaInfo>(*info);
  spa_info_->Load();
  user_tracker_->UpdateSpaInfo(spa_info_.get());
}

void XamState::SetContentRegisterCallback(uint32_t callback) {
  content_register_callback = callback;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
