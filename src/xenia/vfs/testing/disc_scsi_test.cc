/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#define CDB_INTERNAL_TYPES
#include "xenia/vfs/devices/disc_scsi.h"
#undef CDB_INTERNAL_TYPES

#include "third_party/catch/include/catch.hpp"
#include "xenia/vfs/devices/disc_omnidrive_device.h"

namespace xe::vfs::test {

TEST_CASE("BuildRead12Cdb packs fields and FUA bit", "[disc_scsi]") {
  const auto cdb = BuildRead12Cdb(0x12345678, 0x9ABCDEF0, true);

  REQUIRE(cdb.operation_code == 0xA8);
  REQUIRE(cdb.byte1 == static_cast<uint8_t>(1u << 3));
  REQUIRE(cdb.logical_block_address[0] == 0x12);
  REQUIRE(cdb.logical_block_address[1] == 0x34);
  REQUIRE(cdb.logical_block_address[2] == 0x56);
  REQUIRE(cdb.logical_block_address[3] == 0x78);
  REQUIRE(cdb.transfer_length[0] == 0x9A);
  REQUIRE(cdb.transfer_length[1] == 0xBC);
  REQUIRE(cdb.transfer_length[2] == 0xDE);
  REQUIRE(cdb.transfer_length[3] == 0xF0);
  REQUIRE(cdb.byte10 == 0);
  REQUIRE(cdb.control == 0);
}

TEST_CASE("BuildReadOmniDriveCdb packs fields and OmniDrive flags",
          "[disc_scsi]") {
  const auto cdb = BuildReadOmniDriveCdb(
      0x01020304, 0x05060708, OmniDriveDiscType::kDVD, true, true, true,
      OmniDriveSubchannel::kRaw96, true);

  REQUIRE(cdb.operation_code == 0xC0);
  REQUIRE(cdb.byte1 ==
          static_cast<uint8_t>(0x01 | (1u << 2) | (1u << 3) | (1u << 4)));
  REQUIRE(cdb.address[0] == 0x01);
  REQUIRE(cdb.address[1] == 0x02);
  REQUIRE(cdb.address[2] == 0x03);
  REQUIRE(cdb.address[3] == 0x04);
  REQUIRE(cdb.transfer_length[0] == 0x05);
  REQUIRE(cdb.transfer_length[1] == 0x06);
  REQUIRE(cdb.transfer_length[2] == 0x07);
  REQUIRE(cdb.transfer_length[3] == 0x08);
  REQUIRE(cdb.byte10 == static_cast<uint8_t>(0x02 | (1u << 2)));
  REQUIRE(cdb.control == 0);
}

}  // namespace xe::vfs::test
