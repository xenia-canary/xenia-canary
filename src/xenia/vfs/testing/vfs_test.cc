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
#include "xenia/vfs/devices/host_path_entry.h"
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
  constexpr std::string_view hdd_mount = "\\Device\\Harddisk0\\Partition1";
  constexpr std::string_view content_mount =
      "\\Device\\Harddisk0\\Partition1\\Content";
  // The independently configured content root is a directory on the HDD,
  // rather than a device mounted at the Content path.
  const auto hdd_root = content_root / "hdd-partition";
  auto register_hdd = [&]() {
    auto hdd = std::make_unique<HostPathDevice>(hdd_mount, hdd_root, false);
    REQUIRE(hdd->Initialize());
    REQUIRE(hdd->AddDirectory("Content", content_root));
    vfs.RegisterDevice(std::move(hdd));
  };
  register_hdd();
  auto content_entry = vfs.ResolvePath(content_mount);
  REQUIRE(content_entry);
  REQUIRE(content_entry->device() == vfs.ResolvePath(hdd_mount)->device());
  REQUIRE(content_entry->parent() == vfs.ResolvePath(hdd_mount));
  File* directory = nullptr;
  FileAction directory_action;
  REQUIRE(vfs.OpenFile(nullptr, content_mount, FileDisposition::kOpen,
                       FileAccess::kGenericRead, true, false, &directory,
                       &directory_action) == X_STATUS_SUCCESS);
  std::unique_ptr<File> opened_directory(directory);
  auto raw_hdd = std::make_unique<NullDevice>(
      "\\Device\\Harddisk0", std::initializer_list<std::string>{"\\Cache0"});
  REQUIRE(raw_hdd->Initialize());
  vfs.RegisterDevice(std::move(raw_hdd));

  std::string mount_path = "\\Device\\Harddisk0\\Partition1\\Content";
  SECTION("Absolute HDD device path") {
    mount_path = "\\Device\\Harddisk0\\Partition1\\Content";
  }
  SECTION("Symbolic link path") {
    REQUIRE(vfs.RegisterSymbolicLink(
        "content:", "\\Device\\Harddisk0\\Partition1\\Content"));
    mount_path = "content:";
  }
  SECTION("Folders and packages created outside the VFS") {
    const auto external_dir =
        content_root / "0000000000000000" / "12345678" / "00000002";
    std::filesystem::create_directories(external_dir);
    std::ofstream(external_dir / "ExistingPackage") << "existing data";
    const auto guest_dir =
        mount_path + "\\0000000000000000\\12345678\\00000002";
    REQUIRE(vfs.ResolvePath(guest_dir));
    auto existing = vfs.ResolvePath(guest_dir + "\\existingpackage");
    REQUIRE(existing);
    REQUIRE(existing->size() == 13);
    REQUIRE(vfs.CreatePath(guest_dir + "\\existingpackage",
                           kFileAttributeNormal) == nullptr);
    REQUIRE(std::filesystem::file_size(external_dir / "ExistingPackage") == 13);
    REQUIRE(vfs.ResolvePath(guest_dir + "\\ExistingPackage") == existing);
  }
  SECTION("Content mount after clearing a failed launch") {
    opened_directory.reset();
    vfs.Clear();
    register_hdd();
    auto retry_hdd = std::make_unique<NullDevice>(
        "\\Device\\Harddisk0", std::initializer_list<std::string>{"\\Cache0"});
    REQUIRE(retry_hdd->Initialize());
    vfs.RegisterDevice(std::move(retry_hdd));
  }
  SECTION("Extracted game files remain separate from the writable HDD") {
    const auto game_root = content_root / "extracted-game";
    std::filesystem::create_directories(game_root);
    std::ofstream(game_root / "default.xex") << "game data";
    auto game =
        std::make_unique<HostPathDevice>("\\Device\\Cdrom0", game_root, true);
    REQUIRE(game->Initialize());
    vfs.RegisterDevice(std::move(game));
    REQUIRE(vfs.RegisterSymbolicLink("game:", "\\Device\\Cdrom0"));
    auto executable = vfs.ResolvePath("game:\\default.xex");
    REQUIRE(executable);
    REQUIRE(executable->is_read_only());
    File* game_file = nullptr;
    REQUIRE(executable->Open(FileAccess::kFileWriteData, &game_file) ==
            X_STATUS_ACCESS_DENIED);
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

  saved_package.close();
  auto package_entry = vfs.ResolvePath(package_path);
  REQUIRE(package_entry->device()->mount_path() == hdd_mount);
  REQUIRE(static_cast<HostPathEntry*>(package_entry)->host_path() ==
          host_package_path);
  auto package_dir = package_entry->parent();
  const auto renamed_path = package_dir->path() + "\\renamed-package";
  package_entry->Rename("hdd:\\" + renamed_path);
  REQUIRE(std::filesystem::exists(host_package_path.parent_path() /
                                  "renamed-package"));
  REQUIRE(vfs.ResolvePath(std::string(hdd_mount) + "\\" + renamed_path) ==
          package_entry);
  REQUIRE(vfs.DeletePath(std::string(hdd_mount) + "\\" + renamed_path));
  REQUIRE_FALSE(std::filesystem::exists(host_package_path.parent_path() /
                                        "renamed-package"));
  REQUIRE(vfs.CreatePath(std::string(hdd_mount) + "\\ContentCache.pkg",
                         kFileAttributeNormal));
  REQUIRE(std::filesystem::exists(hdd_root / "ContentCache.pkg"));

  REQUIRE(vfs.ResolvePath("\\Device\\Harddisk0\\Cache0"));
}

