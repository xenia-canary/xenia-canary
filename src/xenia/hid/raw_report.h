/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_RAW_REPORT_H_
#define XENIA_HID_RAW_REPORT_H_

#include "xenia/hid/input.h"

namespace xe {
namespace hid {

// Builds the report XamInputRawState returns from an X_INPUT_STATE a driver's
// GetState returned (before InputSystem applies any deadzone), with its
// packet number. A driver whose raw and normal reads share one packet counter
// and state snapshot, as the default InputDriver::GetRawState does, so changes
// the report only together with a packet number XamInputGetState also
// returns; two separate calls can still see different updates, each under its
// own number. Title 45410914 skips its raw read for a user while
// XamInputGetState's packet number is unchanged (its poll loop at 0x8252A13C,
// title update 5), so a report that changed on its own would go unseen.
//
// The report is wired-style: header 00 14, then the 18-byte XUSB payload:
// buttons, LT, RT, LX, LY, RX, RY in wire (little-endian) order, then six
// zero bytes. data_length = 0x14, the rest of `data` zero, packet_number =
// in.packet_number. The title copies the payload byte by byte, so its code
// does not show the byte order. The USB wire order is used; with it a
// keyboard instrument's keys work in that title, and so does its overdrive
// button once SDL delivers it (see SDLInputDriver::RefreshKeyboardAxes).
//
// TODO(stone-ericm): Payload bytes 12-17 are not in X_INPUT_STATE, so they
// are always zero. On a keyboard instrument they hold the touch strip (12)
// and the pedal-connected flag (17, bit 0), per PlasticBand's notes (see
// sdl/sdl_keyboard_report.h). Byte 16 bit 7 must stay clear: the title
// treats a report with it set as a command (0x8252A000).
void BuildWiredRawReport(const X_INPUT_STATE& in, X_INPUT_RAW_STATE* out);

// The report's data_length: header (2) + payload (18) bytes.
constexpr uint8_t kWiredRawReportLength = 0x14;

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_RAW_REPORT_H_
