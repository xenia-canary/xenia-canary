/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/skeleton_scaling.h"

#include "xenia/avatars/guest_asset.h"
#include "xenia/avatars/skeleton.h"
#include "xenia/base/math.h"

namespace xe {
namespace avatars {

static Vector3<float> Lerp(const Vector3<float>& v1, const Vector3<float>& v2,
                           float amount) {
  return {v1.x + amount * (v2.x - v1.x), v1.y + amount * (v2.y - v1.y),
          v1.z + amount * (v2.z - v1.z)};
}

static void Lerp(std::vector<Vector3<float>>& vectors, float factor) {
  for (size_t i = vectors.size(); i--;) {
    auto& vector = vectors[i];
    vector = Lerp({1.0f, 1.0f, 1.0f}, vector, factor);
  }
}

static void GetDefaultScales(std::vector<Vector3<float>>& scales) {
  scales.resize(72);
  for (size_t i = 0; i < scales.size(); ++i) {
    scales[i] = {1.0f, 1.0f, 1.0f};
  }
}

static void ApplyScales(std::shared_ptr<Skeleton> skeleton, float factor,
                        const std::vector<Vector3<float>>& scales) {
  factor = clamp_float(factor, 0.0f, 1.0f);

  for (size_t i = skeleton->joints.size(); i--;) {
    auto& joint = skeleton->joints[i];
    auto scale = Lerp({1.0f, 1.0f, 1.0f}, scales[i], factor);
    joint.pose.scale.x *= scale.x;
    joint.pose.scale.y *= scale.y;
    joint.pose.scale.z *= scale.z;
  }
}

void ApplyScalesToSkeletonV1(BodyType bodyType, float weightFactor,
                             float heightFactor,
                             std::shared_ptr<Skeleton> skeleton) {
  std::vector<Vector3<float>> scales;

  GetDefaultScales(scales);
  if (heightFactor >= 0.0f) {
    scales[0] = {1.1f, 1.1f, 1.1f};
    scales[19] = {0.9f, 0.9f, 0.9f};
  } else {
    scales[0] = {0.9f, 0.9f, 0.9f};
    scales[19] = {1.05f, 1.05f, 1.05f};
    heightFactor = -heightFactor;
  }
  ApplyScales(skeleton, heightFactor, scales);

  GetDefaultScales(scales);
  if (bodyType == BodyType::kMale) {
    if (weightFactor >= 0.0f) {
      scales[4] = {1.6f, 1.6f, 2.2f};
      scales[7] = {1.5f, 1.0f, 1.5f};
      scales[9] = {1.5f, 1.0f, 1.5f};
      scales[10] = {1.8f, 1.0f, 1.9f};
      scales[13] = {1.5f, 1.0f, 1.5f};
      scales[17] = {1.5f, 1.0f, 1.5f};
      scales[18] = {1.5f, 1.0f, 1.4f};
      scales[24] = {1.9f, 1.0f, 1.5f};
      scales[26] = {1.0f, 1.5f, 1.5f};
      scales[27] = {1.0f, 1.5f, 1.5f};
      scales[29] = {1.0f, 1.5f, 1.5f};
      scales[30] = {1.0f, 1.5f, 1.5f};
      scales[32] = {1.0f, 1.5f, 1.5f};
      scales[35] = {1.0f, 1.5f, 1.5f};
      Lerp(scales, 0.6f);
    } else {
      scales[4] = {0.8f, 1.0f, 0.9f};
      scales[7] = {0.7f, 1.0f, 0.7f};
      scales[9] = {0.7f, 1.0f, 0.7f};
      scales[10] = {0.6f, 1.0f, 0.6f};
      scales[13] = {0.7f, 1.0f, 0.7f};
      scales[17] = {0.7f, 1.0f, 0.7f};
      scales[18] = {0.9f, 1.0f, 0.8f};
      scales[24] = {0.7f, 1.0f, 0.7f};
      scales[26] = {1.0f, 0.7f, 0.7f};
      scales[27] = {1.0f, 0.7f, 0.7f};
      scales[29] = {1.0f, 0.7f, 0.7f};
      scales[30] = {1.0f, 0.7f, 0.7f};
      scales[32] = {1.0f, 0.7f, 0.7f};
      scales[35] = {1.0f, 0.7f, 0.7f};
      weightFactor = -weightFactor;
    }
  } else if (bodyType == BodyType::kFemale) {
    if (weightFactor > 0) {
      scales[4] = {1.5f, 1.5f, 2.0f};
      scales[7] = {1.6f, 1.0f, 1.6f};
      scales[9] = {1.6f, 1.0f, 1.6f};
      scales[10] = {1.6f, 1.0f, 2.0f};
      scales[13] = {1.6f, 1.0f, 1.6f};
      scales[17] = {1.6f, 1.0f, 1.6f};
      scales[18] = {1.6f, 1.0f, 1.6f};
      scales[24] = {2.0f, 1.0f, 1.6f};
      scales[26] = {1.0f, 1.6f, 1.6f};
      scales[27] = {1.0f, 1.6f, 1.6f};
      scales[29] = {1.0f, 1.6f, 1.6f};
      scales[30] = {1.0f, 1.6f, 1.6f};
      scales[32] = {1.0f, 1.6f, 1.6f};
      scales[35] = {1.0f, 1.6f, 1.6f};
      Lerp(scales, 0.6f);
    } else {
      scales[4] = {0.65f, 1.0f, 0.7f};
      scales[7] = {0.7f, 1.0f, 0.7f};
      scales[9] = {0.7f, 1.0f, 0.7f};
      scales[10] = {0.7f, 1.0f, 0.5f};
      scales[13] = {0.7f, 1.0f, 0.7f};
      scales[17] = {0.7f, 1.0f, 0.7f};
      scales[18] = {0.8f, 1.0f, 0.7f};
      scales[24] = {0.7f, 1.0f, 0.7f};
      scales[26] = {1.0f, 0.7f, 0.7f};
      scales[27] = {1.0f, 0.7f, 0.7f};
      scales[29] = {1.0f, 0.7f, 0.7f};
      scales[30] = {1.0f, 0.7f, 0.7f};
      scales[32] = {1.0f, 0.7f, 0.7f};
      scales[35] = {1.0f, 0.7f, 0.7f};
      weightFactor = -weightFactor;
    }
  }
  ApplyScales(skeleton, weightFactor, scales);
}

void ApplyScalesToSkeletonV2(BodyType bodyType, float weightFactor,
                             float heightFactor,
                             std::shared_ptr<Skeleton> skeleton) {
  std::vector<Vector3<float>> scales;

  GetDefaultScales(scales);
  if (heightFactor >= 0.0f) {
    scales[0] = {1.1f, 1.1f, 1.1f};
    scales[19] = {0.9f, 0.9f, 0.9f};
  } else {
    scales[0] = {0.9f, 0.9f, 0.9f};
    scales[19] = {1.05f, 1.05f, 1.05f};
    heightFactor = -heightFactor;
  }
  ApplyScales(skeleton, heightFactor, scales);

  GetDefaultScales(scales);
  if (bodyType == BodyType::kMale) {
    if (weightFactor >= 0.0f) {
      scales[4] = {1.6f, 1.6f, 2.2f};
      scales[7] = {1.5f, 1.0f, 1.5f};
      scales[9] = {1.5f, 1.0f, 1.5f};
      scales[10] = {1.8f, 1.0f, 1.9f};
      scales[13] = {1.5f, 1.0f, 1.5f};
      scales[17] = {1.5f, 1.0f, 1.5f};
      scales[18] = {1.5f, 1.0f, 1.4f};
      scales[24] = {1.9f, 1.0f, 1.5f};
      scales[26] = {1.0f, 1.5f, 1.5f};
      scales[27] = {1.0f, 1.5f, 1.5f};
      scales[29] = {1.0f, 1.5f, 1.5f};
      scales[30] = {1.0f, 1.5f, 1.5f};
      scales[32] = {1.0f, 1.5f, 1.5f};
      scales[35] = {1.0f, 1.5f, 1.5f};
      Lerp(scales, 0.6f);
    } else {
      scales[4] = {0.84f, 1.0f, 0.92f};
      scales[7] = {0.76f, 1.0f, 0.76f};
      scales[9] = {0.76f, 1.0f, 0.76f};
      scales[10] = {0.68f, 1.0f, 0.68f};
      scales[13] = {0.76f, 1.0f, 0.76f};
      scales[17] = {0.76f, 1.0f, 0.76f};
      scales[18] = {0.92f, 1.0f, 0.84f};
      scales[24] = {0.76f, 1.0f, 0.76f};
      scales[26] = {1.0f, 0.76f, 0.76f};
      scales[27] = {1.0f, 0.76f, 0.76f};
      scales[29] = {1.0f, 0.76f, 0.76f};
      scales[30] = {1.0f, 0.76f, 0.76f};
      scales[32] = {1.0f, 0.76f, 0.76f};
      scales[35] = {1.0f, 0.76f, 0.76f};
      weightFactor = -weightFactor;
    }
  } else if (bodyType == BodyType::kFemale) {
    if (weightFactor > 0) {
      scales[4] = {1.5f, 1.5f, 2.0f};
      scales[7] = {1.6f, 1.0f, 1.6f};
      scales[9] = {1.6f, 1.0f, 1.6f};
      scales[10] = {1.6f, 1.0f, 2.0f};
      scales[13] = {1.6f, 1.0f, 1.6f};
      scales[17] = {1.6f, 1.0f, 1.6f};
      scales[18] = {1.6f, 1.0f, 1.6f};
      scales[24] = {2.0f, 1.0f, 1.6f};
      scales[26] = {1.0f, 1.6f, 1.6f};
      scales[27] = {1.0f, 1.6f, 1.6f};
      scales[29] = {1.0f, 1.6f, 1.6f};
      scales[30] = {1.0f, 1.6f, 1.6f};
      scales[32] = {1.0f, 1.6f, 1.6f};
      scales[35] = {1.0f, 1.6f, 1.6f};
      Lerp(scales, 0.6f);
    } else {
      scales[4] = {0.79f, 1.0f, 0.82f};
      scales[7] = {0.82f, 1.0f, 0.82f};
      scales[9] = {0.82f, 1.0f, 0.82f};
      scales[10] = {0.82f, 1.0f, 0.7f};
      scales[13] = {0.82f, 1.0f, 0.82f};
      scales[17] = {0.82f, 1.0f, 0.82f};
      scales[18] = {0.88f, 1.0f, 0.82f};
      scales[24] = {0.82f, 1.0f, 0.82f};
      scales[26] = {1.0f, 0.82f, 0.82f};
      scales[27] = {1.0f, 0.82f, 0.82f};
      scales[29] = {1.0f, 0.82f, 0.82f};
      scales[30] = {1.0f, 0.82f, 0.82f};
      scales[32] = {1.0f, 0.82f, 0.82f};
      scales[35] = {1.0f, 0.82f, 0.82f};
      weightFactor = -weightFactor;
    }
  }
  ApplyScales(skeleton, weightFactor, scales);
}

}  // namespace avatars
}  // namespace xe
