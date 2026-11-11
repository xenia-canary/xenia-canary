/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XamInputRawState as title 45410914 calls it: r3 = user, r4 = flags,
// r5 = 0 (read) / 1 (send), r6 = an X_INPUT_RAW_STATE. The cases call the
// real export through its trampoline, so a missing export fails as "unknown
// export" and the register order is the one the guest uses.

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>

#include "third_party/catch/include/catch.hpp"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/testing/kernel_state_test.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_ordinals.h"

namespace xe {
namespace kernel {
namespace test {
namespace {

using hid::X_INPUT_CAPABILITIES;
using hid::X_INPUT_GAMEPAD;
using hid::X_INPUT_KEYSTROKE;
using hid::X_INPUT_RAW_STATE;
using hid::X_INPUT_STATE;
using hid::X_INPUT_VIBRATION;

constexpr uint32_t kNotConnected = X_ERROR_DEVICE_NOT_CONNECTED;  // 0x48F

// Implements only the InputDriver pure virtuals. GetRawState is deliberately
// NOT overridden: these cases go through InputDriver's default (GetState +
// BuildWiredRawReport), which every driver without its own override uses.
class FakeInputDriver final : public hid::InputDriver {
 public:
  FakeInputDriver() : InputDriver(nullptr, 0) {}

