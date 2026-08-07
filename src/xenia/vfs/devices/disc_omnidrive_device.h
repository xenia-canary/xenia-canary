/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_DISC_OMNIDRIVE_DEVICE_H_
#define XENIA_VFS_DEVICES_DISC_OMNIDRIVE_DEVICE_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif  // XE_PLATFORM_WIN32
#include "xenia/vfs/optical_drive_device.h"

namespace xe {
namespace vfs {

class DiscDriveEntry;

typedef struct {
  uint8_t major;
  uint8_t minor;
  uint8_t build;
} DriveVersion;

enum class OmniDriveDiscType : uint8_t {
  kCD,
  kDVD,
};

enum class OmniDriveSubchannel : uint8_t {
  kNone,
  kQ16,
  kRaw96,
};

enum class TransportFailureKind : uint8_t {
  kNone,
  kSizeRelated,
  kIoError,
};

struct OmniDriveAddress {
  int64_t lba;
  uint64_t sector;
  uint64_t offset;
};

struct OmnidriveDriveInfo {
  std::filesystem::path path;
  bool omnidrive_confirmed = false;
  bool media_inserted = false;
  DriveVersion omnidrive_version = {0};
  std::string
      identity_summary;  // e.g. "PLDS DG-16D5S rev0500 (OmniDrive v1.0.4)"
};

class DiscOmnidriveDevice : public OpticalDriveDevice {
 public:
  DiscOmnidriveDevice(std::string_view mount_path,
                      const std::filesystem::path& host_path,
                      OmniDriveDiscType disc_type, bool raw_dump_mode = false);
  ~DiscOmnidriveDevice() override;

  bool Initialize() override;

