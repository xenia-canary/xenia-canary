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
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif
#include <cstddef>

#include <fstream>
#include <mutex>
#include <sstream>
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

bool IsWritableExecutableMemorySupported() {
#if XE_PLATFORM_MAC
  static const bool supported = []() {
    const size_t test_size = page_size();
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    flags |= MAP_JIT;
#endif
    void* test_mapping = mmap(nullptr, test_size,
                              PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    if (test_mapping == MAP_FAILED) {
      const int err = errno;
      XELOGE("MAP_JIT test failed size=0x{:X} err={} ({})", test_size, err,
             std::strerror(err));
      return false;
    }
    munmap(test_mapping, test_size);
    return true;
  }();
  return supported;
#else
  return true;
#endif
}

struct MappedFileRange {
  uintptr_t region_begin;
  uintptr_t region_end;
};

std::vector<MappedFileRange> mapped_file_ranges;
std::mutex g_mapped_file_ranges_mutex;

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  // mmap does not support reserve / commit, so ignore allocation_type.
  uint32_t prot = ToPosixProtectFlags(access);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#if XE_PLATFORM_MAC
  if (access == PageAccess::kExecuteReadWrite ||
      access == PageAccess::kExecuteReadOnly) {
#ifdef MAP_JIT
    flags |= MAP_JIT;
#endif
  }
#endif

  if (base_address != nullptr) {
    if (allocation_type == AllocationType::kCommit) {
#if XE_PLATFORM_MAC
      const size_t system_page_size = page_size();
      uintptr_t start = reinterpret_cast<uintptr_t>(base_address);
      uintptr_t aligned_start = start & ~(system_page_size - 1);
      uintptr_t aligned_end = xe::align(start + length, system_page_size);
      size_t aligned_length =
          aligned_end > aligned_start ? aligned_end - aligned_start : 0;
      if (!aligned_length) {
        return base_address;
      }
      return mprotect(reinterpret_cast<void*>(aligned_start), aligned_length,
                      prot) == 0
                 ? base_address
                 : nullptr;
#else
      if (Protect(base_address, length, access)) {
        return base_address;
      }
      return nullptr;
#endif
    }
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
  }

  void* result = mmap(base_address, length, prot, flags, -1, 0);

  if (result != MAP_FAILED) {
    if (base_address && result != base_address) {
      munmap(result, length);
      return nullptr;
    }
    return result;
  }
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
          assert_always("Error: Tried to release mapped memory!");
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
  if (mprotect(base_address, length, prot) != 0) {
    const int err = errno;
    XELOGE("mprotect failed addr=0x{:X} len=0x{:X} prot=0x{:X} err={} ({})",
           reinterpret_cast<uintptr_t>(base_address), length, prot, err,
           std::strerror(err));
    return false;
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if XE_PLATFORM_MAC
  access_out = PageAccess::kNoAccess;

  mach_vm_address_t address =
      static_cast<mach_vm_address_t>(reinterpret_cast<uintptr_t>(base_address));
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;

  kern_return_t kr = mach_vm_region(
      mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);

  if (object_name != MACH_PORT_NULL) {
    mach_port_deallocate(mach_task_self(), object_name);
  }

  if (kr != KERN_SUCCESS) {
    return false;
  }

  length = static_cast<size_t>(region_size);

  const vm_prot_t prot = info.protection;
  const bool can_read = (prot & VM_PROT_READ) != 0;
  const bool can_write = (prot & VM_PROT_WRITE) != 0;
  const bool can_execute = (prot & VM_PROT_EXECUTE) != 0;

  if (can_write) {
    access_out =
        can_execute ? PageAccess::kExecuteReadWrite : PageAccess::kReadWrite;
  } else if (can_read) {
    access_out =
        can_execute ? PageAccess::kExecuteReadOnly : PageAccess::kReadOnly;
  } else if (can_execute) {
    access_out = PageAccess::kExecuteReadOnly;
  } else {
    access_out = PageAccess::kNoAccess;
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
    char separator, protection[4];

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
        char next_protection[4];

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
  std::string shm_name = path.filename().string();
#if XE_PLATFORM_MAC
  constexpr size_t kMacShmNameLimit = 30;
  if (shm_name.size() > kMacShmNameLimit) {
    size_t hash = std::hash<std::string>{}(shm_name);
    char hash_name[32] = {};
    std::snprintf(hash_name, sizeof(hash_name), "xe_%016zx", hash);
    shm_name = hash_name;
  }
#endif  // XE_PLATFORM_MAC
  auto full_path = "/" + shm_name;
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    XELOGE("shm_open failed for {}: {} ({})", full_path, errno,
           std::strerror(errno));
    return kFileMappingHandleInvalid;
  }
#ifdef __APPLE__
  if (ftruncate(ret, length) != 0) {
    int err = errno;
    close(ret);
    errno = err;
    return kFileMappingHandleInvalid;
  }
#else
  if (ftruncate64(ret, length) != 0) {
    int err = errno;
    close(ret);
    errno = err;
    return kFileMappingHandleInvalid;
  }
#endif
  return ret;
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  close(handle);
#if !XE_PLATFORM_ANDROID
  std::string shm_name = path.filename().string();
#if XE_PLATFORM_MAC
  constexpr size_t kMacShmNameLimit = 30;
  if (shm_name.size() > kMacShmNameLimit) {
    size_t hash = std::hash<std::string>{}(shm_name);
    char hash_name[32] = {};
    std::snprintf(hash_name, sizeof(hash_name), "xe_%016zx", hash);
    shm_name = hash_name;
  }
#endif  // XE_PLATFORM_MAC
  auto full_path = "/" + shm_name;
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
  uint32_t prot = ToPosixProtectFlags(access);

  int flags = MAP_SHARED;
  if (base_address != nullptr) {
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
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

  return nullptr;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
#if XE_PLATFORM_MAC
  if (munmap(base_address, length) != 0) {
    return false;
  }
  const auto region_begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t region_end = region_begin + length;
  std::lock_guard guard(g_mapped_file_ranges_mutex);
  for (auto it = mapped_file_ranges.begin(); it != mapped_file_ranges.end();
       ++it) {
    if (region_begin >= it->region_begin && region_end <= it->region_end) {
      if (region_begin == it->region_begin && region_end == it->region_end) {
        mapped_file_ranges.erase(it);
      } else if (region_begin == it->region_begin) {
        it->region_begin = region_end;
      } else if (region_end == it->region_end) {
        it->region_end = region_begin;
      } else {
        MappedFileRange tail{region_end, it->region_end};
        it->region_end = region_begin;
        mapped_file_ranges.insert(it + 1, tail);
      }
      break;
    }
  }
  return true;
#else
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
#endif
}

}  // namespace memory
}  // namespace xe