  void Connect(uint32_t user, uint8_t sub_type, const X_INPUT_GAMEPAD& pad) {
    Slot& slot = slots_[user];
    slot.connected = true;
    slot.sub_type = sub_type;
    slot.pad = pad;
    slot.packet = 0x1233;
    slot.changed = true;
  }
  void SetPad(uint32_t user, const X_INPUT_GAMEPAD& pad) {
    slots_[user].pad = pad;
    slots_[user].changed = true;
  }
  // GetState fails with `error` for this user (0 = no injected error).
  void SetError(uint32_t user, X_RESULT error) { slots_[user].error = error; }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  X_RESULT GetCapabilities(uint32_t user, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override {
    if (!IsConnected(user)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = hid::XINPUT_DEVTYPE_GAMEPAD;
    out_caps->sub_type = slots_[user].sub_type;
    return X_ERROR_SUCCESS;
  }
  // As the SDL driver does: a changed state takes the next packet number
  // when it is read.
  X_RESULT GetState(uint32_t user, X_INPUT_STATE* out_state) override {
    if (user < slots_.size() && slots_[user].error) {
      return slots_[user].error;
    }
    if (!IsConnected(user)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    Slot& slot = slots_[user];
    if (slot.changed) {
      ++slot.packet;
      slot.changed = false;
    }
    out_state->packet_number = slot.packet;
    out_state->gamepad = slot.pad;
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t user, X_INPUT_VIBRATION*) override {
    return IsConnected(user) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetKeystroke(uint32_t user, uint32_t, X_INPUT_KEYSTROKE*) override {
    return IsConnected(user) ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  hid::InputType GetInputType() const override {
    return hid::InputType::Controller;
  }

 private:
  struct Slot {
    bool connected = false;
    uint8_t sub_type = 0;
    X_INPUT_GAMEPAD pad = {};
    uint32_t packet = 0;
    bool changed = false;
    X_RESULT error = 0;
  };
  bool IsConnected(uint32_t user) const {
    return user < slots_.size() && slots_[user].connected;
  }
  std::array<Slot, 4> slots_;
};

// Constructed before KernelStateTest, which takes a pointer to it, and
// destroyed after it.
struct TestInputSystem {
  hid::InputSystem input_system{nullptr};
};

class XamInputRawStateTest : public TestInputSystem, public KernelStateTest {
 public:
  static constexpr uint32_t kPage = 0x90020000;
  static constexpr uint32_t kRaw = kPage;            // X_INPUT_RAW_STATE
  static constexpr uint32_t kState = kPage + 0x100;  // X_INPUT_STATE

  XamInputRawStateTest()
      : KernelStateTest(&input_system),
        // A real thread context: on Windows, PPCContext::TranslateVirtual
        // compares guest addresses with the context's own address, which
        // ThreadState places at ...E0000000.
        thread_(processor(), 0x100) {
    xam::XamModule::RegisterExportTable(processor()->export_resolver());
    // Two drivers, asked in this order, as InputSystem asks real ones.
    auto driver = std::make_unique<FakeInputDriver>();
    driver_ = driver.get();
    input_system.AddDriver(std::move(driver));
    auto second = std::make_unique<FakeInputDriver>();
    second_driver_ = second.get();
    input_system.AddDriver(std::move(second));
    REQUIRE(memory()->LookupHeap(kPage)->AllocFixed(
        kPage, 0x1000, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    memory()->Zero(kPage, 0x1000);
  }

  uint32_t Call(uint16_t ordinal, std::initializer_list<uint32_t> args) {
    cpu::ppc::PPCContext* context = thread_.context();
    size_t index = 3;
    for (uint32_t arg : args) {
      context->r[index++] = arg;
    }
    auto* entry =
        processor()->export_resolver()->GetExportByOrdinal("xam.xex", ordinal);
    REQUIRE(entry != nullptr);
    INFO("unknown export: xam.xex ordinal " << fmt::format("{:#x}", ordinal)
                                            << " " << entry->name);
    REQUIRE(entry->function_data.trampoline != nullptr);
    entry->function_data.trampoline(context);
    return uint32_t(context->r[3]);
  }

  // The title's read: r5 = 0, data_length = capacity 0x20 on the way in.
  uint32_t ReadRaw(uint32_t user) {
    raw()->data_length = 0x20;
    return Call(::ordinals::XamInputRawState, {user, 0, 0, kRaw});
  }
  uint32_t GetStatePacket(uint32_t user) {
    REQUIRE(Call(::ordinals::XamInputGetState, {user, 0, kState}) ==
            X_ERROR_SUCCESS);
    return memory()->TranslateVirtual<X_INPUT_STATE*>(kState)->packet_number;
  }

  X_INPUT_RAW_STATE* raw() {
    return memory()->TranslateVirtual<X_INPUT_RAW_STATE*>(kRaw);
  }
  std::array<uint8_t, 0x28> RawBytes() {
    std::array<uint8_t, 0x28> bytes;
    std::memcpy(bytes.data(), raw(), bytes.size());
    return bytes;
  }
  void FillRaw(uint8_t seed) {
    auto* bytes = reinterpret_cast<uint8_t*>(raw());
    for (size_t i = 0; i < sizeof(X_INPUT_RAW_STATE); ++i) {
      bytes[i] = uint8_t(seed + 7 * i);
    }
  }

  FakeInputDriver* driver_ = nullptr;
  FakeInputDriver* second_driver_ = nullptr;

 private:
  cpu::ThreadState thread_;
};

std::string Hex(const std::array<uint8_t, 0x28>& bytes) {
  std::string out;
  for (size_t i = 0; i < bytes.size(); ++i) {
    out += fmt::format("{}{:02X}", i ? " " : "", bytes[i]);
  }
  return out;
}

// The wired-style report: header 00 14 (so the title strips 2 bytes), 18
// payload bytes in wire order, data_length 0x14 at +0x20, the GetState packet
// number big-endian at +0x24.
std::array<uint8_t, 0x28> WiredReport(std::initializer_list<uint8_t> payload,
                                      uint32_t packet) {
  std::array<uint8_t, 0x28> bytes = {};
  bytes[0] = 0x00;
  bytes[1] = 0x14;
  size_t i = 2;
  for (uint8_t b : payload) {
    bytes[i++] = b;
  }
  bytes[0x20] = 0x14;
  bytes[0x24] = uint8_t(packet >> 24);
  bytes[0x25] = uint8_t(packet >> 16);
  bytes[0x26] = uint8_t(packet >> 8);
  bytes[0x27] = uint8_t(packet);
  return bytes;
}

// A keyboard instrument at rest except key 1 (C1, LT bit 7). LY 0xFFFF and
// RY 0x7F00 are the idle values the SDL driver reports for it.
X_INPUT_GAMEPAD KeyboardPad(uint8_t lt) {
  X_INPUT_GAMEPAD pad = {};
  pad.left_trigger = lt;
  pad.thumb_ly = int16_t(-1);
  pad.thumb_ry = int16_t(0x7F00);
  return pad;
}

}  // namespace

TEST_CASE_METHOD(XamInputRawStateTest, "RawStateReturnsKeyBitsForKeyboardSlot",
                 "[kernel][xam][input]") {
  driver_->Connect(1, hid::XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT,
                   KeyboardPad(0x80));
  FillRaw(0x11);

  REQUIRE(ReadRaw(1) == X_ERROR_SUCCESS);
  // payload: buttons 00 00, LT 80 (C1), RT 00, LX 0000, LY FFFF, RX 0000,
  // RY 7F00 (wire order 00 7F), bytes 12..17 zero.
  CHECK(Hex(RawBytes()) == Hex(WiredReport({0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
                                            0xFF, 0xFF, 0x00, 0x00, 0x00, 0x7F},
                                           0x1234)));
  CHECK((raw()->data[2 + 2] & 0x80) != 0);
  CHECK(raw()->data[1] != 1);  // not the 6-byte wireless header
}

TEST_CASE_METHOD(XamInputRawStateTest,
                 "RawStateConnectedNonKeyboardRawDeviceIsNotDisconnected",
                 "[kernel][xam][input]") {
  // The title reads raw reports from drum kits (subtype 8) too: 0x48F would
  // make it close the slot and drop that user's pad data.
  X_INPUT_GAMEPAD pad = {};
  pad.buttons = hid::X_INPUT_GAMEPAD_B;  // red pad
  pad.thumb_lx = int16_t(0x1234);
  driver_->Connect(2, hid::XINPUT_DEVSUBTYPE_DRUM_KIT, pad);
  FillRaw(0x22);

  const uint32_t result = ReadRaw(2);
  CHECK(result != kNotConnected);
  REQUIRE(result == X_ERROR_SUCCESS);
  CHECK(Hex(RawBytes()) ==
        Hex(WiredReport({0x00, 0x20, 0x00, 0x00, 0x34, 0x12}, 0x1234)));
}

TEST_CASE_METHOD(XamInputRawStateTest, "RawStatePacketNumberTracksGetState",
                 "[kernel][xam][input]") {
  driver_->Connect(1, hid::XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT,
                   KeyboardPad(0x80));

  // No change: two raw reads give the same report and GetState's number.
  const uint32_t first = GetStatePacket(1);
  REQUIRE(ReadRaw(1) == X_ERROR_SUCCESS);
  const auto report = RawBytes();
  CHECK(uint32_t(raw()->packet_number) == first);
  REQUIRE(ReadRaw(1) == X_ERROR_SUCCESS);
  CHECK(Hex(RawBytes()) == Hex(report));
  CHECK(GetStatePacket(1) == first);

  // LT changes, GetState read first: both numbers advance together, and
  // only now does the raw report change.
  driver_->SetPad(1, KeyboardPad(0x40));
  const uint32_t second = GetStatePacket(1);
  CHECK(second != first);
  REQUIRE(ReadRaw(1) == X_ERROR_SUCCESS);
  CHECK(uint32_t(raw()->packet_number) == second);
  CHECK(raw()->data[2 + 2] == 0x40);
  auto expected = report;
  expected[2 + 2] = 0x40;
  expected[0x24] = uint8_t(second >> 24);
  expected[0x25] = uint8_t(second >> 16);
  expected[0x26] = uint8_t(second >> 8);
  expected[0x27] = uint8_t(second);
  CHECK(Hex(RawBytes()) == Hex(expected));

  // LT changes, raw read first: same counter, so GetState agrees after.
  driver_->SetPad(1, KeyboardPad(0x00));
  REQUIRE(ReadRaw(1) == X_ERROR_SUCCESS);
  const uint32_t third = raw()->packet_number;
  CHECK(third != second);
  CHECK(raw()->data[2 + 2] == 0x00);
  CHECK(GetStatePacket(1) == third);
}

TEST_CASE_METHOD(XamInputRawStateTest, "RawStateSendAcceptsAndIgnores",
                 "[kernel][xam][input]") {
  driver_->Connect(1, hid::XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT,
                   KeyboardPad(0x80));
  FillRaw(0x33);
  raw()->data_length = 0x03;  // A send carries the payload length.
  const auto before = RawBytes();

  const uint32_t result = Call(::ordinals::XamInputRawState, {1, 0, 1, kRaw});
  // Not 0x3E5 (IO_PENDING): the title would leave the request pending.
  CHECK(result == X_ERROR_SUCCESS);
  CHECK(Hex(RawBytes()) == Hex(before));
}

TEST_CASE_METHOD(XamInputRawStateTest, "RawStateDisconnectedSlot",
                 "[kernel][xam][input]") {
  driver_->Connect(1, hid::XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT,
                   KeyboardPad(0x80));
  FillRaw(0x44);
  raw()->data_length = 0x20;
  const auto before = RawBytes();

  // The title treats 0x48F as "no controller" and closes the slot.
  CHECK(Call(::ordinals::XamInputRawState, {3, 0, 0, kRaw}) == kNotConnected);
  CHECK(Call(::ordinals::XamInputRawState, {0, 0, 0, kRaw}) == kNotConnected);
  CHECK(Hex(RawBytes()) == Hex(before));
}

TEST_CASE_METHOD(XamInputRawStateTest, "RawStateBadUserIndex",
                 "[kernel][xam][input]") {
  driver_->Connect(0, hid::XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT,
                   KeyboardPad(0x80));
  FillRaw(0x55);
  raw()->data_length = 0x20;
  const auto before = RawBytes();

  CHECK(Call(::ordinals::XamInputRawState, {4, 0, 0, kRaw}) == kNotConnected);
  CHECK(Call(::ordinals::XamInputRawState, {0xFF, 0, 0, kRaw}) ==
        kNotConnected);
  CHECK(Hex(RawBytes()) == Hex(before));
}

TEST_CASE_METHOD(XamInputRawStateTest, "RawStateKeepsDriverReadError",
                 "[kernel][xam][input]") {
  // A read error other than "not connected" must not become 0x48F: the title
  // keeps the slot (stale data) on other codes but closes it on 0x48F.
  constexpr X_RESULT kReadError = X_ERROR_FUNCTION_FAILED;
  FillRaw(0x66);
  raw()->data_length = 0x20;
  const auto before = RawBytes();

  SECTION("The only driver that has the user fails") {
    driver_->SetError(2, kReadError);
    // second_driver_ has nobody in slot 2 (0x48F).
    const uint32_t result = Call(::ordinals::XamInputRawState, {2, 0, 0, kRaw});
    CHECK(result != kNotConnected);
    CHECK(result == kReadError);
    CHECK(Hex(RawBytes()) == Hex(before));
  }
  SECTION("A driver after the failing one still answers") {
    driver_->SetError(2, kReadError);
    second_driver_->Connect(2, hid::XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT,
                            KeyboardPad(0x80));
    REQUIRE(ReadRaw(2) == X_ERROR_SUCCESS);
    CHECK(raw()->data[2 + 2] == 0x80);
  }
}

TEST_CASE_METHOD(XamInputRawStateTest, "RawStateShortCapacityIsRejected",
                 "[kernel][xam][input]") {
  driver_->Connect(1, hid::XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT,
                   KeyboardPad(0x80));

  // The report needs 0x14 bytes of data. A smaller capacity gets
  // X_ERROR_INSUFFICIENT_BUFFER (not 0x48F) and the buffer is not written.
  for (uint8_t capacity : {uint8_t(0x00), uint8_t(0x13)}) {
    INFO("capacity " << int(capacity));
    FillRaw(0x77);
    raw()->data_length = capacity;
    const auto before = RawBytes();
    CHECK(Call(::ordinals::XamInputRawState, {1, 0, 0, kRaw}) ==
          X_ERROR_INSUFFICIENT_BUFFER);
    CHECK(Hex(RawBytes()) == Hex(before));
  }

  // Exactly 0x14 fits; the title's 0x20 (ReadRaw) is unchanged.
  raw()->data_length = 0x14;
  REQUIRE(Call(::ordinals::XamInputRawState, {1, 0, 0, kRaw}) ==
          X_ERROR_SUCCESS);
  CHECK(raw()->data_length == 0x14);
  const uint32_t packet = raw()->packet_number;
  REQUIRE(ReadRaw(1) == X_ERROR_SUCCESS);
  CHECK(Hex(RawBytes()) == Hex(WiredReport({0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
                                            0xFF, 0xFF, 0x00, 0x00, 0x00, 0x7F},
                                           packet)));
}

}  // namespace test
}  // namespace kernel
}  // namespace xe
