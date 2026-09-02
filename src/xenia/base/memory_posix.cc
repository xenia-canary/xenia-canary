/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_ANDROID
#include <dlfcn.h>
#include <linux/ashmem.h>
#include <string.h>
#include <sys/ioctl.h>

#include "xenia/base/main_android.h"
#endif

#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#if XE_PLATFORM_GNU_LINUX
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

DEFINE_bool(use_shm_open, false,
            "Back guest memory and the code cache with a /dev/shm file instead "
            "of memfd.\n"
            "Exposes both as named files that other processes can open to "
            "inspect guest memory or JIT output while a title runs.",
            "Linux");
#endif  // XE_PLATFORM_GNU_LINUX

namespace xe {
namespace memory {

#if XE_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (xe::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ =
          reinterpret_cast<decltype(android_ASharedMemory_create_)>(
              dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() { return getpagesize(); }
size_t allocation_granularity() { return page_size(); }

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

PageAccess ToXeniaProtectFlags(const char* protection) {
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == 'x') {
    return PageAccess::kExecuteReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == 'x') {
    return PageAccess::kExecuteReadOnly;
  }
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == '-') {
    return PageAccess::kReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == '-') {
    return PageAccess::kReadOnly;
  }
  return PageAccess::kNoAccess;
}

bool IsWritableExecutableMemorySupported() { return true; }

struct MappedFileRange {
  uintptr_t region_begin;
  uintptr_t region_end;
};

std::vector<MappedFileRange> mapped_file_ranges;
std::mutex g_mapped_file_ranges_mutex;

// Track shm file names for cleanup on exit
std::vector<std::string> g_shm_file_names;
std::mutex g_shm_file_names_mutex;
static bool g_cleanup_handlers_installed = false;

#if !XE_PLATFORM_ANDROID
static void CleanupAtExit() {
  for (const auto& name : g_shm_file_names) {
    shm_unlink(name.c_str());
  }
}

static void InstallCleanupHandlers() {
  if (g_cleanup_handlers_installed) {
    return;
  }
  g_cleanup_handlers_installed = true;

  std::atexit(CleanupAtExit);
  std::at_quick_exit(CleanupAtExit);
}
#endif  // !XE_PLATFORM_ANDROID

#if XE_PLATFORM_MAC
// macOS has no MAP_FIXED_NOREPLACE, and mmap(MAP_FIXED) silently unmaps whatever
// is already at the target address. Callers such as AllocateContext() and the
// code-cache/trampoline allocators depend on a fixed request FAILING when the
// range is occupied so they can advance to the next candidate address. Probe the
// range first and report a conflict, mirroring MAP_FIXED_NOREPLACE.
static bool MacFixedRangeIsFree(void* base_address, size_t length) {
  mach_vm_address_t region_addr = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;
  kern_return_t kr = mach_vm_region(
      mach_task_self(), &region_addr, &region_size, VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info), &count, &object_name);
  if (kr != KERN_SUCCESS) {
    // No region at or above the requested address; the range is free.
    return true;
  }
  mach_vm_address_t requested_start =
      reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_address_t requested_end = requested_start + length;
  mach_vm_address_t existing_end = region_addr + region_size;
  return !(region_addr < requested_end && requested_start < existing_end);
}
#endif  // XE_PLATFORM_MAC

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  // mmap does not support reserve / commit, so ignore allocation_type.
  uint32_t prot = ToPosixProtectFlags(access);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;

  if (base_address != nullptr) {
    if (allocation_type == AllocationType::kCommit) {
      if (Protect(base_address, length, access)) {
        return base_address;
      }
      return nullptr;
    }
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#elif XE_PLATFORM_MAC
    if (!MacFixedRangeIsFree(base_address, length)) {
      return nullptr;
    }
    flags |= MAP_FIXED;
#else
    flags |= MAP_FIXED;
#endif
  }
#ifdef __APPLE__
  if ((prot & PROT_EXEC) && !(flags & MAP_FIXED)) {
    flags |= MAP_JIT;
  }
