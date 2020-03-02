/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/user_profile.h"

#include <sstream>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/crypto_utils.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/vfs/devices/host_path_device.h"

DECLARE_int32(license_mask);

DEFINE_string(user_0_xuid, "", "XUID of the profile to use for user 0 (E0...",
              "Profiles");

DEFINE_int32(user_0_state, 1, "User 0 signin state (0/1/2)", "Profiles");

DEFINE_string(user_1_xuid, "", "XUID of the profile to use for user 1 (E0...",
              "Profiles");

DEFINE_int32(user_1_state, 0, "User 1 signin state (0/1/2)", "Profiles");

DEFINE_string(user_2_xuid, "", "XUID of the profile to use for user 2 (E0...",
              "Profiles");

DEFINE_int32(user_2_state, 0, "User 2 signin state (0/1/2)", "Profiles");

DEFINE_string(user_3_xuid, "", "XUID of the profile to use for user 3 (E0...",
              "Profiles");

DEFINE_int32(user_3_state, 0, "User 3 signin state (0/1/2)", "Profiles");

namespace xe {
namespace kernel {
namespace xam {

void UserProfile::CreateUsers(KernelState* kernel_state,
                              std::unique_ptr<UserProfile>* profiles) {
  std::string xuids[] = {cvars::user_0_xuid, cvars::user_1_xuid,
                         cvars::user_2_xuid, cvars::user_3_xuid};
  int32_t states[] = {cvars::user_0_state, cvars::user_1_state,
                      cvars::user_2_state, cvars::user_3_state};

  // Create UserProfile instances for all 4 user slots
  // But only login the slots the player has already configured
  for (int i = 0; i < kMaxNumUsers; i++) {
    auto xuid_str = xuids[i];
    int32_t state = states[i];

    uint64_t xuid = 0;
    if (!xuid_str.empty()) {
      xuid = std::strtoull(xuid_str.c_str(), 0, 16);
    }

    auto profile = std::make_unique<UserProfile>(kernel_state);
    profile->signin_state(state);
    if (state > 0) {
      profile->Login(xuid);
    }
    profiles[i] = std::move(profile);
  }
}

uint64_t UserProfile::XuidFromPath(const std::wstring& path) {
  filesystem::FileInfo info;
  if (!filesystem::GetInfo(path, &info)) {
    return -1;
  }

  auto fname = info.name;
  size_t len = fname.length();

  // Get substring of name that has valid hex chars
  // TODO: atm this just stops at . if the filename has it..
  // it should probably check each char is valid hex instead
  size_t i = 0;
  for (auto c : fname) {
    if (c == '.') {
      len = i;
      break;
    }
    i++;
  }

  auto xuid_str = xe::to_string(fname.substr(0, len));
  return std::strtoull(xuid_str.c_str(), 0, 16);
}

std::wstring UserProfile::base_path(KernelState* state) {
  return xe::to_absolute_path(
      state->content_manager()->ResolvePackageRoot(0x10000, 0xFFFE07D1));
}

std::map<uint64_t, std::tuple<std::wstring, X_XAMACCOUNTINFO>>
UserProfile::Enumerate(KernelState* state, bool exclude_signed_in) {
  std::map<uint64_t, std::tuple<std::wstring, X_XAMACCOUNTINFO>> map;

  auto files = filesystem::ListFiles(base_path(state));
  for (auto f : files) {
    auto profile_path = f.path + f.name;
    // use .dir for this profile if it exists
    auto dir_path = profile_path + L".dir\\";
    if (filesystem::PathExists(dir_path)) {
      profile_path = dir_path;
    }

    auto xuid = XuidFromPath(profile_path);
    if (!xuid) {
      continue;  // could cause infinite loop, so skip any invalid ones
    }
    if (map.count(xuid)) {
      continue;  // already added this xuid!
    }

    bool in_use = false;
    if (exclude_signed_in) {
      // check if this xuid is signed in already
      for (uint32_t i = 0; i < state->num_profiles(); i++) {
        auto profile = state->user_profile(i);
        if (!profile) {
          continue;
        }
        if (profile->xuid() == xuid || profile->xuid_offline() == xuid) {
          in_use = true;
          break;
        }
      }
    }

    if (!in_use) {
      // Login with UserProfile so we can retrieve info about it
      auto profile = std::make_unique<UserProfile>(state);
      if (!profile->Login(xuid)) {
        continue;
      }

      map[xuid] = std::tuple<std::wstring, X_XAMACCOUNTINFO>(profile_path,
                                                             profile->account_);
    }
  }

  return map;
}

std::string X_XAMACCOUNTINFO::GetGamertagString() const {
  return xe::to_string(std::wstring(gamertag));
}

std::wstring UserProfile::path() const {
  if (!profile_path_.empty()) {
    return profile_path_;
  }

  return base_path_;
}

std::wstring UserProfile::path(uint64_t xuid) const {
  wchar_t path[4096];
  std::swprintf(path, 4096, L"%s%llX", base_path_.c_str(), xuid);
  return path;
}

bool UserProfile::DecryptAccountFile(const uint8_t* data,
                                     X_XAMACCOUNTINFO* output, bool devkit) {
  const uint8_t* key = util::GetXeKey(0x19, devkit);
  if (!key) {
    return false;  // this shouldn't happen...
  }

  // Generate RC4 key from data hash
  uint8_t rc4_key[0x14];
  util::HmacSha(key, 0x10, data, 0x10, 0, 0, 0, 0, rc4_key, 0x14);

  uint8_t dec_data[sizeof(X_XAMACCOUNTINFO) + 8];

  // Decrypt data
  util::RC4(rc4_key, 0x10, data + 0x10, sizeof(dec_data), dec_data,
            sizeof(dec_data));

  // Verify decrypted data against hash
  uint8_t data_hash[0x14];
  util::HmacSha(key, 0x10, dec_data, sizeof(dec_data), 0, 0, 0, 0, data_hash,
                0x14);

  if (std::memcmp(data, data_hash, 0x10) == 0) {
    // Copy account data to output
    std::memcpy(output, dec_data + 8, sizeof(X_XAMACCOUNTINFO));

    // Swap gamertag endian
    xe::copy_and_swap<wchar_t>(output->gamertag, output->gamertag, 0x10);
    return true;
  }

  return false;
}

void UserProfile::EncryptAccountFile(const X_XAMACCOUNTINFO* input,
                                     uint8_t* output, bool devkit) {
  const uint8_t* key = util::GetXeKey(0x19, devkit);
  if (!key) {
    return;  // this shouldn't happen...
  }

  X_XAMACCOUNTINFO* output_acct = (X_XAMACCOUNTINFO*)(output + 0x18);
  std::memcpy(output_acct, input, sizeof(X_XAMACCOUNTINFO));

  // Swap gamertag endian
  xe::copy_and_swap<wchar_t>(output_acct->gamertag, output_acct->gamertag,
                             0x10);

  // Set confounder, should be random but meh
  std::memset(output + 0x10, 0xFD, 8);

  // Encrypted data = xam account info + 8 byte confounder
  uint32_t enc_data_size = sizeof(X_XAMACCOUNTINFO) + 8;

  // Set data hash
  uint8_t data_hash[0x14];
  util::HmacSha(key, 0x10, output + 0x10, enc_data_size, 0, 0, 0, 0, data_hash,
                0x14);

  std::memcpy(output, data_hash, 0x10);

  // Generate RC4 key from data hash
  uint8_t rc4_key[0x14];
  util::HmacSha(key, 0x10, data_hash, 0x10, 0, 0, 0, 0, rc4_key, 0x14);

  // Encrypt data
  util::RC4(rc4_key, 0x10, output + 0x10, enc_data_size, output + 0x10,
            enc_data_size);
}

UserProfile::UserProfile(KernelState* kernel_state)
    : kernel_state_(kernel_state), dash_gpd_(kDashboardID) {
  base_path_ = base_path(kernel_state);  // store path for later
}

std::wstring UserProfile::ExtractProfile(const std::wstring& path) {
  auto package = path;
  auto package_dir = package + L".dir\\";
  if (filesystem::PathExists(package_dir)) {
    return package_dir;
  }

  // Get info about the path
  filesystem::FileInfo info;
  if (!filesystem::GetInfo(package, &info)) {
    // Path doesn't exist - return the .dir version to create later
    return package_dir;
  }

  // If path points to a file, open it as STFS and extract
  if (info.type == filesystem::FileInfo::Type::kFile) {
    XELOGI("MountProfile: extracting STFS profile %S", package.c_str());
    auto mount_path = "\\Device\\Profile_" + xe::to_string(info.name);

    // Register the container in the virtual filesystem.
    auto device =
        std::make_unique<vfs::StfsContainerDevice>(mount_path, package);
    if (!device->Initialize()) {
      XELOGE(
          "MountProfile: Unable to mount %S as STFS; file not found or "
          "corrupt.",
          package.c_str());
      return L"";
    }
    device->ExtractToFolder(package_dir);
    return package_dir;
  }

  // Must be an existing directory, just return the path for it
  return package + L"\\";
}

bool UserProfile::Create(X_XAMACCOUNTINFO* account, bool generate_gamertag) {
  // TODO: check for any existing profile with the same account->gamertag!

  static int num_xuids = 0;  // TODO: we really should save this between runs
  // Create some unique IDs for the profile
  xuid_offline_ = 0xE0000000BEEFCAFE + num_xuids;
  account->xuid_online = 0x09000000BEEFCAFE + num_xuids;
  if (generate_gamertag) {
    std::swprintf(account->gamertag,
                  sizeof account->gamertag / sizeof *account->gamertag,
                  L"XeniaUser%d",
                  num_xuids);
  }
  num_xuids++;

  profile_path_ = path(xuid_offline_);
  profile_path_ = ExtractProfile(profile_path_);

  memcpy(&account_, account, sizeof(X_XAMACCOUNTINFO));

  if (!filesystem::PathExists(profile_path_)) {
    // Profile path doesn't exist - create new profile!
    if (!filesystem::CreateFolder(profile_path_)) {
      XELOGE("UserProfile::Create: Failed to create profile for '%S' at %S!",
             account->gamertag, path().c_str());
      return false;
    } else {
      // Write out an account file
      filesystem::CreateFile(
          path() + L"Account");  // MappedMemory needs an existing file...
      auto mmap_ = MappedMemory::Open(path() + L"Account",
                                      MappedMemory::Mode::kReadWrite, 0,
                                      sizeof(X_XAMACCOUNTINFO) + 0x18);
      if (!mmap_) {
        XELOGE("UserProfile::Create: Failed to create new Account at %S!",
               path().c_str());
        return false;
      } else {
        XELOGI("Writing Account file for '%S' to path %SAccount",
               account->gamertag, path().c_str());
        EncryptAccountFile(account, mmap_->data(), false);
        mmap_->Close();
      }

      // Dash GPD will be updated below, after default settings are added
    }
  }
  return true;
}

bool UserProfile::Login(uint64_t offline_xuid) {
  xuid_offline_ = offline_xuid;
  auto profile_path = path(xuid_offline_);

  if (!xuid_offline_) {
    // Try logging in as any non-signed-in profile...
    profile_path.clear();

    auto profiles = Enumerate(kernel_state_, true);
    if (profiles.size() > 0) {
      const auto& profile = profiles.begin();
      xuid_offline_ = profile->first;
      profile_path = std::get<0>(profile->second);
    }
  }

  if (profile_path.empty()) {
    XELOGW(
        "UserProfile::Login: Couldn't find available profile to login to, "
        "using temp. profile");

    memset(&account_, 0, sizeof(X_XAMACCOUNTINFO));
    if (!Create(&account_, true)) {
      return false;
    }
    profile_path = path(xuid_offline_);
  }

  // Try extracting profile to a folder, so we can modify any contents
  profile_path_ = ExtractProfile(profile_path);
  if (profile_path_.empty()) {
    XELOGW("UserProfile::Login: Failed to extract profile from %S!",
           profile_path.c_str());
    return false;
  }

  if (filesystem::PathExists(profile_path_)) {
    // Profile exists, load Account and any GPDs
    auto mmap_ =
        MappedMemory::Open(path() + L"Account", MappedMemory::Mode::kRead);
    if (mmap_) {
      XELOGI("Loading Account file from path %SAccount", path().c_str());

      X_XAMACCOUNTINFO tmp_acct;
      bool success = DecryptAccountFile(mmap_->data(), &tmp_acct);
      if (!success) {
        success = DecryptAccountFile(mmap_->data(), &tmp_acct, true);
      }

      if (!success) {
        XELOGW("Failed to decrypt Account file data");
      } else {
        std::memcpy(&account_, &tmp_acct, sizeof(X_XAMACCOUNTINFO));
        XELOGI("Loaded Account '%s' successfully!", name().c_str());
      }

      mmap_->Close();
    }

    XELOGD("Loading profile GPDs from path %S", path().c_str());

    mmap_ =
        MappedMemory::Open(path() + L"FFFE07D1.gpd", MappedMemory::Mode::kRead);
    if (mmap_) {
      dash_gpd_.Read(mmap_->data(), mmap_->size());
      mmap_->Close();
    } else {
      XELOGW("Failed to read dash GPD (FFFE07D1.gpd), using blank one");

      // Create empty settings syncdata, helps tools identify this XDBF as a GPD
      xdbf::Entry ent;
      ent.info.section = static_cast<uint16_t>(xdbf::GpdSection::kSetting);
      ent.info.id = 0x200000000;
      ent.data.resize(0x18);
      memset(ent.data.data(), 0, 0x18);
      dash_gpd_.UpdateEntry(ent);
    }

    // Load in any extra game GPDs
    std::vector<xdbf::TitlePlayed> titles;
    dash_gpd_.GetTitles(&titles);

    for (auto title : titles) {
      wchar_t fname[256];
      swprintf(fname, 256, L"%X.gpd", title.title_id);
      mmap_ = MappedMemory::Open(path() + fname, MappedMemory::Mode::kRead);
      if (!mmap_) {
        XELOGE("Failed to open GPD for title %X (%s)!", title.title_id,
               xe::to_string(title.title_name).c_str());
        continue;
      }

      xdbf::GpdFile title_gpd(title.title_id);
      bool result = title_gpd.Read(mmap_->data(), mmap_->size());
      mmap_->Close();

      if (!result) {
        XELOGE("Failed to read GPD for title %X (%s)!", title.title_id,
               xe::to_string(title.title_name).c_str());
        continue;
      }

      title_gpds_[title.title_id] = title_gpd;
    }
  }

  // Add some default settings games/apps usually ask for
  // Not every setting requested needs to be added though, since now we can
  // handle non-existing settings fine!
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMER_YAXIS_INVERSION, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_OPTION_CONTROLLER_VIBRATION, 3u));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMERCARD_ZONE, 0u));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMERCARD_REGION, 0u));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMERCARD_CRED, 0u));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMERCARD_HAS_VISION, 0u));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMERCARD_REP, 0.0f));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_OPTION_VOICE_MUTED, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_OPTION_VOICE_THRU_SPEAKERS, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_OPTION_VOICE_VOLUME, 0x64u));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMERCARD_PICTURE_KEY,
                                     L"gamercard_picture_key"));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMERCARD_PERSONAL_PICTURE,
                                     L"gamercard_personal_picture"));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMERCARD_MOTTO, L"gamercard_motto"));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMERCARD_TITLES_PLAYED, 1u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED, 0u));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMER_DIFFICULTY, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMER_CONTROL_SENSITIVITY, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMER_PREFERRED_COLOR_FIRST, 0xFFFF0000u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMER_PREFERRED_COLOR_SECOND, 0xFF00FF00u));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMER_ACTION_AUTO_AIM, 1u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMER_ACTION_AUTO_CENTER, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMER_RACE_TRANSMISSION, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMER_RACE_CAMERA_LOCATION, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMER_RACE_BRAKE_CONTROL, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMERCARD_TITLE_CRED_EARNED, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED, 0u));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMERCARD_USER_NAME, L"XeniaUserName"));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_GAMERCARD_USER_LOCATION,
                                     L"XeniaUserLocation"));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMERCARD_USER_URL, L"XeniaUserUrl"));
  AddSettingIfNotExist(
      xdbf::Setting(xdbf::XPROFILE_GAMERCARD_USER_BIO, L"XeniaUserBio"));

  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_TITLE_SPECIFIC1, {}));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_TITLE_SPECIFIC2, {}));
  AddSettingIfNotExist(xdbf::Setting(xdbf::XPROFILE_TITLE_SPECIFIC3, {}));

  // Make sure the dash GPD is up-to-date
  UpdateGpd(kDashboardID, dash_gpd_);

  XELOGI("Loaded %d profile GPDs", title_gpds_.size());
  return true;
}

