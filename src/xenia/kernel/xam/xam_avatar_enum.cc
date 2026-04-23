/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/guest_asset.h"
#include "xenia/avatars/guest_load_animation.h"
#include "xenia/avatars/guest_load_asset.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

using namespace xe::avatars;

struct X_AVATAR_ASSET_INFO {
  AssetId asset_id;
  be<uint32_t> categories;
  uint8_t body_mask;
  uint8_t random_body_mask;
  be<uint32_t> flags;
  uint8_t unknown[164];
  be<uint32_t> subcategory;
  AssetId associated_ids[2];
  be<uint16_t> name[64];
  be<uint32_t> data_offset;
  be<uint32_t> data_size;
  be<uint16_t> description[100];
  be<uint32_t> title_id;
  be<uint16_t> title_name[64];
  be<uint32_t> time_0;
  be<uint32_t> time_1;
  uint8_t skeleton_version_mask;
};
static_assert_size(X_AVATAR_ASSET_INFO, 0x2C4);

class AssetEnumerator {
 public:
  AssetEnumerator(uint32_t items_per_enumerate)
      : items_per_enumerate_(items_per_enumerate),
        item_count_(0),
        current_item_(0) {}
  ~AssetEnumerator() {}

  size_t item_size() const { return sizeof(X_AVATAR_ASSET_INFO); }
  size_t items_per_enumerate() const { return items_per_enumerate_; }

  X_AVATAR_ASSET_INFO* AppendItem() {
    size_t offset = buffer_.size();
    buffer_.resize(offset + item_size());
    item_count_++;
    return reinterpret_cast<X_AVATAR_ASSET_INFO*>(&buffer_.data()[offset]);
  }

  uint32_t WriteItems(uint8_t* buffer_data, uint32_t* written_count) {
    size_t count = std::min(item_count_ - current_item_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    size_t size = count * item_size();
    size_t offset = current_item_ * item_size();
    std::memcpy(buffer_data, buffer_.data() + offset, size);

    current_item_ += count;

    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }

    return X_ERROR_SUCCESS;
  }

 private:
  size_t items_per_enumerate_;
  size_t item_count_;
  size_t current_item_;
  std::vector<uint8_t> buffer_;
};

AssetEnumerator gAssetEnumerator(50);

dword_result_t XamAvatarBeginEnumAssets_entry(
    dword_t user_index, dword_t items_per_enumerate, dword_t category_mask,
    dword_t body_mask, dword_t flags, pointer_t<XAM_OVERLAPPED> overlapped) {
  assert_null(overlapped);

  gAssetEnumerator = AssetEnumerator(items_per_enumerate);

  auto asset_pack = kernel_state()->avatar_asset_pack();
  for (const auto& asset_info : asset_pack->asset_infos()) {
    if (!(asset_info.categories & category_mask) ||
        !(asset_info.bodies & body_mask)) {
      continue;
    }

    auto guest_info = gAssetEnumerator.AppendItem();
    *guest_info = {};
    guest_info->asset_id = asset_info.asset_ids[0];
    guest_info->categories = asset_info.categories;
    guest_info->body_mask = asset_info.bodies;
    guest_info->random_body_mask = asset_info.random_bodies;
    guest_info->flags = asset_info.flags;
    guest_info->subcategory = asset_info.subcategory;
    guest_info->data_offset = static_cast<uint32_t>(asset_info.data_offset);
    guest_info->data_size = static_cast<uint32_t>(asset_info.data_size);

    auto name = asset_pack->GetAssetName(asset_info.asset_ids[0]);
    xe::string_util::copy_and_swap_truncating(
        reinterpret_cast<char16_t*>(&guest_info->name[0]), name,
        countof(guest_info->name));
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarBeginEnumAssets, kAvatars, kStub);

dword_result_t XamAvatarEnumAssets_entry(lpvoid_t buffer,
                                         lpdword_t writtem_items, dword_t a3) {
  uint32_t dummy = 0;
  auto result = gAssetEnumerator.WriteItems(buffer, &dummy);
  *writtem_items = dummy;
  return result;
}
DECLARE_XAM_EXPORT1(XamAvatarEnumAssets, kAvatars, kStub);

dword_result_t XamAvatarEndEnumAssets_entry(dword_t a1) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarEndEnumAssets, kAvatars, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(AvatarEnum);
