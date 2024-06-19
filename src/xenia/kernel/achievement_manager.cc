/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "achievement_manager.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/ui/imgui_notification.h"

DEFINE_bool(show_achievement_notification, false,
            "Show achievement notification on screen.", "UI");

DECLARE_int32(user_language);

namespace xe {
namespace kernel {

AchievementManager::AchievementManager() {
  for (auto& achievements : unlocked_achievements) {
    achievements.clear();
  }
}

void AchievementManager::EarnAchievement(uint32_t user_index,
                                         uint32_t achievement_id) {
  if (user_index >= 4) {
    return;
  }

  if (IsAchievementUnlocked(user_index, achievement_id)) {
    return;
  }

  const Emulator* emulator = kernel_state()->emulator();
  ui::WindowedAppContext& app_context =
      kernel_state()->emulator()->display_window()->app_context();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  const util::XdbfGameData title_xdbf = kernel_state()->title_xdbf();
  const util::XdbfAchievementTableEntry achievement =
      title_xdbf.GetAchievement(achievement_id);

  if (!achievement.id) {
    return;
  }

  const XLanguage title_language = title_xdbf.GetExistingLanguage(
      static_cast<XLanguage>(cvars::user_language));

  const std::string label =
      title_xdbf.GetStringTableEntry(title_language, achievement.label_id);
  const std::string desc = title_xdbf.GetStringTableEntry(
      title_language, achievement.description_id);

  XELOGI("Achievement unlocked: {}", label);

  unlocked_achievements[user_index][achievement_id] =
      Clock::QueryHostSystemTime();
  Save(user_index);
  // Even if we disable popup we still should store info that this
  // achievement was earned.
  if (!cvars::show_achievement_notification) {
    return;
  }

  const std::string description =
      fmt::format("{}G - {}", achievement.gamerscore, label);

  app_context.CallInUIThread([imgui_drawer, description]() {
    new xe::ui::AchievementNotificationWindow(
        imgui_drawer, "Achievement unlocked", description, 0,
        kernel_state()->notification_position_);
  });
}

bool AchievementManager::IsAchievementUnlocked(uint32_t user_index,
                                               uint32_t achievement_id) {
  if (user_index >= 4) {
    return false;
  }
  auto itr = unlocked_achievements[user_index].find(achievement_id);

  return itr != unlocked_achievements[user_index].cend();
}

uint64_t AchievementManager::GetAchievementUnlockTime(uint32_t user_index,
                                                      uint32_t achievement_id) {
  if (user_index >= 4) {
    return 0;
  }
  auto itr = unlocked_achievements[user_index].find(achievement_id);
  if (itr == unlocked_achievements[user_index].cend()) {
    return 0;
  }

  return itr->second;
}

void AchievementManager::Save(uint32_t user_index) {
  if (user_index >= 4) {
    return;
  }

  if (!kernel_state()->title_id()) {
    return;
  }

  const std::filesystem::path content_dir =
      kernel_state()->content_manager()->ResolveGameUserContentPath();

  std::filesystem::create_directories(content_dir);
  std::filesystem::path file_path =
      content_dir / xe::to_path("achievements.bin");
  FILE* file = xe::filesystem::OpenFile(file_path, "wb");
  if (!file) {
    return;
  }

  auto& ach_map = unlocked_achievements[user_index];

  uint32_t num_unlocks = static_cast<uint32_t>(ach_map.size());
  fwrite(&num_unlocks, sizeof(uint32_t), 1, file);

  for (auto& elem : ach_map) {
    uint32_t ach_id = elem.first;
    uint64_t ach_time = elem.second;
    fwrite(&ach_id, sizeof(uint32_t), 1, file);
    fwrite(&ach_time, sizeof(uint64_t), 1, file);
  }

  fclose(file);
}

void AchievementManager::Load(uint32_t user_index) {
  if (user_index >= 4) {
    return;
  }

  auto& ach_map = unlocked_achievements[user_index];
  ach_map.clear();

  if (!kernel_state()->title_id()) {
    return;
  }

  const std::filesystem::path content_dir =
      kernel_state()->content_manager()->ResolveGameUserContentPath();
  std::filesystem::path file_path =
      content_dir / xe::to_path("achievements.bin");
  FILE* file = xe::filesystem::OpenFile(file_path, "rb");
  if (!file) {
    return;
  }

  uint32_t num_unlocks;
  if (fread(&num_unlocks, sizeof(uint32_t), 1, file) != 1) {
    fclose(file);
    return;
  }

  for (uint32_t i = 0; i < num_unlocks; i++) {
    uint32_t ach_id;
    uint64_t ach_time;
    if (fread(&ach_id, sizeof(uint32_t), 1, file) != 1 ||
        fread(&ach_time, sizeof(uint64_t), 1, file) != 1) {
      ach_map.clear();
      fclose(file);
      return;
    }

    ach_map[ach_id] = ach_time;
  }

  fclose(file);
}

}  // namespace kernel
}  // namespace xe