void UserProfile::Logout() {
  *this = UserProfile(kernel_state_);  // Reset ourselves
}

xdbf::GpdFile* UserProfile::SetTitleSpaData(const xdbf::SpaFile& spa_data) {
  curr_title_id_ = 0;
  curr_gpd_ = nullptr;

  xdbf::X_XDBF_XTHD_DATA title_data;
  spa_data.GetTitleData(&title_data);

  curr_title_id_ = title_data.title_id;

  std::vector<xdbf::Achievement> spa_achievements;
  // TODO: let user choose locale?
  spa_data.GetAchievements(spa_data.GetDefaultLanguage(), &spa_achievements);

  // Check if title should be included in the dash GPD title list
  // These checks should hopefully match the same checks X360 uses
  bool title_included =
      title_data.title_type == xdbf::X_XDBF_XTHD_DATA::TitleType::kFull ||
      title_data.title_type == xdbf::X_XDBF_XTHD_DATA::TitleType::kDownload;

  if (title_data.flags &
      (uint32_t)xdbf::X_XDBF_XTHD_DATA::Flags::kAlwaysIncludeInProfile) {
    title_included = true;
  }

  if (title_data.flags &
      (uint32_t)xdbf::X_XDBF_XTHD_DATA::Flags::kNeverIncludeInProfile) {
    title_included = false;
  }

  // If arcade game, only include if license_mask is set
  if ((title_data.title_id >> 16) == 0x5841) {
    title_included = cvars::license_mask != 0;
  }

  xdbf::TitlePlayed title_info;
  auto gpd = title_gpds_.find(title_data.title_id);
  if (gpd != title_gpds_.end()) {
    auto& title_gpd = (*gpd).second;

    XELOGD("Updating existing GPD for title %X", (uint32_t)title_data.title_id);

    bool always_update_title = false;
    if (!dash_gpd_.GetTitle(title_data.title_id, &title_info)) {
      assert_always();
      XELOGE(
          "GPD exists but is missing XbdfTitlePlayed entry? (this shouldn't be "
          "happening!)");
      // Try to work around it...
      title_info.title_name = xe::to_wstring(spa_data.GetTitleName());
      title_info.title_id = title_data.title_id;
      title_info.achievements_possible = 0;
      title_info.achievements_earned = 0;
      title_info.gamerscore_total = 0;
      title_info.gamerscore_earned = 0;
      always_update_title = true;
    }
    title_info.last_played = Clock::QueryHostSystemTime();

    // Check SPA for any achievements current GPD might be missing
    // (maybe added in TUs etc?)
    bool ach_updated = false;
    for (auto ach : spa_achievements) {
      bool ach_exists = title_gpd.GetAchievement(ach.id, nullptr);
      if (ach_exists && !always_update_title) {
        continue;
      }

      // Achievement doesn't exist in current title info, lets add it
      title_info.achievements_possible++;
      title_info.gamerscore_total += ach.gamerscore;

      // If it doesn't exist in GPD, add it to that too
      if (!ach_exists) {
        XELOGD(
            "Adding new achievement %d (%s) from SPA (wasn't inside existing "
            "GPD)",
            ach.id, xe::to_string(ach.label).c_str());

        ach_updated = true;
        title_gpd.UpdateAchievement(ach);
      }
    }

    // Update dash with new title_info
    if (title_included) {
      dash_gpd_.UpdateTitle(title_info);
    }

    // Only write game GPD if achievements were updated
    if (ach_updated) {
      UpdateGpd(title_data.title_id, title_gpd);
    }
    UpdateGpd(kDashboardID, dash_gpd_);
  } else {
    // GPD not found... have to create it!
    XELOGI("Creating new GPD for title %X", (uint32_t)title_data.title_id);

    title_info.title_name = xe::to_wstring(spa_data.GetTitleName());
    title_info.title_id = title_data.title_id;
    title_info.last_played = Clock::QueryHostSystemTime();

    // Copy cheevos from SPA -> GPD
    auto new_gpd = xdbf::GpdFile(title_data.title_id);
    auto title_gpd = &new_gpd;
    if (title_data.title_id == kDashboardID) {
      // we're loading dash - may as well update dash gpd
      title_gpd = &dash_gpd_;
    } else {
      for (auto ach : spa_achievements) {
        title_gpd->UpdateAchievement(ach);

        title_info.achievements_possible++;
        title_info.gamerscore_total += ach.gamerscore;
      }
    }

    // Try copying achievement images if we can...
    for (auto ach : spa_achievements) {
      auto* image_entry = spa_data.GetEntry(
          static_cast<uint16_t>(xdbf::SpaSection::kImage), ach.image_id);
      if (image_entry) {
        title_gpd->UpdateEntry(*image_entry);
      }
    }

    // Try adding title image & name
    auto* title_image =
        spa_data.GetEntry(static_cast<uint16_t>(xdbf::SpaSection::kImage),
                          static_cast<uint64_t>(xdbf::SpaID::Title));
    if (title_image) {
      title_gpd->UpdateEntry(*title_image);
    }

    auto title_name = xe::to_wstring(spa_data.GetTitleName());
    if (title_name.length()) {
      xdbf::Entry title_name_ent;
      title_name_ent.info.section =
          static_cast<uint16_t>(xdbf::GpdSection::kString);
      title_name_ent.info.id = static_cast<uint64_t>(xdbf::SpaID::Title);
      title_name_ent.data.resize((title_name.length() + 1) * 2);
      xe::copy_and_swap((wchar_t*)title_name_ent.data.data(),
                        title_name.c_str(), title_name.length());
      title_gpd->UpdateEntry(title_name_ent);
    }

    // Update dash GPD with title and write updated GPDs
    if (title_data.title_id != kDashboardID) {
      title_gpds_[title_data.title_id] = *title_gpd;
      if (title_included) {
        dash_gpd_.UpdateTitle(title_info);
      }
      UpdateGpd(title_data.title_id, title_gpds_[title_data.title_id]);
    }

    UpdateGpd(kDashboardID, dash_gpd_);
  }

  if (title_data.title_id != kDashboardID) {
    curr_gpd_ = &title_gpds_[title_data.title_id];
  } else {
    curr_gpd_ = &dash_gpd_;
  }

  // Print achievement list to log, ATM there's no other way for users to see
  // achievement status...
  std::vector<xdbf::Achievement> achievements;
  if (curr_gpd_->GetAchievements(&achievements)) {
    XELOGI("Achievement list:");

    for (auto ach : achievements) {
      // TODO: use ach.unachieved_desc for locked achievements?
      // depends on XdbfAchievementFlags::kShowUnachieved afaik
      XELOGI("%d - %s - %s - %d GS - %s", ach.id,
             xe::to_string(ach.label).c_str(),
             xe::to_string(ach.description).c_str(), ach.gamerscore,
             ach.IsUnlocked() ? "unlocked" : "locked");
    }

    XELOGI("Unlocked achievements: %d/%d, gamerscore: %d/%d\r\n",
           title_info.achievements_earned, title_info.achievements_possible,
           title_info.gamerscore_earned, title_info.gamerscore_total);
  }

  return curr_gpd_;
}

