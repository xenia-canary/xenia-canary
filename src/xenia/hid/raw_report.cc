/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/raw_report.h"

#include <cstring>

namespace xe {
namespace hid {

namespace {
void PutWire16(uint8_t* at, uint16_t value) {
  at[0] = static_cast<uint8_t>(value & 0xFF);
  at[1] = static_cast<uint8_t>(value >> 8);
}
}  // namespace

void BuildWiredRawReport(const X_INPUT_STATE& in, X_INPUT_RAW_STATE* out) {
  std::memset(out, 0, sizeof(*out));
  constexpr uint8_t kPayloadLength = kWiredRawReportLength;
  out->data[0] = 0x00;  // header type: wired
  out->data[1] = kPayloadLength;
  uint8_t* payload = out->data + 2;
  const X_INPUT_GAMEPAD& pad = in.gamepad;
  PutWire16(payload + 0, pad.buttons);
  payload[2] = pad.left_trigger;
  payload[3] = pad.right_trigger;
  PutWire16(payload + 4, static_cast<uint16_t>(int16_t(pad.thumb_lx)));
  PutWire16(payload + 6, static_cast<uint16_t>(int16_t(pad.thumb_ly)));
  PutWire16(payload + 8, static_cast<uint16_t>(int16_t(pad.thumb_rx)));
  PutWire16(payload + 10, static_cast<uint16_t>(int16_t(pad.thumb_ry)));
  // payload[12..17] stay zero (see the header).
  out->data_length = kPayloadLength;
  out->packet_number = uint32_t(in.packet_number);
}

}  // namespace hid
}  // namespace xe
