/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_ACHIEVEMENT_MANAGER_H_
#define XENIA_KERNEL_ACHIEVEMENT_MANAGER_H_

#include <map>
#include <string>
#include <vector>

#include "xenia/xbox.h"

namespace xe {
namespace kernel {

struct X_ACHIEVEMENT_UNLOCK_TIME {
  xe::be<uint32_t> high_part;
  xe::be<uint32_t> low_part;
};

struct X_ACHIEVEMENT_DETAILS {
  xe::be<uint32_t> id;
  xe::be<uint32_t> label_ptr;
  xe::be<uint32_t> description_ptr;
  xe::be<uint32_t> unachieved_ptr;
  xe::be<uint32_t> image_id;
  xe::be<uint32_t> gamerscore;
  X_ACHIEVEMENT_UNLOCK_TIME unlock_time;
  xe::be<uint32_t> flags;

  static const size_t kStringBufferSize = 464;
};
static_assert_size(X_ACHIEVEMENT_DETAILS, 36);

// This is structure used inside GPD file.
// GPD is writeable XDBF.
// There are two info instances
// 1. In Dashboard ID which contains single GPD that contains info about any
// booted game (name, title_id, last boot time etc)
// 2. In specific Title ID directory GPD contains there structure below for
// every achievement. (unlocked or not)
struct AchievementGpdStructure {
  xe::be<uint32_t> struct_size;
  xe::be<uint32_t> achievement_id;
  xe::be<uint32_t> image_id;
  xe::be<uint32_t> gamerscore;
  xe::be<uint32_t> flags;
  X_ACHIEVEMENT_UNLOCK_TIME unlock_time;
  std::u16string achievement_name;
  std::u16string unlocked_description;
  std::u16string locked_description;
};

enum class AchievementType : uint32_t {
  kCompletion = 1,
  kLeveling = 2,
  kUnlock = 3,
  kEvent = 4,
  kTournament = 5,
  kCheckpoint = 6,
  kOther = 7,
};

enum class AchievementPlatform : uint32_t {
  kX360 = 0x100000,
  kPC = 0x200000,
  kMobile = 0x300000,
  kWebGames = 0x400000,
};

enum class AchievementFlags : uint32_t {
  kTypeMask = 0x7,
  kShowUnachieved = 0x8,
  kAchievedOnline = 0x10000,
  kAchieved = 0x20000,
  kNotAchievable = 0x40000,
  kWasNotAchievable = 0x80000,
  kPlatformMask = 0x700000,
  kColorizable = 0x1000000,  // avatar awards only?
};

class AchievementBackendInterface {
 public:
  virtual ~AchievementBackendInterface() {};

  virtual void EarnAchievement(const uint64_t xuid, const uint32_t title_id,
                               const uint32_t achievement_id) = 0;

  virtual bool IsAchievementUnlocked(const uint64_t xuid,
                                     const uint32_t title_id,
                                     const uint32_t achievement_id) const = 0;

  virtual const AchievementGpdStructure* GetAchievementInfo(
      const uint64_t xuid, const uint32_t title_id,
      const uint32_t achievement_id) const = 0;
  virtual const std::vector<AchievementGpdStructure*> GetTitleAchievements(
      const uint64_t xuid, const uint32_t title_id) const = 0;
  virtual bool LoadAchievementsData(const uint64_t xuid,
                                    const uint32_t title_id) = 0;

 private:
  virtual bool SaveAchievementsData(const uint64_t xuid,
                                    const uint32_t title_id) = 0;
  virtual bool SaveAchievementData(const uint64_t xuid, const uint32_t title_id,
                                   const uint32_t achievement_id) = 0;
};

class GpdAchievementBackend : public AchievementBackendInterface {
 public:
  GpdAchievementBackend();
  ~GpdAchievementBackend();

  void EarnAchievement(const uint64_t xuid, const uint32_t title_id,
                       const uint32_t achievement_id) override;
  bool IsAchievementUnlocked(const uint64_t xuid, const uint32_t title_id,
                             const uint32_t achievement_id) const override;
  const AchievementGpdStructure* GetAchievementInfo(
      const uint64_t xuid, const uint32_t title_id,
      const uint32_t achievement_id) const override;
  const std::vector<AchievementGpdStructure*> GetTitleAchievements(
      const uint64_t xuid, const uint32_t title_id) const override;
  bool LoadAchievementsData(const uint64_t xuid,
                            const uint32_t title_id) override;

 private:
  using UsersAchievements =
      std::map<uint64_t, std::vector<AchievementGpdStructure>>;

  AchievementGpdStructure* GetAchievementInfoInternal(
      const uint64_t xuid, const uint32_t title_id,
      const uint32_t achievement_id) const;

  bool SaveAchievementsData(const uint64_t xuid,
                            const uint32_t title_id) override {
    return 0;
  };
  bool SaveAchievementData(const uint64_t xuid, const uint32_t title_id,
                           const uint32_t achievement_id) override;

  UsersAchievements achievements_;
};

class AchievementManager {
 public:
  AchievementManager();

  void LoadTitleAchievements(const uint64_t xuid,
                             const uint32_t title_id) const;

  void EarnAchievement(const uint32_t user_index, const uint32_t title_id,
                       const uint32_t achievement_id) const;
  void EarnAchievement(const uint64_t xuid, const uint32_t title_id,
                       const uint32_t achievement_id) const;
  const AchievementGpdStructure* GetAchievementInfo(
      const uint64_t xuid, const uint32_t title_id,
      const uint32_t achievement_id) const;
  const std::vector<AchievementGpdStructure*> GetTitleAchievements(
      const uint64_t xuid, const uint32_t title_id) const;

 private:
  bool DoesAchievementExist(const uint32_t achievement_id) const;
  void ShowAchievementEarnedNotification(
      const AchievementGpdStructure* achievement) const;

  // This contains all backends with exception of default storage.
  std::vector<std::unique_ptr<AchievementBackendInterface>>
      achievement_backends_;

  std::unique_ptr<AchievementBackendInterface> default_achievements_backend_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_ACHIEVEMENT_MANAGER_H_