xdbf::GpdFile* UserProfile::GetTitleGpd(uint32_t title_id) {
  if (title_id == -1 || title_id == 0) {
    return curr_gpd_;
  }

  auto gpd = title_gpds_.find(title_id);
  if (gpd == title_gpds_.end()) {
    return nullptr;
  }

  return &(*gpd).second;
}

void UserProfile::GetTitles(std::vector<xdbf::GpdFile*>& titles) {
  for (auto title : title_gpds_) {
    titles.push_back(&title.second);
  }
}

bool UserProfile::UpdateTitleGpd(uint32_t title_id) {
  if (title_id == -1) {
    if (!curr_gpd_ || curr_title_id_ == -1) {
      return false;
    }
    title_id = curr_title_id_;
  }

  bool result = UpdateGpd(title_id, *curr_gpd_);
  if (!result) {
    XELOGE("UpdateTitleGpd failed on title %X!", title_id);
  } else {
    XELOGD("Updated title %X GPD successfully!", title_id);
  }
  return result;
}

bool UserProfile::UpdateAllGpds() {
  for (const auto& pair : title_gpds_) {
    auto gpd = pair.second;
    bool result = UpdateGpd(pair.first, gpd);
    if (!result) {
      XELOGE("UpdateGpdFiles failed on title %X!", pair.first);
      continue;
    }
  }

  // No need to update dash GPD here, the UpdateGpd func should take care of it
  // when needed
  return true;
}