#endif

  void* result = mmap(base_address, length, prot, flags, -1, 0);

  if (result != MAP_FAILED) {
    return result;
  }
  XELOGE("AllocFixed mmap({}, 0x{:X}, prot={:X}, flags={:X}) failed: {} ({})",
         base_address, length, prot, flags, strerror(errno), errno);
  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  const auto region_begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t region_end =
      reinterpret_cast<uintptr_t>(base_address) + length;

  std::lock_guard guard(g_mapped_file_ranges_mutex);
  for (const auto& mapped_range : mapped_file_ranges) {
    if (region_begin >= mapped_range.region_begin &&
        region_end <= mapped_range.region_end) {
      switch (deallocation_type) {
        case DeallocationType::kDecommit:
          return Protect(base_address, length, PageAccess::kNoAccess);
        case DeallocationType::kRelease:
          return false;
        default:
          assert_unhandled_case(deallocation_type);
      }
    }
  }

  switch (deallocation_type) {
    case DeallocationType::kDecommit:
      return Protect(base_address, length, PageAccess::kNoAccess);
    case DeallocationType::kRelease:
      return munmap(base_address, length) == 0;
    default:
      assert_unhandled_case(deallocation_type);
  }
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (out_old_access) {
    size_t length_copy = length;
    QueryProtect(base_address, length_copy, *out_old_access);
  }

  uint32_t prot = ToPosixProtectFlags(access);
  return mprotect(base_address, length, prot) == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if XE_PLATFORM_MAC
  mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_size_t size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;
  kern_return_t kr = mach_vm_region(
      mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info), &count, &object_name);
  if (kr != KERN_SUCCESS) {
    return false;
  }
  length = static_cast<size_t>(size);
  access_out = PageAccess::kNoAccess;
  bool r = (info.protection & VM_PROT_READ) != 0;
  bool w = (info.protection & VM_PROT_WRITE) != 0;
  bool x = (info.protection & VM_PROT_EXECUTE) != 0;
  if (r && w && x) {
    access_out = PageAccess::kExecuteReadWrite;
  } else if (r && w) {
    access_out = PageAccess::kReadWrite;
  } else if (x) {
    // Xenia has no execute-only enumerant; treat any executable mapping that
    // isn't writable as execute+read.
    access_out = PageAccess::kExecuteReadOnly;
  } else if (r) {
    access_out = PageAccess::kReadOnly;
  }
  return true;
#else
  // No generic POSIX solution exists. The Linux solution should work on all
  // Linux kernel based OS, including Android.
  std::ifstream memory_maps;
  memory_maps.open("/proc/self/maps", std::ios_base::in);
  std::string maps_entry_string;

  while (std::getline(memory_maps, maps_entry_string)) {
    std::stringstream entry_stream(maps_entry_string);
    uintptr_t map_region_begin, map_region_end;
    char separator;
    char protection[5];  // 4 chars (e.g., "r-xp") + null terminator

    entry_stream >> std::hex >> map_region_begin >> separator >>
        map_region_end >> protection;

    if (map_region_begin <= reinterpret_cast<uintptr_t>(base_address) &&
        map_region_end > reinterpret_cast<uintptr_t>(base_address)) {
      length = map_region_end - reinterpret_cast<uintptr_t>(base_address);

      access_out = ToXeniaProtectFlags(protection);

      // Look at the next consecutive mappings
      while (std::getline(memory_maps, maps_entry_string)) {
        std::stringstream next_entry_stream(maps_entry_string);
        uintptr_t next_map_region_begin, next_map_region_end;
        char next_protection[5];  // 4 chars (e.g., "r-xp") + null terminator

        next_entry_stream >> std::hex >> next_map_region_begin >> separator >>
            next_map_region_end >> next_protection;
        if (map_region_end == next_map_region_begin &&
            access_out == ToXeniaProtectFlags(next_protection)) {
          length =
              next_map_region_end - reinterpret_cast<uintptr_t>(base_address);
          continue;
        }
        break;
      }

      memory_maps.close();
      return true;
    }
  }

  memory_maps.close();
  return false;
#endif
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
#if XE_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? sharedmem_fd : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), xe::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return ashmem_fd;
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
  auto full_path = "/" / path;
#if XE_PLATFORM_GNU_LINUX
  // memfd is unaffected by noexec /dev/shm and needs no cleanup on exit.
  if (!cvars::use_shm_open) {
    const bool needs_exec = access == PageAccess::kExecuteReadOnly ||
                            access == PageAccess::kExecuteReadWrite;
    int memfd =
        memfd_create(path.c_str(), MFD_CLOEXEC | (needs_exec ? MFD_EXEC : 0u));
    if (memfd < 0 && needs_exec && errno == EINVAL) {
      memfd = memfd_create(path.c_str(), MFD_CLOEXEC);
    }
    if (memfd >= 0) {
      if (ftruncate(memfd, length) < 0) {
        XELOGE("ftruncate(memfd {}, 0x{:X}) failed: {} ({})", path.string(),
               length, strerror(errno), errno);
        close(memfd);
        return kFileMappingHandleInvalid;
      }
      return memfd;
    }
    XELOGW("memfd_create({}) failed: {} ({}), falling back to shm_open",
           path.string(), strerror(errno), errno);
  }
