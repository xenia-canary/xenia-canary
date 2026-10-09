/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/virtual_file_system.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::vfs::test {

TEST_CASE("Create content below a mounted device", "[vfs]") {
  const auto content_root =
      std::filesystem::temp_directory_path() /
      ("xenia-content-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{content_root};

  VirtualFileSystem vfs;
  auto content = std::make_unique<HostPathDevice>(
      "\\Device\\Harddisk0\\Partition1\\Content", content_root, false);
  REQUIRE(content->Initialize());
  vfs.RegisterDevice(std::move(content));
  auto raw_hdd = std::make_unique<NullDevice>(
      "\\Device\\Harddisk0", std::initializer_list<std::string>{"\\Cache0"});
  REQUIRE(raw_hdd->Initialize());
  vfs.RegisterDevice(std::move(raw_hdd));

  std::string mount_path;
  SECTION("Absolute HDD device path") {
    mount_path = "\\Device\\Harddisk0\\Partition1\\Content";
  }
  SECTION("Symbolic link path") {
    REQUIRE(vfs.RegisterSymbolicLink(
        "content:", "\\Device\\Harddisk0\\Partition1\\Content"));
    mount_path = "content:";
  }
  const std::string package_path =
      mount_path + "\\0000000000000000\\12345678\\00000002\\test-package";
  REQUIRE(vfs.CreatePath(package_path, kFileAttributeNormal));
  const auto host_package_path = content_root / "0000000000000000" /
                                 "12345678" / "00000002" / "test-package";
  REQUIRE(std::filesystem::is_regular_file(host_package_path));
  REQUIRE(vfs.ResolvePath(package_path));

  File* file = nullptr;
  FileAction action;
  REQUIRE(vfs.OpenFile(nullptr, package_path, FileDisposition::kOpen,
                       FileAccess::kGenericWrite, false, true, &file,
                       &action) == X_STATUS_SUCCESS);
  std::unique_ptr<File> opened_file(file);
  const std::array<uint8_t, 4> bytes = {'P', 'I', 'R', 'S'};
  size_t bytes_written = 0;
  REQUIRE(opened_file->WriteSync(bytes, 0, &bytes_written) == X_STATUS_SUCCESS);
  REQUIRE(bytes_written == bytes.size());
  opened_file.reset();
  std::ifstream saved_package(host_package_path, std::ios::binary);
  std::array<char, 4> saved_bytes{};
  saved_package.read(saved_bytes.data(), saved_bytes.size());
  REQUIRE(std::string(saved_bytes.data(), saved_bytes.size()) == "PIRS");

  REQUIRE(vfs.ResolvePath("\\Device\\Harddisk0\\Cache0"));
}

TEST_CASE("STFS Decode date and time", "[stfs_decode]") {
  SECTION("10 June 2022 19:46:00 UTC - Decode") {
    const uint16_t date = 0x54CA;
    const uint16_t time = 0x9DBD;
    const uint64_t result = 132993639580000000;

    const uint64_t timestamp = decode_fat_timestamp(date, time);

    REQUIRE(timestamp == result);
  }
}

TEST_CASE("Host path entry found by its new name after a rename",
          "[vfs_rename]") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-rename-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "item.tmp") << "data";

  {
    HostPathDevice device("\\Device\\Test", dir, false);
    REQUIRE(device.Initialize());
    Entry* entry = device.ResolvePath("item.tmp");
    REQUIRE(entry != nullptr);

    entry->Rename("cache:\\item.ipk");

    REQUIRE(entry->name() == "item.ipk");
    REQUIRE(device.ResolvePath("item.ipk") == entry);
    REQUIRE(device.ResolvePath("item.tmp") == nullptr);
    REQUIRE(std::filesystem::exists(dir / "item.ipk"));
  }

  std::filesystem::remove_all(dir);
}

}  // namespace xe::vfs::test
