/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XDBF_GPD_INFO_PROFILE_H_
#define XENIA_KERNEL_XAM_XDBF_GPD_INFO_PROFILE_H_

#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"

#include <string>
#include <vector>

#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

class GpdInfoProfile : public GpdInfo {
 public:
  GpdInfoProfile() : GpdInfo(kDashboardID) {};
  GpdInfoProfile(const std::vector<uint8_t> buffer)
      : GpdInfo(kDashboardID, buffer) {};

  ~GpdInfoProfile() = default;

  void AddNewTitle(const SpaInfo* title_data);
  // SPA-less overload. achievement_count / total_gamerscore default to 0
  // and get filled in on the next real launch via UpdateProfileGpd.
  void AddNewTitle(uint32_t title_id, const std::u16string& title_name,
                   uint32_t achievement_count = 0,
                   uint32_t total_gamerscore = 0);
  bool RemoveTitle(const uint32_t title_id);
  void UpdateTitleInfo(const uint32_t title_id,
                       X_XDBF_GPD_TITLE_PLAYED* title_data);

  const std::vector<const X_XDBF_GPD_TITLE_PLAYED*> GetTitlesInfo() const;
  X_XDBF_GPD_TITLE_PLAYED* GetTitleInfo(const uint32_t title_id);

  std::u16string GetTitleName(const uint32_t title_id) const;

  // Xenia-specific: title launch path(s), read by the GPD->library migration.
  std::optional<std::filesystem::path> GetTitlePath(uint32_t title_id) const;

  struct DiscInfo {
    std::filesystem::path path;
    std::string label;
  };
  std::vector<DiscInfo> GetTitleDiscs(uint32_t title_id) const;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XDBF_GPD_INFO_PROFILE_H_