TEST_CASE("Nested devices match complete path components", "[vfs]") {
  VirtualFileSystem vfs;
  auto parent = std::make_unique<NullDevice>(
      "\\Device\\Harddisk0\\Partition1",
      std::initializer_list<std::string>{"\\ContentCache.pkg"});
  auto nested = std::make_unique<NullDevice>(
      "\\Device\\Harddisk0\\Partition1\\Content",
      std::initializer_list<std::string>{"\\Cache.pkg"});
  REQUIRE(parent->Initialize());
  REQUIRE(nested->Initialize());
  auto parent_device = parent.get();
  auto nested_device = nested.get();
  SECTION("Parent registered first") {
    vfs.RegisterDevice(std::move(parent));
    vfs.RegisterDevice(std::move(nested));
  }
  SECTION("Nested device registered first") {
    vfs.RegisterDevice(std::move(nested));
    vfs.RegisterDevice(std::move(parent));
  }
  auto sibling =
      vfs.ResolvePath("\\Device\\Harddisk0\\Partition1\\ContentCache.pkg");
  REQUIRE(sibling);
  REQUIRE(sibling->device() == parent_device);
  auto child =
      vfs.ResolvePath("\\Device\\Harddisk0\\Partition1\\Content\\Cache.pkg");
  REQUIRE(child);
  REQUIRE(child->device() == nested_device);
  auto root = vfs.ResolvePath("\\Device\\Harddisk0\\Partition1\\Content");
  REQUIRE(root);
  REQUIRE(root->device() == nested_device);
}

TEST_CASE("Host directories are enumerated lazily", "[vfs]") {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("xenia-lazy-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{root};
  std::filesystem::create_directories(root / "folder");
  std::ofstream(root / "folder" / "file") << "data";
  HostPathDevice device("\\Device\\Test", root, false);
  REQUIRE(device.Initialize());
  auto root_entry = device.ResolvePath("");
  REQUIRE(root_entry->Entry::children().empty());
  auto folder = device.ResolvePath("folder");
  REQUIRE(folder);
  REQUIRE(folder->Entry::children().empty());
  REQUIRE(folder->child_count() == 1);
  REQUIRE(root_entry->child_count() == 1);
  std::ofstream(root / "folder" / "new-file") << "new data";
  xe::filesystem::WildcardEngine wildcard;
  wildcard.SetRule("*");
  size_t index = 0;
  size_t files = 0;
  while (folder->IterateChildren(wildcard, &index)) {
    ++files;
  }
  REQUIRE(files == 2);
  REQUIRE(device.ResolvePath("folder\\file"));
  SECTION("Directory symlinks to ancestors do not recurse during enumeration") {
    std::error_code error;
    std::filesystem::create_directory_symlink(root, root / "folder" / "loop",
                                              error);
    if (error) {
      // Creating symlinks may require privileges on Windows.
      WARN("Host does not permit directory symlinks");
    } else {
      auto loop = device.ResolvePath("folder\\loop");
      REQUIRE(loop);
      REQUIRE(loop->children().empty());
    }
  }
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
