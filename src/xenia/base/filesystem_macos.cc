#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/string.h"

#ifdef __APPLE__
#include <dirent.h>

#include <errno.h>
#include <fcntl.h>

#include <ftw.h>
#include <pwd.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <filesystem>
#include <string>

// macOS specific includes
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/syslimits.h>

namespace xe {

// Common path conversion functions that macOS needs
std::string path_to_utf8(const std::filesystem::path& path) {
  return path.string();
}

std::u16string path_to_utf16(const std::filesystem::path& path) {
  return xe::to_utf16(path.string());
}

std::filesystem::path to_path(const std::string_view source) { return source; }

std::filesystem::path to_path(const std::u16string_view source) {
  return xe::to_utf8(source);
}

namespace filesystem {

// macOS-specific implementations

namespace {

using DirHandle = std::unique_ptr<DIR, int (*)(DIR*)>;

bool StatsReferToSameObject(const struct stat& before,
                            const struct stat& after) {
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         (before.st_mode & S_IFMT) == (after.st_mode & S_IFMT);
}

DirHandle OpenParentDirectory(const std::filesystem::path& path,
                              std::filesystem::path* leaf_name) {
  *leaf_name = path.filename();
  if (leaf_name->empty()) {
    errno = EINVAL;
    return DirHandle(nullptr, &closedir);
  }

  auto parent_path = path.parent_path();
  if (parent_path.empty()) {
    parent_path = ".";
  }

  return DirHandle(opendir(parent_path.c_str()), &closedir);
}

int OpenVerifiedExistingPath(int parent_fd,
                             const std::filesystem::path& leaf_name,
                             int open_flags) {
  struct stat st_before;
  if (fstatat(parent_fd, leaf_name.c_str(), &st_before, AT_SYMLINK_NOFOLLOW) !=
      0) {
    return -1;
  }
  if (S_ISLNK(st_before.st_mode)) {
    errno = ELOOP;
    return -1;
  }

  int handle = openat(parent_fd, leaf_name.c_str(), open_flags);
  if (handle == -1) {
    return -1;
  }

  struct stat st_after;
  if (fstat(handle, &st_after) != 0) {
    close(handle);
    return -1;
  }

  if (!StatsReferToSameObject(st_before, st_after)) {
    close(handle);
    errno = EIO;
    return -1;
  }

  return handle;
}

int OpenVerifiedPath(const std::filesystem::path& path, int open_flags,
                     mode_t create_mode, bool truncate_existing) {
  const bool allow_create = (open_flags & O_CREAT) != 0;
  const int existing_open_flags = open_flags & ~(O_CREAT | O_TRUNC);
  std::filesystem::path leaf_name;
  DirHandle parent_dir = OpenParentDirectory(path, &leaf_name);
  if (!parent_dir) {
    return -1;
  }
  int parent_fd = dirfd(parent_dir.get());
  if (parent_fd == -1) {
    return -1;
  }

  int handle =
      OpenVerifiedExistingPath(parent_fd, leaf_name, existing_open_flags);
  if (handle == -1) {
    const int open_error = errno;
    if (!allow_create || open_error != ENOENT) {
      return -1;
    }

    handle = openat(parent_fd, leaf_name.c_str(),
                    existing_open_flags | O_CREAT | O_EXCL, create_mode);
    if (handle == -1) {
      if (errno != EEXIST) {
        return -1;
      }
      handle =
          OpenVerifiedExistingPath(parent_fd, leaf_name, existing_open_flags);
      if (handle == -1) {
        return -1;
      }
    } else {
      struct stat st_after_create;
      struct stat st_path;
      if (fstat(handle, &st_after_create) != 0 ||
          fstatat(parent_fd, leaf_name.c_str(), &st_path,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
          !StatsReferToSameObject(st_path, st_after_create)) {
        close(handle);
        errno = EIO;
        return -1;
      }
    }
  }

  if (truncate_existing && ftruncate(handle, 0) != 0) {
    close(handle);
    return -1;
  }

  return handle;
}

}  // namespace

std::filesystem::path GetExecutablePath() {
  char path[PATH_MAX];
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == 0) {
    std::error_code ec;
    auto rp = std::filesystem::canonical(std::filesystem::path(path), ec);
    if (!ec) {
      return rp;
    }
  }
  return "";
}

std::filesystem::path GetExecutableFolder() {
  return GetExecutablePath().parent_path();
}

std::filesystem::path GetUserFolder() {
  // get preferred data home
  char* home = std::getenv("XDG_DATA_HOME");
  if (home) {
    return std::string(home);
  }
  home = std::getenv("HOME");

  // Fallback if HOME not set
  if (home == NULL) {
    struct passwd pw1;
    struct passwd* pw;
    char buf[4096];
    getpwuid_r(getuid(), &pw1, buf, sizeof(buf), &pw);
    assert(&pw1 == pw);
    home = pw->pw_dir;
  }

  return std::filesystem::path(home) / ".local" / "share";
}

FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
  std::string mode_str(mode);
  int flags = 0;
  bool truncate_existing = false;
  bool plus = mode_str.find('+') != std::string::npos;
  if (mode_str.find('r') != std::string::npos) {
    flags = plus ? O_RDWR : O_RDONLY;
  }
  if (mode_str.find('w') != std::string::npos) {
    flags = plus ? O_RDWR | O_CREAT : O_WRONLY | O_CREAT;
    truncate_existing = true;
  }
  if (mode_str.find('a') != std::string::npos) {
    flags = plus ? O_RDWR | O_CREAT | O_APPEND : O_WRONLY | O_CREAT | O_APPEND;
  }
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif

