/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_SKELETON_SCALING_H_
#define XENIA_AVATARS_SKELETON_SCALING_H_

#include <memory>
#include <vector>

#include "xenia/avatars/guest_asset.h"
#include "xenia/avatars/skeleton.h"

namespace xe {
namespace avatars {

void ApplyScalesToSkeletonV1(BodyType bodyType, float weightFactor,
                             float heightFactor,
                             std::shared_ptr<Skeleton> skeleton);

void ApplyScalesToSkeletonV2(BodyType bodyType, float weightFactor,
                             float heightFactor,
                             std::shared_ptr<Skeleton> skeleton);

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_SKELETON_SCALING_H_
