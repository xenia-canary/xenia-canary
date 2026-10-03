/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/hid_flags.h"

DEFINE_bool(guide_button, true, "Forward guide button presses to guest.",
            "HID");

DEFINE_int32(gyro_mode, 2,
             "Gyro aiming for controllers with a gyroscope (DualShock 4, "
             "DualSense, Switch Pro...). Gyro motion is added to the right "
             "stick.\n"
             " 0 = Disabled\n"
             " 1 = Always on\n"
             " 2 = While the left trigger (LT / L2) is held\n"
             " 3 = While the left bumper (LB / L1) is held",
             "Gyro");
DEFINE_double(gyro_sensitivity_x, 1.0,
              "Horizontal gyro sensitivity. At 1.0 turning the controller at "
              "100 degrees per second fully deflects the right stick.",
              "Gyro");
DEFINE_double(gyro_sensitivity_y, 1.0,
              "Vertical gyro sensitivity. At 1.0 tilting the controller at "
              "100 degrees per second fully deflects the right stick.",
              "Gyro");
DEFINE_double(gyro_anti_deadzone, 0.20,
              "Minimum right stick deflection produced by any gyro motion, "
              "from 0 to 0.9. Compensates the game's own stick deadzone so "
              "small movements still register. Raise it if small motions are "
              "ignored, lower it if the aim drifts or jitters.",
              "Gyro");
DEFINE_double(gyro_roll_mix, 0.0,
              "How much rolling the controller (tilting it left/right) turns "
              "horizontally, added to yaw. 0 = yaw only, 1 = yaw + roll.",
              "Gyro");
DEFINE_double(gyro_smoothing_threshold, 4.0,
              "Rotation speed in degrees per second below which gyro input is "
              "smoothed to remove hand shake. 0 disables smoothing.",
              "Gyro");
DEFINE_bool(gyro_invert_x, false, "Invert horizontal gyro aiming.", "Gyro");
DEFINE_bool(gyro_invert_y, false, "Invert vertical gyro aiming.", "Gyro");
