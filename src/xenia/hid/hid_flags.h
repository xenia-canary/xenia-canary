/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_HID_FLAGS_H_
#define XENIA_HID_HID_FLAGS_H_

#include "xenia/base/cvar.h"

DECLARE_bool(guide_button);
DECLARE_int32(gyro_mode);
DECLARE_double(gyro_sensitivity_x);
DECLARE_double(gyro_sensitivity_y);
DECLARE_double(gyro_anti_deadzone);
DECLARE_double(gyro_roll_mix);
DECLARE_double(gyro_smoothing_threshold);
DECLARE_bool(gyro_invert_x);
DECLARE_bool(gyro_invert_y);

#endif  // XENIA_HID_HID_FLAGS_H_