#endif  // XE_PLATFORM_GNU_LINUX

#ifdef __APPLE__
  std::string tmp_file = "/tmp/xe_map_" + path.filename().string();
  int ret = open(tmp_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (ret < 0) {
    XELOGE("open({}) failed: {} ({})", tmp_file, strerror(errno), errno);
    return kFileMappingHandleInvalid;
  }
  unlink(tmp_file.c_str());
  if (ftruncate(ret, length) < 0) {
    XELOGE("ftruncate({}, 0x{:X}) failed: {} ({})", tmp_file, length,
           strerror(errno), errno);
    close(ret);
    return kFileMappingHandleInvalid;
  }
  return ret;
#else
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  auto actual_path_str = full_path.string();
  if (ret < 0) {
    XELOGE("shm_open({}) failed: {} ({})", actual_path_str, strerror(errno),
           errno);
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, length) < 0) {
    XELOGE("ftruncate({}, 0x{:X}) failed: {} ({})", full_path.string(), length,
           strerror(errno), errno);
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  // Track for cleanup on abnormal exit and install cleanup handlers
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    g_shm_file_names.push_back(full_path.string());
  }
  InstallCleanupHandlers();
  return ret;
#endif
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  close(handle);
#if !XE_PLATFORM_ANDROID
  auto full_path = "/" / path;
  // Only shm_open handles are tracked and need unlinking.
  bool tracked = false;
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    auto it = std::ranges::find(g_shm_file_names, full_path.string());
    if (it != g_shm_file_names.end()) {
      g_shm_file_names.erase(it);
      tracked = true;
    }
  }
  if (tracked) {
    shm_unlink(full_path.c_str());
  }
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
  uint32_t prot = ToPosixProtectFlags(access);

#if XE_PLATFORM_MAC
  if (base_address != nullptr) {
    mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(base_address);
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(
        mach_task_self(), &address, &region_size,
        VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
        &count, &object_name);
    if (kr == KERN_SUCCESS) {
      mach_vm_address_t requested_start =
          reinterpret_cast<mach_vm_address_t>(base_address);
      mach_vm_address_t requested_end = requested_start + length;
      mach_vm_address_t existing_start = address;
      mach_vm_address_t existing_end = address + region_size;
      if (existing_start < requested_end && requested_start < existing_end) {
        XELOGW(
            "MapFileView: requested address [0x{:X}, 0x{:X}) overlaps "
            "existing VM region [0x{:X}, 0x{:X})",
            requested_start, requested_end, existing_start, existing_end);
        return nullptr;
      }
    }
  }
#endif

  int flags = MAP_SHARED;
  if (base_address != nullptr) {
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#else
    flags |= MAP_FIXED;
#endif
  }

  void* result = mmap(base_address, length, prot, flags, handle, file_offset);

  if (result != MAP_FAILED) {
    std::lock_guard guard(g_mapped_file_ranges_mutex);
    mapped_file_ranges.push_back(
        {reinterpret_cast<uintptr_t>(result),
         reinterpret_cast<uintptr_t>(result) + length});
    return result;
  }
  XELOGE("MapFileView mmap({}, 0x{:X}, prot={:X}, flags={:X}, handle={}, offset={:X}) failed: {} ({})",
         base_address, length, prot, flags, handle, file_offset, strerror(errno), errno);
  return nullptr;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  std::lock_guard guard(g_mapped_file_ranges_mutex);
  for (auto mapped_range = mapped_file_ranges.begin();
       mapped_range != mapped_file_ranges.end();) {
    if (mapped_range->region_begin ==
            reinterpret_cast<uintptr_t>(base_address) &&
        mapped_range->region_end ==
            reinterpret_cast<uintptr_t>(base_address) + length) {
      mapped_file_ranges.erase(mapped_range);
      return munmap(base_address, length) == 0;
    }
    ++mapped_range;
  }
  // TODO: Implement partial file unmapping.
  assert_always("Error: Partial unmapping of files not yet supported.");
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace xe
