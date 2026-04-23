/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_COMMON_H_
#define XENIA_AVATARS_COMMON_H_

namespace xe {
namespace avatars {

template <typename T>
struct Vector2 {
  T x;
  T y;
};

template <typename T>
struct Vector3 {
  T x;
  T y;
  T z;
};

template <typename T>
struct Vector4 {
  T x;
  T y;
  T z;
  T w;
};

template <typename T>
struct Quaternion {
  T x;
  T y;
  T z;
  T w;
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_COMMON_H_