  void Dump(StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;
  bool ReadFileBytes(size_t offset, std::span<uint8_t> buffer) const override;
  bool is_media_available() const override;
  bool ReadFileBytesAsync(size_t offset, std::span<uint8_t> buffer) const {
    return ReadFileBytes(offset, buffer);
  }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override {
    return total_sector_units() / sectors_per_allocation_unit();
  }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override {
    return static_cast<uint32_t>(physical_sector_size());
  }

  bool media_inserted() const { return media_inserted_; }

  bool omnidrive_firmware_confirmed() const {
    return omnidrive_firmware_confirmed_;
  }
  DriveVersion* omnidrive_version() const { return &omnidrive_version_; }
  const std::string& omnidrive_identity_summary() const {
    return identity_summary_;
  }

  // Performs only physical-transport open + TEST UNIT READY/INQUIRY
  // preflight (InitializePhysicalTransport + InitializePhysicalDriveStateOnce).
  // Does NOT call DiscImageDevice::Initialize() or parse any disc image
  // structure. Intended for cheap "is this drive present/usable" probing,
  // e.g. from EmulatorWindow's File-menu-opening hook.
  bool ProbeOnly();

  // Linux: scans /dev/sg* only (SCSI generic nodes; deliberately does NOT
  // also scan /dev/sr*/cdrom/dvd to avoid double-listing one physical drive
  // under two device nodes). Windows: scans logical drives of type
  // DRIVE_CDROM. Other platforms: returns empty.
  static std::vector<std::filesystem::path>
  EnumerateCandidateOpticalDrivePaths();

  // Enumerates candidates, constructs a transient (unregistered, not
  // mounted into any VFS) DiscOmnidriveDevice per candidate, calls
  // ProbeOnly() on each, and returns info for every candidate that at least
  // has a working physical transport (media_inserted may be false — caller
  // filters for "qualifying" = firmware_confirmed && media_inserted).
  static std::vector<OmnidriveDriveInfo> ProbeAllCandidateDrives();

  std::unique_ptr<std::vector<uint8_t>> ReadSecuritySectorFromCandidates()
      const;

 private:
  size_t total_sector_units() const;

  bool ReadOmniDriveBlocks(
      uint32_t address, uint32_t transfer_length, bool raw_addressing, bool fua,
      bool descramble, OmniDriveSubchannel subchannels, bool c2,
      uint8_t* buffer, size_t buffer_length,
      TransportFailureKind* out_failure_kind = nullptr) const;
  bool ReadDiskBytesAsync(size_t offset, std::span<uint8_t> buffer) const;

  void set_layer0_last(uint32_t layer0_last) {
    layer0_last_ = layer0_last;
    layer0_last_available_ = true;
    layer0_last_auto_discovered_ = false;
  }
  uint32_t layer0_last() const { return layer0_last_; }
  bool layer0_last_available() const { return layer0_last_available_; }
  bool layer0_last_auto_discovered() const {
    return layer0_last_auto_discovered_;
  }

  bool physical_preflight_attempted() const {
    return physical_drive_state_attempted_;
  }
  bool physical_preflight_ready() const { return physical_drive_state_ready_; }
  const char* physical_preflight_failed_command() const {
    return physical_drive_state_failed_command_;
  }

 private:
  struct CachedReadRange {
    uint64_t sector_start;
    uint32_t sector_count;
    std::vector<uint8_t> data;
    // Frequency counter: starts at 1 if inserted by a demand read, 0 if
    // inserted by a prefetch; incremented by 1 on every subsequent cache
    // hit. Primary eviction key (evict lowest first).
    uint64_t use_tick;
    // Set once at insertion time, never modified afterward. Tiebreak key
    // for eviction when use_tick is equal: evict the smallest (oldest)
    // insertion_tick first.
    uint64_t insertion_tick;
  };

  struct ReadTelemetry {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> async_dispatches{0};
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> cache_misses{0};
    std::atomic<uint64_t> prefetch_requests{0};
    std::atomic<uint64_t> prefetch_hits{0};
    std::atomic<uint64_t> prefetch_misses{0};
    std::atomic<uint64_t> prefetch_deferred{0};
    std::atomic<uint64_t> prefetch_skipped{0};
    std::atomic<uint64_t> prefetch_exec_count{0};
    std::atomic<uint64_t> prefetch_monotonic_ok{0};
    std::atomic<uint64_t> prefetch_monotonic_breaks{0};
    std::atomic<uint64_t> prefetch_coalesced{0};
    std::atomic<uint64_t> prefetch_superseded{0};
    std::atomic<uint64_t> demand_queue_max_depth{0};
    std::atomic<uint64_t> failed_reads{0};
    std::atomic<uint64_t> disk_issue_total{0};
    std::atomic<uint64_t> disk_issue_contiguous{0};
    std::atomic<uint64_t> disk_issue_forward_gap{0};
    std::atomic<uint64_t> disk_issue_backward{0};
    std::atomic<uint64_t> disk_issue_largest_forward_gap{0};
    std::atomic<uint64_t> disk_issue_last_start{
        std::numeric_limits<uint64_t>::max()};
    std::atomic<uint64_t> disk_issue_last_end{
        std::numeric_limits<uint64_t>::max()};
  };

  struct PendingPrefetchRequest {
    uint64_t sector_start;
    uint32_t sector_count;
  };

  size_t physical_sector_size() const;

  int64_t TranslateAddressToLba(uint32_t address, bool raw_addressing) const;
  OmniDriveAddress TranslateAddress(uint32_t address,
                                    bool raw_addressing) const;

  bool InitializePhysicalTransport();
  struct InquiryProbeResult {
    bool omnidrive_detected = false;
    struct {
      uint8_t major;
      uint8_t minor;
      uint8_t build;
    } omnidrive_version = {0};
    std::string vendor;
    std::string product;
    std::string revision;
  };

  InquiryProbeResult ProbeFirmwareIdentityByInquiry() const;
  bool InitializePhysicalDriveStateOnce() const;
  bool InitializePhysicalDriveStateOnceLinux() const;
#if XE_PLATFORM_WIN32
  bool InitializePhysicalDriveStateOnceWindows() const;
#endif  // XE_PLATFORM_WIN32
  bool ReadFromPhysicalTransport(uint32_t address, uint32_t transfer_length,
                                 bool raw_addressing, bool fua, bool descramble,
                                 OmniDriveSubchannel subchannels, bool c2,
                                 uint8_t* buffer, size_t buffer_length,
                                 TransportFailureKind* out_failure_kind) const;
  uint32_t EffectiveTransportReadChunkCap() const;
  void LowerTransportReadChunkCap(uint32_t reduced_cap) const;
  bool EnsureReadWorkerStarted() const;
  void StopReadWorker();
  void ReadWorkerMain();
  std::future<bool> EnqueueReadTask(std::function<bool()> task) const;
  bool ReadSectorsBlocking(uint64_t sector_start, uint32_t sector_count,
                           uint8_t* buffer) const;
  bool ExecuteDemandRead(uint64_t sector_start, uint32_t sector_count,
                         uint8_t* buffer) const;
  bool ReadMetadataBytesFromPhysicalTransport(size_t offset,
                                              std::span<uint8_t> buffer) const;
  bool IsRangeCachedLocked(uint64_t sector_start, uint32_t sector_count,
                           uint64_t* cached_end = nullptr) const;
  void InsertCacheRange(uint64_t sector_start, uint32_t sector_count,
                        std::span<const uint8_t> data,
                        bool is_demand_read) const;
  void TryScheduleSequentialPrefetch(uint64_t current_end_sector) const;
  bool ExecutePrefetchRead(uint64_t sector_start, uint32_t sector_count) const;
  void MaybeLogReadTelemetry() const;
  void RecordDiskIssue(uint64_t start_sector, uint32_t sector_count) const;

  std::filesystem::path host_path_;
  OmniDriveDiscType disc_type_;
  bool raw_dump_mode_;
  bool is_physical_device_path_;
  mutable uint32_t layer0_last_;
  mutable bool layer0_last_available_;
  mutable bool layer0_last_auto_discovered_;
  mutable bool physical_drive_state_attempted_;
  mutable bool physical_drive_state_ready_;
  mutable const char* physical_drive_state_failed_command_;
  mutable bool omnidrive_firmware_confirmed_;
  mutable bool media_inserted_ = false;
  mutable uint32_t total_sector_units_;
  mutable bool total_sector_units_available_;
  mutable DriveVersion omnidrive_version_ = {0};
  mutable std::string identity_summary_;

  mutable std::mutex read_worker_mutex_;
  mutable std::condition_variable read_worker_cv_;
  mutable std::deque<std::function<void()>> read_worker_demand_tasks_;
  mutable std::optional<PendingPrefetchRequest> pending_prefetch_request_;
  mutable std::unique_ptr<std::thread> read_worker_thread_;
  mutable bool read_worker_stop_;
  mutable std::atomic<uint64_t> pending_demand_reads_{0};
  mutable std::atomic<uint64_t> queued_demand_tasks_{0};
  mutable std::atomic<uint64_t> last_prefetch_exec_start_{0};
  mutable std::atomic<uint64_t> last_prefetch_exec_end_{0};
  mutable std::atomic<bool> has_last_prefetch_exec_{false};
  mutable std::atomic<uint32_t> transport_read_chunk_cap_;

  mutable std::mutex cache_mutex_;
  mutable std::deque<CachedReadRange> read_cache_;
  mutable size_t read_cache_bytes_;
  mutable uint64_t cache_insertion_count_;
  mutable bool last_read_valid_;
  mutable uint64_t last_read_end_sector_;

  mutable ReadTelemetry read_telemetry_;
  mutable std::atomic<bool> path_log_emitted_{false};

  bool has_physical_transport_;
  int physical_transport_fd_;

#if XE_PLATFORM_WIN32
  enum class PhysicalTransportBackend : uint8_t {
    kNone,
    kLinuxStyleFile,
    kWinSpti,
  };

  bool TryAcquireWindowsExclusiveAccess();
  void ReleaseWindowsExclusiveAccess();

  void* physical_transport_handle_;
  bool running_under_wine_;
  PhysicalTransportBackend physical_transport_backend_;
  mutable bool windows_exclusive_access_attempted_;
  mutable bool windows_exclusive_access_held_;
  mutable DWORD windows_exclusive_access_last_error_;
#endif  // XE_PLATFORM_WIN32

  std::string name_;
  std::unique_ptr<Entry> root_entry_;

  typedef struct {
    size_t size;         // Size (bytes) of total image.
    size_t game_offset;  // Offset (bytes) of game partition.
    size_t root_sector;  // Offset (sector) of root.
    size_t root_offset;  // Offset (bytes) of root.
    size_t root_size;    // Size (bytes) of root.
  } ParseState;

  enum class Error {
    kSuccess = 0,
    kErrorOutOfMemory = -1,
    kErrorReadError = -10,
    kErrorFileMismatch = -30,
    kErrorDamagedFile = -31,
  };

  Error Verify(ParseState* state);
  bool VerifyMagic(ParseState* state, size_t offset);
  Error ReadAllEntries(ParseState* state, const uint8_t* root_buffer);
  bool ReadEntry(ParseState* state, const uint8_t* buffer, size_t buffer_length,
                 uint16_t entry_ordinal, DiscDriveEntry* parent);
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_DISC_OMNIDRIVE_DEVICE_H_
