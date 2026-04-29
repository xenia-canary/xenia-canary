/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_NUI_NUI_CONSTANTS_H_
#define XENIA_HID_NUI_NUI_CONSTANTS_H_

#include <cstdint>

namespace xe {
namespace hid {
namespace nui {

// ----------------------------------------------------------------------------
// Skeleton joint indices.
//
// Order and indices match the public Xbox 360 Kinect SDK. Do not renumber:
// XAM consumers receive raw arrays indexed by these values, and any
// disagreement with the SDK enum produces silent skeletal mis-tracking
// that is very hard to diagnose.
//
// References:
//   - Microsoft Kinect for Windows SDK 1.8 NuiSkeleton.h
//   - Reverse-engineered XAM NUI headers (community)
// ----------------------------------------------------------------------------

enum SkeletonPosition : uint32_t {
  kSkeletonPositionHipCenter = 0,
  kSkeletonPositionSpine = 1,
  kSkeletonPositionShoulderCenter = 2,
  kSkeletonPositionHead = 3,
  kSkeletonPositionShoulderLeft = 4,
  kSkeletonPositionElbowLeft = 5,
  kSkeletonPositionWristLeft = 6,
  kSkeletonPositionHandLeft = 7,
  kSkeletonPositionShoulderRight = 8,
  kSkeletonPositionElbowRight = 9,
  kSkeletonPositionWristRight = 10,
  kSkeletonPositionHandRight = 11,
  kSkeletonPositionHipLeft = 12,
  kSkeletonPositionKneeLeft = 13,
  kSkeletonPositionAnkleLeft = 14,
  kSkeletonPositionFootLeft = 15,
  kSkeletonPositionHipRight = 16,
  kSkeletonPositionKneeRight = 17,
  kSkeletonPositionAnkleRight = 18,
  kSkeletonPositionFootRight = 19,
  kSkeletonPositionCount = 20,
};

// Per-joint tracking state (matches NUI_SKELETON_POSITION_TRACKING_STATE).
enum SkeletonPositionTrackingState : uint32_t {
  kSkeletonPositionNotTracked = 0,
  kSkeletonPositionInferred = 1,
  kSkeletonPositionTracked = 2,
};

// ----------------------------------------------------------------------------
// Skeleton frame layout.
// ----------------------------------------------------------------------------

// Maximum simultaneously tracked skeletons. Kinect v1 provides up to 6 slots,
// of which 2 may be "fully tracked" (joint data) and 4 "position only".
constexpr uint32_t kMaxSkeletons = 6;
constexpr uint32_t kMaxFullyTrackedSkeletons = 2;

// Per-skeleton tracking state.
enum SkeletonTrackingState : uint32_t {
  kSkeletonNotTracked = 0,
  kSkeletonPositionOnly = 1,
  kSkeletonTracked = 2,
};

// ----------------------------------------------------------------------------
// Sensor capabilities (returned by XamNuiGetSkeletonCapabilities et al).
//
// These are best-guess based on observed XAM NUI calls; expect to refine
// during Stage 0 telemetry.
// ----------------------------------------------------------------------------

enum SensorCapability : uint32_t {
  kCapabilitySkeleton = 1u << 0,
  kCapabilityDepth = 1u << 1,
  kCapabilityColor = 1u << 2,
  kCapabilityAudio = 1u << 3,  // Microphone array. Out of scope v1.
  kCapabilityMotor = 1u << 4,  // Tilt motor. Currently unused.
};

// ----------------------------------------------------------------------------
// XAM NUI error codes.
//
// Values transcribed from documented Kinect SDK HRESULTs. Xenia's existing
// xam_nui.cc returns E_FAIL for everything; we want richer signaling so
// guest software can distinguish "no sensor" from "sensor present but
// uninitialized" from "transient error".
// ----------------------------------------------------------------------------

constexpr uint32_t X_E_NUI_DEVICE_NOT_CONNECTED = 0x83010001u;
constexpr uint32_t X_E_NUI_DEVICE_NOT_READY = 0x83010002u;
constexpr uint32_t X_E_NUI_ALREADY_INITIALIZED = 0x83010003u;
constexpr uint32_t X_E_NUI_NOT_INITIALIZED = 0x83010004u;
constexpr uint32_t X_E_NUI_FRAME_NOT_READY = 0x83010005u;
constexpr uint32_t X_E_NUI_NOT_GENUINE = 0x83010006u;
constexpr uint32_t X_E_NUI_INSUFFICIENT_BANDWIDTH = 0x83010007u;
constexpr uint32_t X_E_NUI_NOT_SUPPORTED = 0x83010008u;

// ----------------------------------------------------------------------------
// Stream parameters.
// ----------------------------------------------------------------------------

constexpr uint32_t kColorFrameRateHz = 30;
constexpr uint32_t kDepthFrameRateHz = 30;
constexpr uint32_t kSkeletonFrameRateHz = 30;

constexpr uint32_t kColorWidth640 = 640;
constexpr uint32_t kColorHeight480 = 480;
constexpr uint32_t kColorWidth1280 = 1280;
constexpr uint32_t kColorHeight960 = 960;

constexpr uint32_t kDepthWidth80 = 80;
constexpr uint32_t kDepthHeight60 = 60;
constexpr uint32_t kDepthWidth320 = 320;
constexpr uint32_t kDepthHeight240 = 240;
constexpr uint32_t kDepthWidth640 = 640;
constexpr uint32_t kDepthHeight480 = 480;

}  // namespace nui
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NUI_NUI_CONSTANTS_H_