bool UserProfile::UpdateGpd(uint32_t title_id, xdbf::GpdFile& gpd_data) {
  // Recalculate achievement totals
  uint32_t num_ach_total = 0;
  uint32_t num_ach_earned = 0;
  uint32_t gamerscore_total = 0;
  uint32_t gamerscore_earned = 0;

  // Update achievement total settings
  if (title_id != kDashboardID) {
    std::vector<xdbf::Achievement> gpd_achievements;
    gpd_data.GetAchievements(&gpd_achievements);

    for (auto ach : gpd_achievements) {
      num_ach_total++;
      gamerscore_total += ach.gamerscore;
      if (ach.IsUnlocked()) {
        num_ach_earned++;
        gamerscore_earned += ach.gamerscore;
      }
    }

    gpd_data.UpdateSetting(xdbf::Setting(
        xdbf::XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED, num_ach_earned));
    gpd_data.UpdateSetting(xdbf::Setting(
        xdbf::XPROFILE_GAMERCARD_TITLE_CRED_EARNED, gamerscore_earned));
  } else {
    // We're writing dash gpd, so recalculate total achievements
    // earned/gamerscore
    std::vector<xdbf::TitlePlayed> titles;
    dash_gpd_.GetTitles(&titles);
    for (auto title : titles) {
      num_ach_earned += title.achievements_earned;
      gamerscore_earned += title.gamerscore_earned;
    }

    dash_gpd_.UpdateSetting(xdbf::Setting(
        xdbf::XPROFILE_GAMERCARD_TITLES_PLAYED, (uint32_t)titles.size()));
    dash_gpd_.UpdateSetting(xdbf::Setting(
        xdbf::XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED, num_ach_earned));
    dash_gpd_.UpdateSetting(
        xdbf::Setting(xdbf::XPROFILE_GAMERCARD_CRED, gamerscore_earned));
  }

  size_t gpd_length = 0;
  if (!gpd_data.Write(nullptr, &gpd_length)) {
    XELOGE("Failed to get GPD size for title %X!", title_id);
    return false;
  }

  if (!filesystem::PathExists(path())) {
    filesystem::CreateFolder(path());
  }

  wchar_t fname[256];
  swprintf(fname, 256, L"%X.gpd", title_id);

  filesystem::CreateFile(path() + fname);
  auto mmap_ = MappedMemory::Open(
      path() + fname, MappedMemory::Mode::kReadWrite, 0, gpd_length);
  if (!mmap_) {
    XELOGE("Failed to open %X.gpd for writing!", title_id);
    return false;
  }

  // Write out GPD
  if (!gpd_data.Write(mmap_->data(), &gpd_length)) {
    XELOGE("Failed to write GPD data for %X!", title_id);
    mmap_->Close(gpd_length);
    return false;
  }

  // Check if we need to update dashboard data...
  if (title_id != kDashboardID) {
    xdbf::TitlePlayed title_info;
    if (dash_gpd_.GetTitle(title_id, &title_info)) {
      // Only update dash GPD if something has changed
      if (num_ach_total != title_info.achievements_possible ||
          num_ach_earned != title_info.achievements_earned ||
          gamerscore_total != title_info.gamerscore_total ||
          gamerscore_earned != title_info.gamerscore_earned) {
        title_info.achievements_possible = num_ach_total;
        title_info.achievements_earned = num_ach_earned;
        title_info.gamerscore_total = gamerscore_total;
        title_info.gamerscore_earned = gamerscore_earned;

        dash_gpd_.UpdateTitle(title_info);
        UpdateGpd(kDashboardID, dash_gpd_);
      }
    }
  }

  mmap_->Close(gpd_length);
  return true;
}

bool UserProfile::AddSettingIfNotExist(xdbf::Setting&& setting) {
  if (dash_gpd_.GetSetting(setting.id, nullptr)) {
    return false;
  }
  if (setting.value.type == xdbf::X_XUSER_DATA_TYPE::kBinary &&
      !setting.extraData.size()) {
    setting.extraData.resize(XPROFILEID_SIZE(setting.id));
  }
  return dash_gpd_.UpdateSetting(setting);
}

xdbf::GpdFile* UserProfile::GetDashboardGpd() { return &dash_gpd_; }

}  // namespace xam
}  // namespace kernel
}  // namespace xe