  int fd = OpenVerifiedPath(path, flags, 0666, truncate_existing);
  if (fd == -1) {
    return nullptr;
  }

  FILE* file = fdopen(fd, mode_str.c_str());
  if (!file) {
    close(fd);
    return nullptr;
  }
  return file;
}

bool Seek(FILE* file, int64_t offset, int origin) {
  return fseeko(file, offset, origin) == 0;
}

int64_t Tell(FILE* file) { return int64_t(ftello(file)); }

bool TruncateStdioFile(FILE* file, uint64_t length) {
  if (fflush(file)) {
    return false;
  }
  int64_t position = Tell(file);
  if (position < 0) {
    return false;
  }
  if (ftruncate(fileno(file), length)) {
    return false;
  }
  if (uint64_t(position) > length) {
    if (!Seek(file, 0, SEEK_END)) {
      return false;
    }
  }
  return true;
}

static uint64_t convertUnixtimeToWinFiletime(time_t unixtime) {
  // Linux uses number of seconds since 1/1/1970, and Windows uses
  // number of nanoseconds since 1/1/1601
  // so we convert linux time to nanoseconds and then add the number of
  // nanoseconds from 1601 to 1970
  // see https://msdn.microsoft.com/en-us/library/ms724228
  uint64_t filetime = (unixtime * 10000000) + 116444736000000000;
  return filetime;
}

bool CreateEmptyFile(const std::filesystem::path& path) {
  int flags = O_WRONLY | O_CREAT;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif

  int file = OpenVerifiedPath(path, flags, 0774, true);
  if (file >= 0) {
    close(file);
    return true;
  }
  return false;
}

class PosixFileHandle : public FileHandle {
 public:
  PosixFileHandle(std::filesystem::path path, int handle)
      : FileHandle(std::move(path)), handle_(handle) {}
  ~PosixFileHandle() override {
    close(handle_);
    handle_ = -1;
  }
  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    ssize_t out = pread(handle_, buffer, buffer_length, file_offset);
    *out_bytes_read = out;
    return out >= 0 ? true : false;
  }
  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,
             size_t* out_bytes_written) override {
    ssize_t out = pwrite(handle_, buffer, buffer_length, file_offset);
    *out_bytes_written = out;
    return out >= 0 ? true : false;
  }
  bool SetLength(size_t length) override {
    return ftruncate(handle_, length) >= 0 ? true : false;
  }
  void Flush() override { fsync(handle_); }

 private:
  int handle_ = -1;
};

