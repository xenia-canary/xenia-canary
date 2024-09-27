/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/achievement_manager.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/ui/imgui_guest_notification.h"

DEFINE_bool(show_achievement_notification, false,
            "Show achievement notification on screen.", "UI");

DEFINE_string(default_achievement_backend, "",
              "Defines which achievement backend should be used as an defult. "
              "Possible options: [].",
              "Achievements");

DECLARE_int32(user_language);

namespace xe {
namespace kernel {

GpdAchievementBackend::GpdAchievementBackend() { achievements_.clear(); }

GpdAchievementBackend::~GpdAchievementBackend() {}

void GpdAchievementBackend::EarnAchievement(const uint64_t xuid,
                                            const uint32_t title_id,
                                            const uint32_t achievement_id) {
  const auto user = kernel_state()->user_profile(xuid);
  if (!user) {
    return;
  }

  auto achievement = GetAchievementInfoInternal(xuid, title_id, achievement_id);
  if (!achievement) {
    return;
  }

  XELOGI("Player: {} Unlocked Achievement: {}", user->name(),
         xe::to_utf8(xe::load_and_swap<std::u16string>(
             achievement->achievement_name.c_str())));

  const uint64_t unlock_time = Clock::QueryHostSystemTime();
  achievement->flags =
      achievement->flags | static_cast<uint32_t>(AchievementFlags::kAchieved);
  achievement->unlock_time.high_part = static_cast<uint32_t>(unlock_time >> 32);
  achievement->unlock_time.low_part = static_cast<uint32_t>(unlock_time);

  SaveAchievementData(xuid, title_id, achievement_id);
}

AchievementGpdStructure* GpdAchievementBackend::GetAchievementInfoInternal(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  const auto user = kernel_state()->user_profile(xuid);
  if (!user) {
    return nullptr;
  }

  if (!achievements_.count(xuid)) {
    return nullptr;
  }

  for (auto& entry : achievements_.at(xuid)) {
    if (entry.achievement_id == achievement_id) {
      return const_cast<AchievementGpdStructure*>(&entry);
    }
  }

  return nullptr;
}

const AchievementGpdStructure* GpdAchievementBackend::GetAchievementInfo(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  return GetAchievementInfoInternal(xuid, title_id, achievement_id);
}

bool GpdAchievementBackend::IsAchievementUnlocked(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  const auto achievement =
      GetAchievementInfoInternal(xuid, title_id, achievement_id);

  return (achievement->flags &
          static_cast<uint32_t>(AchievementFlags::kAchieved)) != 0;
}

const std::vector<AchievementGpdStructure*>
GpdAchievementBackend::GetTitleAchievements(const uint64_t xuid,
                                            const uint32_t title_id) const {
  return {};
}

bool GpdAchievementBackend::LoadAchievementsData(const uint64_t xuid,
                                                 const uint32_t title_id) {
  const util::XdbfGameData title_xdbf = kernel_state()->title_xdbf();
  if (!title_xdbf.is_valid()) {
    return false;
  }

  const auto achievements = title_xdbf.GetAchievements();
  if (achievements.empty()) {
    return true;
  }

  const XLanguage title_language = title_xdbf.GetExistingLanguage(
      static_cast<XLanguage>(cvars::user_language));
  for (const auto& achievement : achievements) {
    const std::string label =
        title_xdbf.GetStringTableEntry(title_language, achievement.label_id);
    const std::string desc = title_xdbf.GetStringTableEntry(
        title_language, achievement.description_id);
    const std::string locked_desc = title_xdbf.GetStringTableEntry(
        title_language, achievement.unachieved_id);

    AchievementGpdStructure achievementData = {};
    achievementData.struct_size = 0x1C;
    achievementData.achievement_id = static_cast<uint32_t>(achievement.id);
    achievementData.image_id = achievement.image_id;
    achievementData.gamerscore = static_cast<uint32_t>(achievement.gamerscore);
    achievementData.flags = achievement.flags;
    achievementData.unlock_time.high_part = 0;
    achievementData.unlock_time.low_part = 0;
    achievementData.achievement_name =
        xe::load_and_swap<std::u16string>(xe::to_utf16(label).c_str());
    achievementData.unlocked_description =
        xe::load_and_swap<std::u16string>(xe::to_utf16(desc).c_str());
    achievementData.locked_description =
        xe::load_and_swap<std::u16string>(xe::to_utf16(locked_desc).c_str());

    achievements_[xuid].push_back(achievementData);
  }

  // TODO(Gliniak): Here should be loader of GPD file for loaded title. That way
  // we can load flags and unlock_time from specific user.
  return true;
}

bool GpdAchievementBackend::SaveAchievementData(const uint64_t xuid,
                                                const uint32_t title_id,
                                                const uint32_t achievement_id) {
  return true;
}

AchievementManager::AchievementManager() {
  default_achievements_backend_ = std::make_unique<GpdAchievementBackend>();

  // Add any optional backend here.
};
void AchievementManager::EarnAchievement(const uint32_t user_index,
                                         const uint32_t title_id,
                                         const uint32_t achievement_id) const {
  const auto user = kernel_state()->user_profile(user_index);
  if (!user) {
    return;
  }

  EarnAchievement(user->xuid(), title_id, achievement_id);
};

void AchievementManager::EarnAchievement(const uint64_t xuid,
                                         const uint32_t title_id,
                                         const uint32_t achievement_id) const {
  if (!DoesAchievementExist(achievement_id)) {
    XELOGW(
        "{}: Achievement with ID: {} for title: {:08X} doesn't exist in "
        "database!",
        __func__, achievement_id, title_id);
    return;
  }
  // Always send request to unlock in 3PP backends. It's up to them to check if
  // achievement was unlocked
  for (auto& backend : achievement_backends_) {
    backend->EarnAchievement(xuid, title_id, achievement_id);
  }

  if (default_achievements_backend_->IsAchievementUnlocked(xuid, title_id,
                                                           achievement_id)) {
    return;
  }

  default_achievements_backend_->EarnAchievement(xuid, title_id,
                                                 achievement_id);

  if (!cvars::show_achievement_notification) {
    return;
  }

  const auto achievement = default_achievements_backend_->GetAchievementInfo(
      xuid, title_id, achievement_id);

  if (!achievement) {
    // Something went really wrong!
    return;
  }
  ShowAchievementEarnedNotification(achievement);
}

void AchievementManager::LoadTitleAchievements(const uint64_t xuid,
                                               const uint32_t title_id) const {
  default_achievements_backend_->LoadAchievementsData(xuid, title_id);
}

const AchievementGpdStructure* AchievementManager::GetAchievementInfo(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  return default_achievements_backend_->GetAchievementInfo(xuid, title_id,
                                                           achievement_id);
}

bool AchievementManager::DoesAchievementExist(
    const uint32_t achievement_id) const {
  const util::XdbfGameData title_xdbf = kernel_state()->title_xdbf();
  const util::XdbfAchievementTableEntry achievement =
      title_xdbf.GetAchievement(achievement_id);

  if (!achievement.id) {
    return false;
  }
  return true;
}

void AchievementManager::ShowAchievementEarnedNotification(
    const AchievementGpdStructure* achievement) const {
  const std::string description =
      fmt::format("{}G - {}", achievement->gamerscore,
                  xe::to_utf8(xe::load_and_swap<std::u16string>(
                      achievement->achievement_name.c_str())));

  const Emulator* emulator = kernel_state()->emulator();
  ui::WindowedAppContext& app_context =
      emulator->display_window()->app_context();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  app_context.CallInUIThread([imgui_drawer, description]() {
    new xe::ui::AchievementNotificationWindow(
        imgui_drawer, "Achievement unlocked", description, 0,
        kernel_state()->notification_position_);
  });
}

}  // namespace kernel
}  // namespace xe
