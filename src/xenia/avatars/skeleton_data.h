/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_SKELETON_DATA_H_
#define XENIA_AVATARS_SKELETON_DATA_H_

#include "xenia/avatars/skeleton.h"

namespace xe {
namespace avatars {

std::shared_ptr<Skeleton> LoadSkeleton(uint32_t skeleton_version,
                                       SkeletonLoadOptions load_options);

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_GUEST_LOAD_ASSET_H_