std::unique_ptr<FileHandle> FileHandle::OpenExisting(
    const std::filesystem::path& path, uint32_t desired_access) {
  // Map desired_access to open(2) flags and verify the opened inode still
  // matches the path we checked before use.
  int open_flags = 0;
  if (desired_access & FileAccess::kGenericAll) {
    open_flags = O_RDWR;
  } else if (desired_access &
             (FileAccess::kGenericWrite | FileAccess::kFileWriteData)) {
    open_flags = O_RDWR;
  } else {
    open_flags = O_RDONLY;
  }
  if (desired_access & FileAccess::kFileAppendData) {
    open_flags |= O_APPEND;
  }
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  open_flags |= O_NOFOLLOW;
#endif

  std::filesystem::path leaf_name;
  DirHandle parent_dir = OpenParentDirectory(path, &leaf_name);
  if (!parent_dir) {
    return nullptr;
  }
  int parent_fd = dirfd(parent_dir.get());
  if (parent_fd == -1) {
    return nullptr;
  }

  int handle = OpenVerifiedExistingPath(parent_fd, leaf_name, open_flags);
  if (handle == -1) {
    return nullptr;
  }
  return std::make_unique<PosixFileHandle>(path, handle);
}

std::optional<FileInfo> GetInfo(const std::filesystem::path& path) {
  FileInfo info{};
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      info.type = FileInfo::Type::kDirectory;
      info.total_size = 0;
    } else {
      info.type = FileInfo::Type::kFile;
      info.total_size = st.st_size;
    }
    info.path = path.parent_path();
    info.name = path.filename();
    info.create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    info.access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    info.write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    return std::move(info);
  }
  return {};
}

std::vector<FileInfo> ListFiles(const std::filesystem::path& path) {
  std::vector<FileInfo> result;

  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return result;
  }

  while (auto ent = readdir(dir)) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    FileInfo info;

    info.name = ent->d_name;
    struct stat st;
    stat((path / info.name).c_str(), &st);
    info.create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    info.access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    info.write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    info.path = path;
    if (ent->d_type == DT_DIR) {
      info.type = FileInfo::Type::kDirectory;
      info.total_size = 0;
    } else {
      info.type = FileInfo::Type::kFile;
      info.total_size = st.st_size;
    }
    result.push_back(info);
  }
  closedir(dir);
  return std::move(result);
}

// This does not need to be set on macOS, so you can return this as false
// Since this is Windows/Linux exclusive code that's needed
bool SetAttributes(const std::filesystem::path& path, uint64_t attributes) {
  return false;
}

bool DeleteFile(const std::filesystem::path& path) {
  return unlink(path.c_str()) == 0;
}

static int removeCallback(const char* fpath, const struct stat* sb,
                          int typeflag, struct FTW* ftwbuf) {
  int rv = remove(fpath);
  return rv;
}

bool DeleteFolder(const std::filesystem::path& path) {
  return nftw(path.c_str(), removeCallback, 64, FTW_DEPTH | FTW_PHYS) == 0;
}

bool IsFolder(const std::filesystem::path& path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  return false;
}

bool CopyFile(const std::filesystem::path& src,
              const std::filesystem::path& dest, bool overwrite) {
  std::error_code ec;
  if (!overwrite) {
    if (std::filesystem::exists(dest, ec)) {
      if (!ec) return false;
      // if exists check failed with an error, fallthrough to attempt copy
    }
    bool ok = std::filesystem::copy_file(
        src, dest, std::filesystem::copy_options::none, ec);
    return ok && !ec;
  }
  bool ok = std::filesystem::copy_file(
      src, dest, std::filesystem::copy_options::overwrite_existing, ec);
  return ok && !ec;
}

bool MoveFile(const std::filesystem::path& src,
              const std::filesystem::path& dest) {
  return rename(src.c_str(), dest.c_str()) == 0;
}

bool FileExists(const std::filesystem::path& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

uint64_t GetFileSize(const std::filesystem::path& path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    return st.st_size;
  }
  return 0;
}

std::filesystem::path GetTempFolder() {
  const char* tmpdir = std::getenv("TMPDIR");
  if (tmpdir) {
    return std::string(tmpdir);
  }

  tmpdir = std::getenv("TEMP");
  if (tmpdir) {
    return std::string(tmpdir);
  }

  tmpdir = std::getenv("TMP");
  if (tmpdir) {
    return std::string(tmpdir);
  }

  return "/tmp";
}

bool IsSymbolicLink(const std::filesystem::path& path) {
  struct stat st;
  if (lstat(path.c_str(), &st) == 0) {
    return S_ISLNK(st.st_mode);
  }
  return false;
}

std::filesystem::path ResolvePath(const std::filesystem::path& path) {
  std::error_code ec;
  auto rp = std::filesystem::canonical(path, ec);
  if (!ec) {
    return rp;
  }
  return path;
}

}  // namespace filesystem
}  // namespace xe

#endif
