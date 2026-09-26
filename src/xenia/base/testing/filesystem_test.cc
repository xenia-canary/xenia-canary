/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/platform.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

using xe::filesystem::FileInfo;

namespace {

// A directory of our own under the system temp directory, removed with the
// test.
class ScratchDir {
 public:
  ScratchDir() {
    std::error_code ec;
    path_ = std::filesystem::temp_directory_path(ec) /
            "xenia_filesystem_test_scratch";
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }
  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

const FileInfo* Find(const std::vector<FileInfo>& infos,
                     const std::string& name) {
  for (const auto& info : infos) {
    if (info.name == name) {
      return &info;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("List Files", "[filesystem]") {
  ScratchDir scratch;
  REQUIRE(std::filesystem::exists(scratch.path()));

  std::error_code ec;
  std::filesystem::create_directory(scratch.path() / "a_directory", ec);
  REQUIRE(!ec);
  {
    std::ofstream file(scratch.path() / "a_file");
    file << "1234";
  }

  SECTION("a directory and a file are told apart") {
    auto infos = xe::filesystem::ListFiles(scratch.path());
    const auto* dir = Find(infos, "a_directory");
    const auto* file = Find(infos, "a_file");
    REQUIRE(dir);
    REQUIRE(file);
    REQUIRE(dir->type == FileInfo::Type::kDirectory);
    REQUIRE(dir->total_size == 0);
    REQUIRE(file->type == FileInfo::Type::kFile);
    REQUIRE(file->total_size == 4);
  }

#if !XE_PLATFORM_WIN32
  SECTION("a symlinked directory is a directory") {
    // dirent::d_type reports DT_LNK here, so classifying from it made a
    // symlinked directory look like a file.
    std::filesystem::create_directory_symlink(
        scratch.path() / "a_directory", scratch.path() / "linked_directory",
        ec);
    REQUIRE(!ec);
    auto infos = xe::filesystem::ListFiles(scratch.path());
    const auto* link = Find(infos, "linked_directory");
    REQUIRE(link);
    REQUIRE(link->type == FileInfo::Type::kDirectory);
  }

  SECTION("a dangling symlink reports the link, not a garbage size") {
    // stat fails on it, and its return used to be discarded, leaving the
    // struct indeterminate and feeding the size and the timestamps from it.
    // The lstat fallback describes the link itself, whose size is the length
    // of the path it holds.
    const auto target = scratch.path() / "not_here";
    std::filesystem::create_symlink(target, scratch.path() / "dangling", ec);
    REQUIRE(!ec);
    auto infos = xe::filesystem::ListFiles(scratch.path());
    const auto* dangling = Find(infos, "dangling");
    REQUIRE(dangling);
    REQUIRE(dangling->type == FileInfo::Type::kFile);
    REQUIRE(dangling->total_size == target.string().size());
  }
#endif  // !XE_PLATFORM_WIN32
}

}  // namespace xe::base::test
