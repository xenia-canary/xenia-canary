#include "xenia/vfs/devices/disc_omnidrive_device.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#if XE_PLATFORM_LINUX
#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif  // XE_PLATFORM_LINUX

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"

#include <winioctl.h>  //The next newline fixes a linter bug. Removing it will break compilation.

#include <ntddcdrm.h>
#include <ntddscsi.h>

namespace xe {
extern bool IsRunningUnderWine();
}
#endif  // XE_PLATFORM_WIN32

#include "xenia/app/physical_drive_dialogs.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/system.h"
#include "xenia/base/xxhash.h"
#include "xenia/vfs/devices/disc_drive_entry.h"

namespace xe::vfs {
namespace {

using namespace xe::literals;

#pragma pack(push, 1)
struct CDB12_ReadOmniDrive {
  uint8_t operation_code;
  uint8_t byte1;
  uint8_t address[4];
  uint8_t transfer_length[4];
  uint8_t byte10;
  uint8_t control;
};
#pragma pack(pop)
static_assert(sizeof(CDB12_ReadOmniDrive) == 12,
              "CDB12_ReadOmniDrive must be 12 bytes");

#pragma pack(push, 1)
struct CDB12_Read12 {
  uint8_t operation_code;
  uint8_t byte1;
  uint8_t logical_block_address[4];
  uint8_t transfer_length[4];
  uint8_t byte10;
  uint8_t control;
};
#pragma pack(pop)
static_assert(sizeof(CDB12_Read12) == 12, "CDB12_Read12 must be 12 bytes");

constexpr size_t kOmniDriveDvdFrameSize = 2064;
constexpr size_t kOmniDriveDvdMainDataOffset = 12;
constexpr size_t kOmniDriveDvdMainDataSize = 2048;
constexpr size_t kGdfSectorSize = 2_KiB;
constexpr uint64_t kUnixEpochAsFiletime = 10000 * 11644473600000LL;
constexpr uint64_t kInvalidIssueSector = std::numeric_limits<uint64_t>::max();
constexpr uint32_t kSequentialPrefetchSectors = 64;
constexpr uint32_t kMaxTransportReadSectors = 256;
constexpr size_t kReadCacheMaxBytes = 32 * 1024 * 1024;
constexpr uint32_t kSecuritySectorProbeBaseAddress = 0x00FD021E;
constexpr uint32_t kSecuritySectorProbeRetryStride = 0x40;
constexpr uint32_t kSecuritySectorProbeRetryCount = 4;

std::string HexPreview(const uint8_t* data, size_t length,
                       size_t max_bytes = 16) {
  if (!data || length == 0) {
    return "empty";
  }
  const size_t bytes = std::min(length, max_bytes);
  std::string out;
  out.reserve(bytes * 3);
  char hex[4] = {};
  for (size_t i = 0; i < bytes; ++i) {
    std::snprintf(hex, sizeof(hex), "%02X", data[i]);
    if (i != 0) {
      out.push_back(' ');
    }
    out.append(hex);
  }
  return out;
}

bool IsParseRelevantRead(uint32_t transfer_length) {
  return transfer_length > 0 && transfer_length <= 64;
}

void PackBE32(uint32_t value, uint8_t out[4]) {
  out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[3] = static_cast<uint8_t>(value & 0xFF);
}

CDB12_ReadOmniDrive BuildReadOmniDriveCdb(
    uint32_t address, uint32_t transfer_length, OmniDriveDiscType disc_type,
    bool raw_addressing, bool fua, bool descramble,
    OmniDriveSubchannel subchannels, bool c2) {
  CDB12_ReadOmniDrive cdb{};
  cdb.operation_code = 0xC0;

  cdb.byte1 = static_cast<uint8_t>(static_cast<uint8_t>(disc_type) & 0x03);
  if (raw_addressing) {
    cdb.byte1 |= (1u << 2);
  }
  if (fua) {
    cdb.byte1 |= (1u << 3);
  }
  if (descramble) {
    cdb.byte1 |= (1u << 4);
  }

  PackBE32(static_cast<uint32_t>(static_cast<int32_t>(address)), cdb.address);
  PackBE32(transfer_length, cdb.transfer_length);

  cdb.byte10 = static_cast<uint8_t>(static_cast<uint8_t>(subchannels) & 0x03);
  if (c2) {
    cdb.byte10 |= (1u << 2);
  }

  cdb.control = 0;
  return cdb;
}

CDB12_Read12 BuildRead12Cdb(uint32_t lba, uint32_t transfer_length, bool fua) {
  CDB12_Read12 cdb{};
  cdb.operation_code = 0xA8;
  cdb.byte1 = fua ? static_cast<uint8_t>(1u << 3) : 0;
  PackBE32(lba, cdb.logical_block_address);
  PackBE32(transfer_length, cdb.transfer_length);
  cdb.byte10 = 0;
  cdb.control = 0;
  return cdb;
}

bool StartsWith(const std::string& value, const char* prefix) {
  const size_t prefix_length = std::strlen(prefix);
  return value.size() >= prefix_length &&
         value.compare(0, prefix_length, prefix) == 0;
}

std::string TrimInquiryField(const uint8_t* field, size_t length) {
  size_t begin = 0;
  while (begin < length && field[begin] == ' ') {
    ++begin;
  }

  size_t end = length;
  while (end > begin && field[end - 1] == ' ') {
    --end;
  }

  std::string text;
  text.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    const char c = static_cast<char>(field[i]);
    if (c >= 0x20 && c <= 0x7E) {
      text.push_back(c);
    }
  }
  return text;
}

std::string ToUpperAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return value;
}

struct InquiryIdentityDetection {
  bool omnidrive_confirmed = false;
  struct {
    uint8_t major;
    uint8_t minor;
    uint8_t build;
  } omnidrive_version = {0};
  std::string vendor;
  std::string product;
  std::string revision;
};

// Offset of the `reserved5` field within a standard 104-byte SCSI INQUIRY
// response (INQUIRY_StandardData). Real OmniDrive custom firmware embeds a
// literal "OmniDrive" ASCII marker followed by a 3-byte version number here.
// Verified against superg/redumper's drive.ixx `is_omnidrive_firmware()`.
constexpr size_t kOmnidriveReserved5Offset = 74;
constexpr char kOmnidriveFirmwareMarker[] = "OmniDrive";
constexpr size_t kOmnidriveFirmwareMarkerLength = 9;
constexpr DriveVersion kOmnidriveMinimumFirmwareVersion{1, 0, 2};

InquiryIdentityDetection ParseOmnidriveInquiryIdentity(
    const uint8_t* inquiry_data, size_t inquiry_data_length) {
  InquiryIdentityDetection result;
  if (!inquiry_data || inquiry_data_length < 36) {
    return result;
  }

  // Firmware detection: real OmniDrive custom firmware embeds a literal
  // "OmniDrive" ASCII marker in the INQUIRY response's `reserved5` field
  // (offset 74), followed by a 3-byte version number. This is independent
  // of the vendor/product allowlist check below, matching redumper's
  // `is_omnidrive_firmware()` (drive.ixx).
  const size_t required_length =
      kOmnidriveReserved5Offset + kOmnidriveFirmwareMarkerLength + 3;
  if (inquiry_data_length >= required_length) {
    const uint8_t* reserved5 = inquiry_data + kOmnidriveReserved5Offset;
    if (std::memcmp(reserved5, kOmnidriveFirmwareMarker,
                    kOmnidriveFirmwareMarkerLength) == 0) {
      const uint8_t* version_bytes = reserved5 + kOmnidriveFirmwareMarkerLength;
      result.omnidrive_confirmed = true;
      result.omnidrive_version.major = version_bytes[0];
      result.omnidrive_version.minor = version_bytes[1];
      result.omnidrive_version.build = version_bytes[2];
    }
  }

  // Redumper-style identity check: optical class + known Xbox 360 drive
  // families, used for menu-visibility purposes only (unchanged from the
  // prior implementation; not part of firmware confirmation).
  const uint8_t peripheral_device_type = inquiry_data[0] & 0x1F;
  if (peripheral_device_type != 0x05) {
    return result;
  }

  return result;
}

bool IsPlausibleLinuxScsiPath(const std::filesystem::path& path) {
  const std::string path_string = path.string();
  if (!StartsWith(path_string, "/dev/")) {
    return false;
  }

  const std::string node = path.filename().string();
  return StartsWith(node, "sg") || StartsWith(node, "sr") || node == "cdrom" ||
         node == "dvd";
}

#if XE_PLATFORM_LINUX
bool RunLinuxScsiCommand(int fd, const char* command_name, const uint8_t* cdb,
                         uint8_t cdb_length, int data_direction,
                         void* data_buffer, uint32_t data_length,
                         uint32_t timeout_ms = 8000,
                         uint8_t* out_sense_key = nullptr,
                         uint8_t* out_asc = nullptr,
                         uint8_t* out_ascq = nullptr) {
  uint8_t sense_buffer[32] = {0};
  sg_io_hdr_t io_hdr = {};
  io_hdr.interface_id = 'S';
  io_hdr.dxfer_direction = data_direction;
  io_hdr.cmd_len = cdb_length;
  io_hdr.mx_sb_len = sizeof(sense_buffer);
  io_hdr.dxfer_len = data_length;
  io_hdr.dxferp = data_buffer;
  io_hdr.cmdp = const_cast<uint8_t*>(cdb);
  io_hdr.sbp = sense_buffer;
  io_hdr.timeout = timeout_ms;

  if (ioctl(fd, SG_IO, &io_hdr) != 0) {
    XELOGW("DiscOmnidriveDevice: {} failed with SG_IO error: {}", command_name,
           std::strerror(errno));
    return false;
  }

  if (io_hdr.sb_len_wr >= 14 &&
      (sense_buffer[0] & 0x7F) == 0x70 /* current fixed-format sense */) {
    if (out_sense_key) {
      *out_sense_key = sense_buffer[2] & 0x0F;
    }
    if (out_asc) {
      *out_asc = sense_buffer[12];
    }
    if (out_ascq) {
      *out_ascq = sense_buffer[13];
    }
  }

  if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK || io_hdr.status != 0 ||
      io_hdr.host_status != 0 || io_hdr.driver_status != 0) {
    XELOGW(
        "DiscOmnidriveDevice: {} status=0x{:02X} host=0x{:04X} "
        "driver=0x{:04X} sense_len={}",
        command_name, io_hdr.status, io_hdr.host_status, io_hdr.driver_status,
        io_hdr.sb_len_wr);
    return false;
  }

  return true;
}

#endif  // XE_PLATFORM_LINUX

#if XE_PLATFORM_WIN32
std::string ToLowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool IsPlausibleWindowsCdromPath(const std::filesystem::path& path) {
  const std::string path_string = ToLowerAscii(path.string());
  if (StartsWith(path_string, "\\\\.\\cdrom")) {
    return true;
  }
  if (path_string.size() >= 2 && std::isalpha(path_string[0]) &&
      path_string[1] == ':') {
    return true;
  }
  if (StartsWith(path_string, "\\\\.\\")) {
    return true;
  }
  return false;
}

std::filesystem::path TranslateWineLinuxPath(
    const std::filesystem::path& path) {
  const std::string path_string = path.string();
  if (!StartsWith(path_string, "/")) {
    return path;
  }

  std::filesystem::path translated("Z:/");
  translated /= path.relative_path();
  return translated;
}

struct SptdWithSense {
  SCSI_PASS_THROUGH_DIRECT sptd;
  ULONG filler;
  UCHAR sense_buffer[32];
};
#endif  // XE_PLATFORM_WIN32

}  // namespace

DiscOmnidriveDevice::DiscOmnidriveDevice(std::string_view mount_path,
                                         const std::filesystem::path& host_path,
                                         OmniDriveDiscType disc_type,
                                         bool raw_dump_mode)
    : OpticalDriveDevice(mount_path),
      name_("GDFX"),
      host_path_(host_path),
      disc_type_(disc_type),
      raw_dump_mode_(raw_dump_mode),
      layer0_last_(0),
      layer0_last_available_(false),
      layer0_last_auto_discovered_(false),
      physical_drive_state_attempted_(false),
      physical_drive_state_ready_(false),
      physical_drive_state_failed_command_("none"),
      omnidrive_firmware_confirmed_(false),
      total_sector_units_(0),
      total_sector_units_available_(false),
      read_worker_stop_(false),
      transport_read_chunk_cap_(kMaxTransportReadSectors),
      read_cache_bytes_(0),
      cache_use_tick_(0),
      last_read_valid_(false),
      last_read_end_sector_(0),
#if XE_PLATFORM_LINUX
      physical_transport_fd_(-1),
      has_physical_transport_(false)
#endif
#if XE_PLATFORM_WIN32
          physical_transport_handle_(INVALID_HANDLE_VALUE),
      has_physical_transport_(false),
      running_under_wine_(false),
      physical_transport_backend_(PhysicalTransportBackend::kNone),
      windows_exclusive_access_attempted_(false),
      windows_exclusive_access_held_(false),
      windows_exclusive_access_last_error_(0)
#endif  // XE_PLATFORM_WIN32
{
}

DiscOmnidriveDevice::~DiscOmnidriveDevice() {
  StopReadWorker();

#if XE_PLATFORM_LINUX
  if (physical_transport_fd_ >= 0) {
    close(physical_transport_fd_);
    physical_transport_fd_ = -1;
  }
#endif  // XE_PLATFORM_LINUX

#if XE_PLATFORM_WIN32
  if (physical_transport_handle_ != INVALID_HANDLE_VALUE) {
    if (windows_exclusive_access_held_) {
      ReleaseWindowsExclusiveAccess();
    }
    CloseHandle(static_cast<HANDLE>(physical_transport_handle_));
    physical_transport_handle_ = INVALID_HANDLE_VALUE;
  }
#endif  // XE_PLATFORM_WIN32
}

bool DiscOmnidriveDevice::Initialize() {
  root_entry_.reset();

  if (!InitializePhysicalTransport()) {
    XELOGE(
        "DiscOmnidriveDevice::Initialize: failed for {} because physical "
        "transport init failed",
        host_path_.string());
    return false;
  }

  if (!InitializePhysicalDriveStateOnce()) {
    XELOGE(
        "DiscOmnidriveDevice::Initialize: failed for {} because physical "
        "preflight failed at {}",
        host_path_.string(), physical_preflight_failed_command());
    return false;
  }

  if (!media_inserted_) {
    XELOGE(
        "DiscOmnidriveDevice::Initialize: failed for {} because no media is "
        "inserted",
        host_path_.string());
    return false;
  }

  ParseState state = {};
  const size_t sector_units = total_sector_units();
  if (sector_units != 0 &&
      sector_units <= (std::numeric_limits<size_t>::max)() / kGdfSectorSize) {
    state.size = sector_units * kGdfSectorSize;
  } else {
    state.size = (std::numeric_limits<size_t>::max)();
  }

  const Error verify_result = Verify(&state);
  if (verify_result != Error::kSuccess) {
    XELOGE("DiscOmnidriveDevice::Initialize: metadata verify failed for {}: {}",
           host_path_.string(), static_cast<int32_t>(verify_result));
    return false;
  }

  std::vector<uint8_t> root_buffer(state.root_size);
  if (!ReadMetadataBytesFromPhysicalTransport(
          state.root_offset,
          std::span<uint8_t>(root_buffer.data(), root_buffer.size()))) {
    XELOGE(
        "DiscOmnidriveDevice::Initialize: failed reading root metadata for "
        "{} (offset={} size={})",
        host_path_.string(), state.root_offset, state.root_size);
    return false;
  }

  const Error read_result = ReadAllEntries(&state, root_buffer.data());
  if (read_result != Error::kSuccess) {
    XELOGE(
        "DiscOmnidriveDevice::Initialize: failed building entry tree for "
        "{}: {}",
        host_path_.string(), static_cast<int32_t>(read_result));
    root_entry_.reset();
    return false;
  }

  return true;
}

void DiscOmnidriveDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  if (root_entry_) {
    root_entry_->Dump(string_buffer, 0);
  }
}

Entry* DiscOmnidriveDevice::ResolvePath(const std::string_view path) {
  XELOGFS("DiscOmnidriveDevice::ResolvePath({})", path);
  if (!root_entry_) {
    return nullptr;
  }
  return root_entry_->ResolvePath(path);
}

bool DiscOmnidriveDevice::ReadFileBytes(size_t offset,
                                        std::span<uint8_t> buffer) const {
  return ReadDiskBytesAsync(offset, buffer);
}

std::unique_ptr<std::vector<uint8_t>>
DiscOmnidriveDevice::ReadSecuritySectorFromCandidates() const {
  if (disc_type_ != OmniDriveDiscType::kDVD) {
    return nullptr;
  }

  struct SecuritySectorCandidate {
    uint32_t address;
    int64_t lba;
    uint64_t sector;
  };

  std::vector<SecuritySectorCandidate> candidates;
  candidates.reserve(kSecuritySectorProbeRetryCount);

  for (uint32_t retry = 0; retry < kSecuritySectorProbeRetryCount; ++retry) {
    const uint32_t probe_address = kSecuritySectorProbeBaseAddress +
                                   retry * kSecuritySectorProbeRetryStride;
    const OmniDriveAddress translated = TranslateAddress(probe_address, true);
    candidates.push_back({probe_address, translated.lba, translated.sector});
  }

  std::stable_sort(
      candidates.begin(), candidates.end(),
      [](const SecuritySectorCandidate& a, const SecuritySectorCandidate& b) {
        const bool a_valid = a.lba >= 0;
        const bool b_valid = b.lba >= 0;
        if (a_valid != b_valid) {
          return a_valid;
        }
        if (!a_valid) {
          return false;
        }
        if (a.sector != b.sector) {
          return a.sector < b.sector;
        }
        return false;
      });

  for (const SecuritySectorCandidate& candidate : candidates) {
    auto sector_payload =
        std::make_unique<std::vector<uint8_t>>(SecuritySectorPayloadSize());
    if (!ReadOmniDriveBlocks(candidate.address, 1, true, false, false,
                             OmniDriveSubchannel::kNone, false,
                             sector_payload->data(), sector_payload->size(),
                             nullptr)) {
      continue;
    }
    return sector_payload;
  }

  return nullptr;
}

bool DiscOmnidriveDevice::is_media_available() const {
  const auto verify_security_sector = [this]() -> bool {
    auto sector_payload = ReadSecuritySectorFromCandidates();
    if (!sector_payload) {
      return false;
    }
    if (!IsSecuritySectorSane(*sector_payload)) {
      xe::app::ShowSecuritySectorSanityWarningOnce();
    }
    return true;
  };

  if (omnidrive_firmware_confirmed_ && media_inserted_) {
    return verify_security_sector();
  }
  if (!has_physical_transport_) {
    if (!const_cast<DiscOmnidriveDevice*>(this)
             ->InitializePhysicalTransport()) {
      return false;
    }
  }

  InitializePhysicalDriveStateOnce();
  if (!(omnidrive_firmware_confirmed_ && media_inserted_)) {
    return false;
  }
  return verify_security_sector();
}

size_t DiscOmnidriveDevice::total_sector_units() const {
  if (total_sector_units_available_) {
    return total_sector_units_;
  }
  if (layer0_last_available_) {
    return static_cast<size_t>(layer0_last_) + 1;
  }
  return 0;
}

size_t DiscOmnidriveDevice::physical_sector_size() const {
  switch (disc_type_) {
    case OmniDriveDiscType::kCD:
      return raw_dump_mode_ ? 2352 : kGdfSectorSize;
    case OmniDriveDiscType::kDVD:
      return raw_dump_mode_ ? kOmniDriveDvdFrameSize
                            : kOmniDriveDvdMainDataSize;
  }

  return kOmniDriveDvdMainDataSize;
}

int64_t DiscOmnidriveDevice::TranslateAddressToLba(uint32_t address,
                                                   bool raw_addressing) const {
  if (!raw_addressing) {
    return static_cast<int64_t>(address);
  }

  if (disc_type_ == OmniDriveDiscType::kDVD) {
    int64_t lba = static_cast<int64_t>(address) - 0x30000;
    if (lba < 0 && layer0_last_available_) {
      lba += 2 * (static_cast<int64_t>(layer0_last_) + 1);
    }
    return lba;
  }

  return static_cast<int64_t>(address);
}

OmniDriveAddress DiscOmnidriveDevice::TranslateAddress(
    uint32_t address, bool raw_addressing) const {
  OmniDriveAddress result{};
  result.lba = TranslateAddressToLba(address, raw_addressing);
  if (result.lba < 0) {
    result.sector = 0;
    result.offset = 0;
    return result;
  }

  const uint64_t sector = static_cast<uint64_t>(result.lba);
  const uint64_t sector_size = static_cast<uint64_t>(physical_sector_size());
  if (sector_size != 0 &&
      sector > (std::numeric_limits<uint64_t>::max)() / sector_size) {
    result.sector = 0;
    result.offset = 0;
    return result;
  }

  result.sector = sector;
  result.offset = sector * sector_size;
  return result;
}

bool DiscOmnidriveDevice::ReadOmniDriveBlocks(
    uint32_t address, uint32_t transfer_length, bool raw_addressing, bool fua,
    bool descramble, OmniDriveSubchannel subchannels, bool c2, uint8_t* buffer,
    size_t buffer_length, TransportFailureKind* out_failure_kind) const {
  if (out_failure_kind) {
    *out_failure_kind = TransportFailureKind::kNone;
  }

  if (!buffer || transfer_length == 0) {
    return false;
  }

  const size_t sector_size = physical_sector_size();
  if (sector_size == 0 ||
      transfer_length > (std::numeric_limits<size_t>::max)() / sector_size) {
    return false;
  }
  const size_t read_length = static_cast<size_t>(transfer_length) * sector_size;
  if (buffer_length < read_length) {
    return false;
  }

  if (ReadFromPhysicalTransport(address, transfer_length, raw_addressing, fua,
                                descramble, subchannels, c2, buffer,
                                buffer_length, out_failure_kind)) {
    return true;
  }

  const TransportFailureKind failure_kind =
      out_failure_kind ? *out_failure_kind : TransportFailureKind::kNone;
  if (failure_kind == TransportFailureKind::kSizeRelated) {
    // Expected during adaptive split fallback for oversized requests.
    return false;
  }

  XELOGE(
      "DiscOmnidriveDevice::ReadOmniDriveBlocks: physical transport read "
      "failed for {}; direct device read is required",
      host_path_.string());
  return false;
}

bool DiscOmnidriveDevice::EnsureReadWorkerStarted() const {
  std::lock_guard<std::mutex> lock(read_worker_mutex_);
  if (read_worker_thread_) {
    return true;
  }
  read_worker_stop_ = false;
  read_worker_thread_ = std::make_unique<std::thread>(
      [this]() { const_cast<DiscOmnidriveDevice*>(this)->ReadWorkerMain(); });
  return true;
}

void DiscOmnidriveDevice::StopReadWorker() {
  {
    std::lock_guard<std::mutex> lock(read_worker_mutex_);
    read_worker_stop_ = true;
  }
  read_worker_cv_.notify_all();
  if (read_worker_thread_ && read_worker_thread_->joinable()) {
    read_worker_thread_->join();
  }
  read_worker_thread_.reset();
}

void DiscOmnidriveDevice::ReadWorkerMain() {
  for (;;) {
    std::function<void()> task;
    PendingPrefetchRequest prefetch_chunk{};
    bool has_prefetch_chunk = false;
    {
      std::unique_lock<std::mutex> lock(read_worker_mutex_);
      read_worker_cv_.wait(lock, [this]() {
        return read_worker_stop_ || !read_worker_demand_tasks_.empty() ||
               pending_prefetch_request_.has_value();
      });
      if (read_worker_stop_) {
        return;
      }
      if (!read_worker_demand_tasks_.empty()) {
        task = std::move(read_worker_demand_tasks_.front());
        read_worker_demand_tasks_.pop_front();
        queued_demand_tasks_.fetch_sub(1, std::memory_order_relaxed);
      } else if (pending_prefetch_request_.has_value()) {
        PendingPrefetchRequest& pending = *pending_prefetch_request_;
        const uint32_t chunk_count =
            std::min(pending.sector_count, kSequentialPrefetchSectors);
        prefetch_chunk = {pending.sector_start, chunk_count};
        has_prefetch_chunk = true;
        pending.sector_start += chunk_count;
        pending.sector_count -= chunk_count;
        if (pending.sector_count == 0) {
          pending_prefetch_request_.reset();
        }
      }
    }
    if (task) {
      task();
      continue;
    }
    if (!has_prefetch_chunk) {
      continue;
    }

    const bool demand_arrived =
        pending_demand_reads_.load(std::memory_order_relaxed) != 0;
    if (demand_arrived) {
      {
        std::lock_guard<std::mutex> lock(read_worker_mutex_);
        if (pending_prefetch_request_.has_value()) {
          auto& pending = *pending_prefetch_request_;
          const uint64_t merged_end = std::max(
              pending.sector_start + pending.sector_count - 1,
              prefetch_chunk.sector_start + prefetch_chunk.sector_count - 1);
          pending.sector_start =
              std::min(pending.sector_start, prefetch_chunk.sector_start);
          pending.sector_count =
              static_cast<uint32_t>(merged_end - pending.sector_start + 1);
        } else {
          pending_prefetch_request_ = prefetch_chunk;
        }
      }
      read_telemetry_.prefetch_skipped.fetch_add(1, std::memory_order_relaxed);
      read_telemetry_.prefetch_deferred.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    if (!ExecutePrefetchRead(prefetch_chunk.sector_start,
                             prefetch_chunk.sector_count)) {
      read_telemetry_.prefetch_misses.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

std::future<bool> DiscOmnidriveDevice::EnqueueReadTask(
    std::function<bool()> task) const {
  if (!EnsureReadWorkerStarted()) {
    std::promise<bool> failed;
    failed.set_value(false);
    return failed.get_future();
  }

  auto promise = std::make_shared<std::promise<bool>>();
  auto future = promise->get_future();
  {
    std::lock_guard<std::mutex> lock(read_worker_mutex_);
    const uint64_t demand_depth =
        queued_demand_tasks_.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t previous_max =
        read_telemetry_.demand_queue_max_depth.load(std::memory_order_relaxed);
    while (demand_depth > previous_max &&
           !read_telemetry_.demand_queue_max_depth.compare_exchange_weak(
               previous_max, demand_depth, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    read_worker_demand_tasks_.emplace_back(
        [promise, task = std::move(task)]() mutable {
          promise->set_value(task());
        });
  }
  read_worker_cv_.notify_one();
  return future;
}

uint32_t DiscOmnidriveDevice::EffectiveTransportReadChunkCap() const {
  const uint32_t cap =
      transport_read_chunk_cap_.load(std::memory_order_relaxed);
  if (cap == 0) {
    return kMaxTransportReadSectors;
  }
  return std::min(cap, kMaxTransportReadSectors);
}

void DiscOmnidriveDevice::LowerTransportReadChunkCap(
    uint32_t reduced_cap) const {
  const uint32_t clamped_cap =
      std::max(1u, std::min(reduced_cap, kMaxTransportReadSectors));

  uint32_t current_cap =
      transport_read_chunk_cap_.load(std::memory_order_relaxed);
  if (current_cap == 0) {
    current_cap = kMaxTransportReadSectors;
  }

  while (clamped_cap < current_cap) {
    if (transport_read_chunk_cap_.compare_exchange_weak(
            current_cap, clamped_cap, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return;
    }
    if (current_cap == 0) {
      current_cap = kMaxTransportReadSectors;
    }
  }
}

bool DiscOmnidriveDevice::ExecuteDemandRead(uint64_t sector_start,
                                            uint32_t sector_count,
                                            uint8_t* buffer) const {
  const uint32_t initial_chunk_cap = EffectiveTransportReadChunkCap();
  if (sector_count > initial_chunk_cap) {
    XELOGD(
        "DiscOmnidriveDevice::ReadSectorsBlocking: chunking "
        "demand read start={} blocks={} chunk_max={}",
        sector_start, sector_count, initial_chunk_cap);
  }

  constexpr int kMaxHalvingSteps = 5;

  uint64_t chunk_start = sector_start;
  uint32_t remaining = sector_count;
  size_t buffer_offset = 0;
  while (remaining != 0) {
    const uint32_t chunk_cap = EffectiveTransportReadChunkCap();
    uint32_t chunk_count = std::min(remaining, chunk_cap);
    int halving_steps = 0;
    bool same_size_retry_used = false;
    while (true) {
      const size_t chunk_length =
          static_cast<size_t>(chunk_count) * physical_sector_size();
      TransportFailureKind failure_kind = TransportFailureKind::kNone;
      if (ReadOmniDriveBlocks(static_cast<uint32_t>(chunk_start), chunk_count,
                              false, false, true, OmniDriveSubchannel::kNone,
                              false, buffer + buffer_offset, chunk_length,
                              &failure_kind)) {
        chunk_start += chunk_count;
        remaining -= chunk_count;
        buffer_offset += chunk_length;
        break;
      }

      if (failure_kind == TransportFailureKind::kIoError) {
        if (!same_size_retry_used) {
          same_size_retry_used = true;
          XELOGW(
              "DiscOmnidriveDevice::ReadSectorsBlocking: "
              "I/O error at sector={} chunk={}; retrying once "
              "at same chunk size",
              chunk_start, chunk_count);
          continue;
        }
        XELOGE(
            "DiscOmnidriveDevice::ReadSectorsBlocking: genuine "
            "I/O error persisted at sector={} chunk={}; failing fast",
            chunk_start, chunk_count);
        return false;
      }

      if (chunk_count == 1 || halving_steps >= kMaxHalvingSteps - 1) {
        XELOGE(
            "DiscOmnidriveDevice::ReadSectorsBlocking: giving "
            "up at sector={} chunk={} after {} halving steps",
            chunk_start, chunk_count, halving_steps);
        return false;
      }

      const uint32_t reduced_chunk_count = std::max(1u, chunk_count / 4);
      ++halving_steps;
      if (failure_kind == TransportFailureKind::kSizeRelated) {
        LowerTransportReadChunkCap(reduced_chunk_count);
      }
      same_size_retry_used = false;
      chunk_count = reduced_chunk_count;
    }
  }

  return true;
}

bool DiscOmnidriveDevice::ReadSectorsBlocking(uint64_t sector_start,
                                              uint32_t sector_count,
                                              uint8_t* buffer) const {
  if (sector_count == 0) {
    return true;
  }
  if (!buffer) {
    return false;
  }
  if (sector_start > (std::numeric_limits<uint32_t>::max)()) {
    return false;
  }
  if (sector_count > (std::numeric_limits<uint32_t>::max)() -
                         static_cast<uint32_t>(sector_start)) {
    return false;
  }

  pending_demand_reads_.fetch_add(1, std::memory_order_relaxed);
  read_telemetry_.async_dispatches.fetch_add(1, std::memory_order_relaxed);
  struct PendingDemandReadGuard {
    explicit PendingDemandReadGuard(std::atomic<uint64_t>* counter)
        : counter(counter) {}
    ~PendingDemandReadGuard() {
      counter->fetch_sub(1, std::memory_order_relaxed);
    }
    std::atomic<uint64_t>* counter;
  } pending_demand_read_guard(&pending_demand_reads_);

  return ExecuteDemandRead(sector_start, sector_count, buffer);
}

bool DiscOmnidriveDevice::ReadMetadataBytesFromPhysicalTransport(
    size_t offset, std::span<uint8_t> buffer) const {
  return ReadDiskBytesAsync(offset, buffer);
}

bool DiscOmnidriveDevice::IsRangeCachedLocked(uint64_t sector_start,
                                              uint32_t sector_count) const {
  if (sector_count == 0) {
    return true;
  }
  std::vector<uint8_t> coverage(sector_count, 0);
  for (const auto& chunk : read_cache_) {
    const uint64_t chunk_end = chunk.sector_start + chunk.sector_count;
    const uint64_t range_end = sector_start + sector_count;
    const uint64_t overlap_start = std::max(sector_start, chunk.sector_start);
    const uint64_t overlap_end = std::min(range_end, chunk_end);
    if (overlap_start >= overlap_end) {
      continue;
    }
    for (uint64_t i = overlap_start; i < overlap_end; ++i) {
      coverage[static_cast<size_t>(i - sector_start)] = 1;
    }
  }
  return std::all_of(coverage.begin(), coverage.end(),
                     [](uint8_t value) { return value != 0; });
}

void DiscOmnidriveDevice::InsertCacheRange(
    uint64_t sector_start, uint32_t sector_count,
    std::span<const uint8_t> data) const {
  if (sector_count == 0 || data.size() != static_cast<size_t>(sector_count) *
                                              physical_sector_size()) {
    return;
  }

  CachedReadRange chunk;
  chunk.sector_start = sector_start;
  chunk.sector_count = sector_count;
  chunk.data.assign(data.begin(), data.end());

  std::lock_guard<std::mutex> lock(cache_mutex_);
  chunk.use_tick = ++cache_use_tick_;
  read_cache_bytes_ += chunk.data.size();
  read_cache_.emplace_back(std::move(chunk));
  while (read_cache_bytes_ > kReadCacheMaxBytes && !read_cache_.empty()) {
    read_cache_bytes_ -= read_cache_.front().data.size();
    read_cache_.pop_front();
  }
}

void DiscOmnidriveDevice::TryScheduleSequentialPrefetch(
    uint64_t current_end_sector) const {
  const uint64_t prefetch_start = current_end_sector + 1;

  if (pending_demand_reads_.load(std::memory_order_relaxed) != 0) {
    const uint64_t deferred = read_telemetry_.prefetch_deferred.fetch_add(
                                  1, std::memory_order_relaxed) +
                              1;
    if (deferred == 1 || (deferred % 128) == 0) {
      XELOGD(
          "DiscOmnidriveDevice::TryScheduleSequentialPrefetch: deferred "
          "prefetch due to pending demand reads (pending={} deferred={})",
          pending_demand_reads_.load(std::memory_order_relaxed), deferred);
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (IsRangeCachedLocked(prefetch_start, kSequentialPrefetchSectors)) {
      read_telemetry_.prefetch_hits.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }

  read_telemetry_.prefetch_requests.fetch_add(1, std::memory_order_relaxed);
  if (!EnsureReadWorkerStarted()) {
    read_telemetry_.prefetch_misses.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(read_worker_mutex_);
    const uint64_t pending_demand =
        pending_demand_reads_.load(std::memory_order_relaxed);
    if (pending_demand != 0 || !read_worker_demand_tasks_.empty()) {
      const uint64_t deferred = read_telemetry_.prefetch_deferred.fetch_add(
                                    1, std::memory_order_relaxed) +
                                1;
      if (deferred == 1 || (deferred % 128) == 0) {
        XELOGD(
            "DiscOmnidriveDevice::TryScheduleSequentialPrefetch: deferred "
            "prefetch due to worker pressure (pending={} demand_queued={} "
            "deferred={})",
            pending_demand, read_worker_demand_tasks_.size(), deferred);
      }
      return;
    }

    PendingPrefetchRequest incoming{prefetch_start, kSequentialPrefetchSectors};
    if (!pending_prefetch_request_.has_value()) {
      pending_prefetch_request_ = incoming;
    } else {
      PendingPrefetchRequest& pending = *pending_prefetch_request_;
      const uint64_t pending_end =
          pending.sector_start + pending.sector_count - 1;
      const uint64_t incoming_end =
          incoming.sector_start + incoming.sector_count - 1;
      if (incoming.sector_start == pending_end + 1) {
        pending.sector_count += incoming.sector_count;
        read_telemetry_.prefetch_coalesced.fetch_add(1,
                                                     std::memory_order_relaxed);
      } else if (incoming.sector_start > pending_end + 1) {
        pending_prefetch_request_ = incoming;
        read_telemetry_.prefetch_superseded.fetch_add(
            1, std::memory_order_relaxed);
      } else if (incoming_end > pending_end) {
        pending.sector_count =
            static_cast<uint32_t>(incoming_end - pending.sector_start + 1);
        read_telemetry_.prefetch_coalesced.fetch_add(1,
                                                     std::memory_order_relaxed);
      } else {
        read_telemetry_.prefetch_superseded.fetch_add(
            1, std::memory_order_relaxed);
      }
    }
  }
  read_worker_cv_.notify_one();
}

bool DiscOmnidriveDevice::ExecutePrefetchRead(uint64_t sector_start,
                                              uint32_t sector_count) const {
  if (sector_count == 0) {
    return true;
  }

  if (pending_demand_reads_.load(std::memory_order_relaxed) != 0) {
    const uint64_t skipped = read_telemetry_.prefetch_skipped.fetch_add(
                                 1, std::memory_order_relaxed) +
                             1;
    const uint64_t deferred = read_telemetry_.prefetch_deferred.fetch_add(
                                  1, std::memory_order_relaxed) +
                              1;
    if (skipped == 1 || (skipped % 128) == 0) {
      XELOGD(
          "DiscOmnidriveDevice::ExecutePrefetchRead: skipping prefetch due to "
          "pending demand (pending={} skipped={} deferred={})",
          pending_demand_reads_.load(std::memory_order_relaxed), skipped,
          deferred);
    }
    return false;
  }

  if (sector_start > (std::numeric_limits<uint32_t>::max)()) {
    return false;
  }
  if (sector_count > (std::numeric_limits<uint32_t>::max)() -
                         static_cast<uint32_t>(sector_start)) {
    return false;
  }

  std::vector<uint8_t> prefetch_buffer(static_cast<size_t>(sector_count) *
                                       physical_sector_size());
  const bool success = ReadOmniDriveBlocks(
      static_cast<uint32_t>(sector_start), sector_count, false, false, true,
      OmniDriveSubchannel::kNone, false, prefetch_buffer.data(),
      prefetch_buffer.size(), nullptr);
  if (!success) {
    return false;
  }

  std::span<uint8_t> prefetch_span{prefetch_buffer.data(),
                                   prefetch_buffer.size()};
  InsertCacheRange(sector_start, sector_count, prefetch_span);
  read_telemetry_.prefetch_exec_count.fetch_add(1, std::memory_order_relaxed);

  const uint64_t current_end = sector_start + sector_count - 1;
  const bool had_last = has_last_prefetch_exec_.load(std::memory_order_relaxed);
  if (!had_last) {
    read_telemetry_.prefetch_monotonic_ok.fetch_add(1,
                                                    std::memory_order_relaxed);
  } else {
    const uint64_t last_end =
        last_prefetch_exec_end_.load(std::memory_order_relaxed);
    if (sector_start == last_end + 1) {
      read_telemetry_.prefetch_monotonic_ok.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      read_telemetry_.prefetch_monotonic_breaks.fetch_add(
          1, std::memory_order_relaxed);
    }
  }
  last_prefetch_exec_start_.store(sector_start, std::memory_order_relaxed);
  last_prefetch_exec_end_.store(current_end, std::memory_order_relaxed);
  has_last_prefetch_exec_.store(true, std::memory_order_relaxed);
  return true;
}

void DiscOmnidriveDevice::RecordDiskIssue(uint64_t start_sector,
                                          uint32_t sector_count) const {
  if (sector_count == 0) {
    return;
  }
  const uint64_t issue_end_sector =
      start_sector + static_cast<uint64_t>(sector_count) - 1;
  read_telemetry_.disk_issue_total.fetch_add(1, std::memory_order_relaxed);
  const uint64_t previous_end = read_telemetry_.disk_issue_last_end.exchange(
      issue_end_sector, std::memory_order_relaxed);
  read_telemetry_.disk_issue_last_start.store(start_sector,
                                              std::memory_order_relaxed);
  if (previous_end == kInvalidIssueSector) {
    return;
  }
  if (start_sector == previous_end + 1) {
    read_telemetry_.disk_issue_contiguous.fetch_add(1,
                                                    std::memory_order_relaxed);
    return;
  }
  if (start_sector > previous_end + 1) {
    read_telemetry_.disk_issue_forward_gap.fetch_add(1,
                                                     std::memory_order_relaxed);
    const uint64_t gap = start_sector - (previous_end + 1);
    uint64_t previous_largest =
        read_telemetry_.disk_issue_largest_forward_gap.load(
            std::memory_order_relaxed);
    while (
        gap > previous_largest &&
        !read_telemetry_.disk_issue_largest_forward_gap.compare_exchange_weak(
            previous_largest, gap, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
    }
    return;
  }
  read_telemetry_.disk_issue_backward.fetch_add(1, std::memory_order_relaxed);
}

void DiscOmnidriveDevice::MaybeLogReadTelemetry() const {
  const uint64_t request_count =
      read_telemetry_.total_requests.load(std::memory_order_relaxed);
  if (request_count == 1 || (request_count % 128) == 0) {
    XELOGD(
        "DiscOmnidriveDevice::ReadDiskBytesAsync: requests={} async={} "
        "cache_hits={} cache_misses={} prefetch_req={} prefetch_hits={} "
        "prefetch_misses={} prefetch_deferred={} prefetch_skipped={} "
        "prefetch_exec_count={} prefetch_monotonic_ok={} "
        "prefetch_monotonic_breaks={} prefetch_coalesced={} "
        "prefetch_superseded={} prefetch_last_start={} "
        "prefetch_last_end={} pending_prefetch_start={} "
        "pending_prefetch_count={} demand_q={} demand_q_max={} "
        "failures={} disk_issue_total={} disk_issue_contiguous={} "
        "disk_issue_forward_gap={} disk_issue_backward={} "
        "disk_issue_largest_forward_gap={} disk_issue_last_start={} "
        "disk_issue_last_end={}",
        request_count,
        read_telemetry_.async_dispatches.load(std::memory_order_relaxed),
        read_telemetry_.cache_hits.load(std::memory_order_relaxed),
        read_telemetry_.cache_misses.load(std::memory_order_relaxed),
        read_telemetry_.prefetch_requests.load(std::memory_order_relaxed),
        read_telemetry_.prefetch_hits.load(std::memory_order_relaxed),
        read_telemetry_.prefetch_misses.load(std::memory_order_relaxed),
        read_telemetry_.prefetch_deferred.load(std::memory_order_relaxed),
        read_telemetry_.prefetch_skipped.load(std::memory_order_relaxed),
        read_telemetry_.prefetch_exec_count.load(std::memory_order_relaxed),
        read_telemetry_.prefetch_monotonic_ok.load(std::memory_order_relaxed),
        read_telemetry_.prefetch_monotonic_breaks.load(
            std::memory_order_relaxed),
        read_telemetry_.prefetch_coalesced.load(std::memory_order_relaxed),
        read_telemetry_.prefetch_superseded.load(std::memory_order_relaxed),
        has_last_prefetch_exec_.load(std::memory_order_relaxed)
            ? last_prefetch_exec_start_.load(std::memory_order_relaxed)
            : 0,
        has_last_prefetch_exec_.load(std::memory_order_relaxed)
            ? last_prefetch_exec_end_.load(std::memory_order_relaxed)
            : 0,
        [&]() -> uint64_t {
          std::lock_guard<std::mutex> lock(read_worker_mutex_);
          return pending_prefetch_request_.has_value()
                     ? pending_prefetch_request_->sector_start
                     : 0;
        }(),
        [&]() -> uint32_t {
          std::lock_guard<std::mutex> lock(read_worker_mutex_);
          return pending_prefetch_request_.has_value()
                     ? pending_prefetch_request_->sector_count
                     : 0;
        }(),
        queued_demand_tasks_.load(std::memory_order_relaxed),
        read_telemetry_.demand_queue_max_depth.load(std::memory_order_relaxed),
        read_telemetry_.failed_reads.load(std::memory_order_relaxed),
        read_telemetry_.disk_issue_total.load(std::memory_order_relaxed),
        read_telemetry_.disk_issue_contiguous.load(std::memory_order_relaxed),
        read_telemetry_.disk_issue_forward_gap.load(std::memory_order_relaxed),
        read_telemetry_.disk_issue_backward.load(std::memory_order_relaxed),
        read_telemetry_.disk_issue_largest_forward_gap.load(
            std::memory_order_relaxed),
        [&]() -> uint64_t {
          const uint64_t value = read_telemetry_.disk_issue_last_start.load(
              std::memory_order_relaxed);
          return value == kInvalidIssueSector ? 0 : value;
        }(),
        [&]() -> uint64_t {
          const uint64_t value = read_telemetry_.disk_issue_last_end.load(
              std::memory_order_relaxed);
          return value == kInvalidIssueSector ? 0 : value;
        }());
  }
}

bool DiscOmnidriveDevice::ReadDiskBytesAsync(size_t offset,
                                             std::span<uint8_t> buffer) const {
  if (buffer.empty()) {
    return true;
  }

  if (!path_log_emitted_.exchange(true, std::memory_order_relaxed)) {
    XELOGI(
        "DiscOmnidriveDevice::ReadDiskBytesAsync: using async Omnidrive "
        "transport path for {}",
        host_path_.string());
  }

  read_telemetry_.total_requests.fetch_add(1, std::memory_order_relaxed);

  if (offset > (std::numeric_limits<uint64_t>::max)() - buffer.size()) {
    read_telemetry_.failed_reads.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const uint64_t end_offset = static_cast<uint64_t>(offset) + buffer.size() - 1;

  const uint64_t first_sector = offset / physical_sector_size();
  const uint64_t last_sector = end_offset / physical_sector_size();
  const uint64_t sector_count_u64 = last_sector - first_sector + 1;
  if (sector_count_u64 > (std::numeric_limits<uint32_t>::max)()) {
    read_telemetry_.failed_reads.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t sector_count = static_cast<uint32_t>(sector_count_u64);

  std::vector<uint8_t> aligned(static_cast<size_t>(sector_count) *
                               physical_sector_size());
  std::vector<uint8_t> covered(sector_count, 0);

  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    for (auto& chunk : read_cache_) {
      const uint64_t chunk_end = chunk.sector_start + chunk.sector_count;
      const uint64_t overlap_start = std::max(first_sector, chunk.sector_start);
      const uint64_t overlap_end = std::min(last_sector + 1, chunk_end);
      if (overlap_start >= overlap_end) {
        continue;
      }

      const size_t src_offset =
          static_cast<size_t>(overlap_start - chunk.sector_start) *
          physical_sector_size();
      const size_t dst_offset =
          static_cast<size_t>(overlap_start - first_sector) *
          physical_sector_size();
      const size_t copy_size =
          static_cast<size_t>(overlap_end - overlap_start) *
          physical_sector_size();

      std::memcpy(aligned.data() + dst_offset, chunk.data.data() + src_offset,
                  copy_size);
      for (uint64_t sector = overlap_start; sector < overlap_end; ++sector) {
        covered[static_cast<size_t>(sector - first_sector)] = 1;
      }
      chunk.use_tick = ++cache_use_tick_;
    }
  }

  size_t covered_count = 0;
  for (uint8_t bit : covered) {
    covered_count += bit ? 1 : 0;
  }
  read_telemetry_.cache_hits.fetch_add(covered_count,
                                       std::memory_order_relaxed);

  bool read_failed = false;
  size_t i = 0;
  while (i < covered.size()) {
    if (covered[i]) {
      ++i;
      continue;
    }
    const size_t run_start = i;
    while (i < covered.size() && !covered[i]) {
      ++i;
    }
    const size_t run_end = i;
    const uint64_t sector_start = first_sector + run_start;
    const uint32_t run_sector_count =
        static_cast<uint32_t>(run_end - run_start);
    uint8_t* run_buffer =
        aligned.data() +
        run_start * static_cast<size_t>(physical_sector_size());
    if (!ReadSectorsBlocking(sector_start, run_sector_count, run_buffer)) {
      read_failed = true;
      break;
    }

    read_telemetry_.cache_misses.fetch_add(run_sector_count,
                                           std::memory_order_relaxed);
    InsertCacheRange(sector_start, run_sector_count,
                     std::span<const uint8_t>(
                         run_buffer, static_cast<size_t>(run_sector_count) *
                                         physical_sector_size()));
  }

  if (read_failed) {
    read_telemetry_.failed_reads.fetch_add(1, std::memory_order_relaxed);
    XELOGE(
        "DiscOmnidriveDevice::ReadDiskBytesAsync: transport read failed at "
        "offset={} length={} on {}",
        offset, buffer.size(), host_path_.string());
    return false;
  }

  const size_t within_sector_offset =
      static_cast<size_t>(offset % physical_sector_size());
  std::memcpy(buffer.data(), aligned.data() + within_sector_offset,
              buffer.size());

  bool sequential_read = false;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    sequential_read =
        last_read_valid_ && first_sector == last_read_end_sector_ + 1;
    last_read_valid_ = true;
    last_read_end_sector_ = last_sector;
  }
  if (sequential_read) {
    TryScheduleSequentialPrefetch(last_sector);
  }

  MaybeLogReadTelemetry();
  return true;
}

bool DiscOmnidriveDevice::InitializePhysicalTransport() {
#if XE_PLATFORM_LINUX
  is_physical_device_path_ = IsPlausibleLinuxScsiPath(host_path_);
  if (physical_transport_fd_ >= 0) {
    has_physical_transport_ = true;
    return true;
  }

  if (!is_physical_device_path_) {
    XELOGI(
        "DiscOmnidriveDevice::InitializePhysicalTransport: {} is not a "
        "plausible /dev sg/sr node",
        host_path_.string());
    has_physical_transport_ = false;
    return false;
  }

  struct stat st = {};
  const std::string path = host_path_.string();
  if (stat(path.c_str(), &st) != 0) {
    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalTransport: stat failed for {}: "
        "{}",
        path, std::strerror(errno));
    has_physical_transport_ = false;
    return false;
  }
  if (!S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode)) {
    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalTransport: {} is not a block "
        "or char device",
        path);
    has_physical_transport_ = false;
    return false;
  }

  physical_transport_fd_ = open(path.c_str(), O_RDWR | O_NONBLOCK);
  if (physical_transport_fd_ < 0) {
    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalTransport: open(O_RDWR) "
        "failed for {}: {}; retrying O_RDONLY",
        path, std::strerror(errno));
    physical_transport_fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK);
  }
  if (physical_transport_fd_ < 0) {
    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalTransport: open failed for {}: "
        "{}",
        path, std::strerror(errno));
    has_physical_transport_ = false;
    return false;
  }

  has_physical_transport_ = true;
  XELOGI("DiscOmnidriveDevice: enabled physical transport for {}", path);
  return true;
#elif XE_PLATFORM_WIN32
  is_physical_device_path_ = IsPlausibleWindowsCdromPath(host_path_);
  if (physical_transport_handle_ != INVALID_HANDLE_VALUE) {
    has_physical_transport_ = true;
    return true;
  }

  running_under_wine_ = xe::IsRunningUnderWine();

  std::filesystem::path path_to_open = host_path_;
  if (running_under_wine_ && IsPlausibleLinuxScsiPath(host_path_)) {
    path_to_open = TranslateWineLinuxPath(host_path_);
    HANDLE handle =
        CreateFile(path_to_open.c_str(), GENERIC_READ | GENERIC_WRITE,
                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      physical_transport_handle_ = handle;
      has_physical_transport_ = true;
      physical_transport_backend_ = PhysicalTransportBackend::kLinuxStyleFile;
      XELOGI(
          "DiscOmnidriveDevice: running under Wine, enabled Linux-style "
          "transport for {} via {}",
          host_path_.string(), path_to_open.string());
      return true;
    }

    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalTransport: Wine Linux-style "
        "open failed for {} (translated to {}), falling back to SPTI",
        host_path_.string(), path_to_open.string());
  }

  if (!is_physical_device_path_) {
    XELOGI(
        "DiscOmnidriveDevice::InitializePhysicalTransport: {} is not a "
        "plausible Windows optical drive path",
        host_path_.string());
    has_physical_transport_ = false;
    physical_transport_backend_ = PhysicalTransportBackend::kNone;
    return false;
  }

  HANDLE handle = CreateFile(host_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalTransport: open failed for "
        "{}: error {}",
        host_path_.string(), GetLastError());
    has_physical_transport_ = false;
    physical_transport_backend_ = PhysicalTransportBackend::kNone;
    return false;
  }

  physical_transport_handle_ = handle;
  has_physical_transport_ = true;
  physical_transport_backend_ = PhysicalTransportBackend::kWinSpti;
  if (!TryAcquireWindowsExclusiveAccess()) {
    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalTransport: continuing in "
        "shared mode for {} (exclusive lock unavailable)",
        host_path_.string());
  }
  XELOGI("DiscOmnidriveDevice: enabled Windows SPTI transport for {}",
         host_path_.string());
  return true;
#else
  XELOGE(
      "DiscOmnidriveDevice::InitializePhysicalTransport: OmniDrive physical "
      "transport is only supported on Linux and Windows ({})",
      host_path_.string());
  return false;
#endif  // XE_PLATFORM_LINUX
}

bool DiscOmnidriveDevice::InitializePhysicalDriveStateOnce() const {
#if XE_PLATFORM_LINUX
  return InitializePhysicalDriveStateOnceLinux();
#elif XE_PLATFORM_WIN32
  return InitializePhysicalDriveStateOnceWindows();
#else
  media_inserted_ = false;
  return true;
#endif  // XE_PLATFORM_LINUX
}

bool DiscOmnidriveDevice::ProbeOnly() {
  if (!InitializePhysicalTransport()) {
    return false;
  }
  return InitializePhysicalDriveStateOnce();
}

std::vector<std::filesystem::path>
DiscOmnidriveDevice::EnumerateCandidateOpticalDrivePaths() {
  std::vector<std::filesystem::path> result;
#if XE_PLATFORM_LINUX
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/dev", ec)) {
    if (ec) {
      break;
    }
    const std::string name = entry.path().filename().string();
    if (StartsWith(name, "sg")) {
      result.push_back(entry.path());
    }
  }
  std::sort(result.begin(), result.end());
#elif XE_PLATFORM_WIN32
  const DWORD drive_mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(drive_mask & (1u << i))) {
      continue;
    }
    const wchar_t drive_letter = static_cast<wchar_t>(L'A' + i);
    std::wstring root = std::wstring(1, drive_letter) + L":\\";
    if (GetDriveTypeW(root.c_str()) == DRIVE_CDROM) {
      std::wstring device_path =
          L"\\\\.\\" + std::wstring(1, drive_letter) + L":";
      result.push_back(std::filesystem::path(device_path));
    }
  }
#endif  // XE_PLATFORM_LINUX
  return result;
}

std::vector<OmnidriveDriveInfo> DiscOmnidriveDevice::ProbeAllCandidateDrives() {
  std::vector<OmnidriveDriveInfo> result;
  for (const auto& path : EnumerateCandidateOpticalDrivePaths()) {
    DiscOmnidriveDevice probe_device("\\Device\\OmnidriveProbeTransient", path,
                                     OmniDriveDiscType::kDVD, false);
    if (!probe_device.ProbeOnly()) {
      continue;
    }
    OmnidriveDriveInfo info;
    info.path = path;
    info.omnidrive_confirmed = probe_device.omnidrive_firmware_confirmed();
    info.media_inserted = probe_device.media_inserted();
    info.omnidrive_version.major = probe_device.omnidrive_version()->major;
    info.omnidrive_version.minor = probe_device.omnidrive_version()->minor;
    info.omnidrive_version.build = probe_device.omnidrive_version()->build;
    info.identity_summary = probe_device.omnidrive_identity_summary();
    result.push_back(std::move(info));
  }
  return result;
}

DiscOmnidriveDevice::InquiryProbeResult
DiscOmnidriveDevice::ProbeFirmwareIdentityByInquiry() const {
  InquiryProbeResult result;
  if (!has_physical_transport_) {
    return result;
  }

  // Must be at least 104 bytes (sizeof INQUIRY_StandardData) so that the
  // `reserved5` field at offset 74, where OmniDrive firmware embeds its
  // "OmniDrive" marker string and version, is actually read.
  uint8_t inquiry_data[104] = {0};

#if XE_PLATFORM_LINUX
  if (physical_transport_fd_ < 0) {
    return result;
  }

  const uint8_t cdb_inquiry[6] = {0x12, 0, 0, 0, sizeof(inquiry_data), 0};
  if (!RunLinuxScsiCommand(physical_transport_fd_, "INQUIRY (standard)",
                           cdb_inquiry, sizeof(cdb_inquiry), SG_DXFER_FROM_DEV,
                           inquiry_data, sizeof(inquiry_data))) {
    XELOGW(
        "DiscOmnidriveDevice::ProbeFirmwareIdentityByInquiry: INQUIRY failed "
        "for {}",
        host_path_.string());
    return result;
  }
#elif XE_PLATFORM_WIN32
  if (physical_transport_handle_ == INVALID_HANDLE_VALUE ||
      physical_transport_backend_ != PhysicalTransportBackend::kWinSpti) {
    return result;
  }

  HANDLE handle = static_cast<HANDLE>(physical_transport_handle_);
  const uint8_t cdb_inquiry[6] = {0x12, 0, 0, 0, sizeof(inquiry_data), 0};

  SptdWithSense sptd = {};
  sptd.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
  sptd.sptd.CdbLength = sizeof(cdb_inquiry);
  sptd.sptd.SenseInfoLength = sizeof(sptd.sense_buffer);
  sptd.sptd.DataIn = SCSI_IOCTL_DATA_IN;
  sptd.sptd.DataTransferLength = sizeof(inquiry_data);
  sptd.sptd.TimeOutValue = 30;
  sptd.sptd.DataBuffer = inquiry_data;
  sptd.sptd.SenseInfoOffset = offsetof(SptdWithSense, sense_buffer);
  std::memcpy(sptd.sptd.Cdb, cdb_inquiry, sizeof(cdb_inquiry));

  DWORD returned = 0;
  if (!DeviceIoControl(handle, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd,
                       sizeof(sptd), &sptd, sizeof(sptd), &returned, nullptr)) {
    XELOGW(
        "DiscOmnidriveDevice::ProbeFirmwareIdentityByInquiry: SPTI INQUIRY "
        "failed for {}: error {}",
        host_path_.string(), GetLastError());
    return result;
  }

  if (sptd.sptd.ScsiStatus != 0) {
    XELOGW(
        "DiscOmnidriveDevice::ProbeFirmwareIdentityByInquiry: SPTI INQUIRY "
        "status=0x{:02X} for {}",
        sptd.sptd.ScsiStatus, host_path_.string());
    return result;
  }
#else
  return result;
#endif  // XE_PLATFORM_LINUX

  const InquiryIdentityDetection detection =
      ParseOmnidriveInquiryIdentity(inquiry_data, sizeof(inquiry_data));

  result.omnidrive_detected = detection.omnidrive_confirmed;
  result.omnidrive_version.major = detection.omnidrive_version.major;
  result.omnidrive_version.minor = detection.omnidrive_version.minor;
  result.omnidrive_version.build = detection.omnidrive_version.build;
  if (result.omnidrive_detected) {
    XELOGI(
        "DiscOmnidriveDevice::ProbeFirmwareIdentityByInquiry: OmniDrive "
        "firmware marker detected for {} (version v{}.{}.{})",
        host_path_.string(), result.omnidrive_version.major,
        result.omnidrive_version.minor, result.omnidrive_version.build);
    if ((static_cast<uint32_t>(result.omnidrive_version.major) << 16 |
         static_cast<uint32_t>(result.omnidrive_version.minor) << 8 |
         static_cast<uint32_t>(result.omnidrive_version.build)) <
        (static_cast<uint32_t>(kOmnidriveMinimumFirmwareVersion.major) << 16 |
         static_cast<uint32_t>(kOmnidriveMinimumFirmwareVersion.minor) << 8 |
         static_cast<uint32_t>(kOmnidriveMinimumFirmwareVersion.build))) {
      XELOGW(
          "DiscOmnidriveDevice::ProbeFirmwareIdentityByInquiry: OmniDrive "
          "firmware version v{}.{}.{} for {} is below the minimum "
          "supported version v{}.{}.{}",
          result.omnidrive_version.major, result.omnidrive_version.minor,
          result.omnidrive_version.build, host_path_.string(),
          kOmnidriveMinimumFirmwareVersion.major,
          kOmnidriveMinimumFirmwareVersion.minor,
          kOmnidriveMinimumFirmwareVersion.build);
    }
  }
  result.vendor = TrimInquiryField(inquiry_data + 8, 8);
  result.product = TrimInquiryField(inquiry_data + 16, 16);
  result.revision = TrimInquiryField(inquiry_data + 32, 4);
  XELOGI(
      "DiscOmnidriveDevice::ProbeFirmwareIdentityByInquiry: "
      "omnidrive_detected={} v{}.{}.{} for {} "
      "(vendor='{}' product='{}' revision='{}')",
      result.omnidrive_detected, result.omnidrive_version.major,
      result.omnidrive_version.minor, result.omnidrive_version.build,
      host_path_.string(), result.vendor, result.product, result.revision);
  return result;
}

#if XE_PLATFORM_LINUX
bool DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux() const {
  if (physical_drive_state_attempted_) {
    return physical_drive_state_ready_;
  }
  physical_drive_state_attempted_ = true;
  physical_drive_state_ready_ = false;
  physical_drive_state_failed_command_ = "none";
  total_sector_units_ = 0;
  total_sector_units_available_ = false;
  total_sector_units_ = 0;
  total_sector_units_available_ = false;

  if (!has_physical_transport_ || physical_transport_fd_ < 0) {
    return false;
  }

  const uint8_t cdb_test_unit_ready[6] = {0x00, 0, 0, 0, 0, 0};
  uint8_t tur_sense_key = 0, tur_asc = 0, tur_ascq = 0;
  const bool tur_ok = RunLinuxScsiCommand(
      physical_transport_fd_, "TEST UNIT READY", cdb_test_unit_ready,
      sizeof(cdb_test_unit_ready), SG_DXFER_NONE, nullptr, 0, 8000,
      &tur_sense_key, &tur_asc, &tur_ascq);
  if (!tur_ok) {
    constexpr uint8_t kSenseKeyNotReady = 0x02;
    constexpr uint8_t kAscMediumNotPresent = 0x3A;
    if (tur_sense_key == kSenseKeyNotReady && tur_asc == kAscMediumNotPresent) {
      // Drive is present/working, just has no disc inserted. Do NOT treat
      // this as a preflight failure — distinct from "drive unusable".
      media_inserted_ = false;
      XELOGI(
          "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
          "no media present for {} (NOT READY/MEDIUM NOT PRESENT)",
          host_path_.string());
    } else {
      physical_drive_state_failed_command_ = "TEST UNIT READY";
      XELOGE(
          "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
          "physical preflight failed for {} at required command {} "
          "(sense_key=0x{:02X} asc=0x{:02X} ascq=0x{:02X})",
          host_path_.string(), physical_drive_state_failed_command_,
          tur_sense_key, tur_asc, tur_ascq);
      return false;
    }
  } else {
    media_inserted_ = true;
  }

  uint8_t inquiry_data[36] = {0};
  const uint8_t cdb_inquiry[6] = {0x12, 0, 0, 0, sizeof(inquiry_data), 0};
  if (!RunLinuxScsiCommand(physical_transport_fd_, "INQUIRY (standard)",
                           cdb_inquiry, sizeof(cdb_inquiry), SG_DXFER_FROM_DEV,
                           inquiry_data, sizeof(inquiry_data))) {
    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
        "noncritical preflight command failed: INQUIRY (standard)");
  }

  const InquiryProbeResult firmware_probe = ProbeFirmwareIdentityByInquiry();
  omnidrive_firmware_confirmed_ = firmware_probe.omnidrive_detected;
  omnidrive_version_.major = firmware_probe.omnidrive_version.major;
  omnidrive_version_.minor = firmware_probe.omnidrive_version.minor;
  omnidrive_version_.build = firmware_probe.omnidrive_version.build;
  identity_summary_ =
      fmt::format("{} {} rev{}{}", firmware_probe.vendor,
                  firmware_probe.product, firmware_probe.revision,
                  firmware_probe.omnidrive_detected
                      ? fmt::format(" (OmniDrive v{}.{}.{})",
                                    firmware_probe.omnidrive_version.major,
                                    firmware_probe.omnidrive_version.minor,
                                    firmware_probe.omnidrive_version.build)
                      : std::string());
  XELOGI(
      "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
      "omnidrive_firmware_confirmed={} for {}",
      omnidrive_firmware_confirmed_, host_path_.string());

  if (!media_inserted_) {
    // No media present, but the drive itself is fine — INQUIRY/firmware
    // detection works without a disc, but READ CAPACITY / READ DISC
    // STRUCTURE below genuinely require media and would fail pointlessly.
    physical_drive_state_ready_ = true;
    XELOGI(
        "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
        "physical preflight succeeded for {} (no media present)",
        host_path_.string());
    return true;
  }

  uint8_t read_capacity_data[8] = {0};
  const uint8_t cdb_read_capacity[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  if (!RunLinuxScsiCommand(physical_transport_fd_, "READ CAPACITY (10)",
                           cdb_read_capacity, sizeof(cdb_read_capacity),
                           SG_DXFER_FROM_DEV, read_capacity_data,
                           sizeof(read_capacity_data))) {
    physical_drive_state_failed_command_ = "READ CAPACITY (10)";
    XELOGE(
        "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
        "physical preflight failed for {} at required command {}",
        host_path_.string(), physical_drive_state_failed_command_);
    return false;
  }

  const uint32_t last_lba =
      (static_cast<uint32_t>(read_capacity_data[0]) << 24) |
      (static_cast<uint32_t>(read_capacity_data[1]) << 16) |
      (static_cast<uint32_t>(read_capacity_data[2]) << 8) |
      static_cast<uint32_t>(read_capacity_data[3]);
  if (last_lba != 0xFFFFFFFFu) {
    total_sector_units_ = last_lba + 1;
    total_sector_units_available_ = true;
  }

  uint8_t disc_structure_data[256] = {0};
  const uint16_t alloc_length =
      static_cast<uint16_t>(sizeof(disc_structure_data));
  const uint8_t cdb_read_disc_structure[12] = {
      0xAD,
      0,
      0,
      0,
      0,
      0,
      0,
      0x00,
      static_cast<uint8_t>((alloc_length >> 8) & 0xFF),
      static_cast<uint8_t>(alloc_length & 0xFF),
      0,
      0,
  };
  const bool read_disc_structure_ok = RunLinuxScsiCommand(
      physical_transport_fd_, "READ DISC STRUCTURE (physical format, layer 0)",
      cdb_read_disc_structure, sizeof(cdb_read_disc_structure),
      SG_DXFER_FROM_DEV, disc_structure_data, sizeof(disc_structure_data));
  if (!read_disc_structure_ok) {
    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
        "noncritical preflight command failed: READ DISC STRUCTURE");
    physical_drive_state_ready_ = true;
    XELOGI(
        "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
        "physical preflight succeeded for {}",
        host_path_.string());
    return true;
  }

  const uint16_t payload_length = static_cast<uint16_t>(
      (static_cast<uint16_t>(disc_structure_data[0]) << 8) |
      static_cast<uint16_t>(disc_structure_data[1]));
  const size_t total_response_length = static_cast<size_t>(payload_length) + 2;
  if (total_response_length < 12 ||
      total_response_length > sizeof(disc_structure_data)) {
    XELOGW(
        "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: READ "
        "DISC STRUCTURE returned unsupported payload length {}",
        total_response_length);
    physical_drive_state_ready_ = true;
    XELOGI(
        "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
        "physical preflight succeeded for {}",
        host_path_.string());
    return true;
  }

  if (!layer0_last_available_) {
    const uint32_t layer0_end_sector =
        (static_cast<uint32_t>(disc_structure_data[9]) << 16) |
        (static_cast<uint32_t>(disc_structure_data[10]) << 8) |
        static_cast<uint32_t>(disc_structure_data[11]);
    layer0_last_ = layer0_end_sector;
    layer0_last_available_ = true;
    layer0_last_auto_discovered_ = true;
    XELOGI(
        "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
        "auto-discovered layer0_last={} from DVD physical descriptor",
        layer0_last_);
  }

  physical_drive_state_ready_ = true;
  XELOGI(
      "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceLinux: "
      "physical preflight succeeded for {}",
      host_path_.string());
  return true;
}
#endif  // XE_PLATFORM_LINUX

#if XE_PLATFORM_WIN32
bool DiscOmnidriveDevice::TryAcquireWindowsExclusiveAccess() {
  if (physical_transport_handle_ == INVALID_HANDLE_VALUE ||
      physical_transport_backend_ != PhysicalTransportBackend::kWinSpti) {
    return false;
  }

  if (windows_exclusive_access_held_) {
    windows_exclusive_access_attempted_ = true;
    windows_exclusive_access_last_error_ = 0;
    return true;
  }

  windows_exclusive_access_attempted_ = true;

  CDROM_EXCLUSIVE_LOCK lock_request = {};
  lock_request.Access.RequestType = ExclusiveAccessLockDevice;
  lock_request.Access.Flags = CDROM_LOCK_IGNORE_VOLUME;

  constexpr const char kCallerName[] = "XeniaOmniDrive";
  const size_t max_caller_chars = sizeof(lock_request.CallerName);
  const size_t caller_chars_to_copy =
      std::min(sizeof(kCallerName) - 1, max_caller_chars - 1);
  std::memset(lock_request.CallerName, 0, max_caller_chars);
  std::memcpy(lock_request.CallerName, kCallerName, caller_chars_to_copy);

  DWORD bytes_returned = 0;
  const bool lock_ok = DeviceIoControl(
      static_cast<HANDLE>(physical_transport_handle_),
      IOCTL_CDROM_EXCLUSIVE_ACCESS, &lock_request, sizeof(lock_request),
      nullptr, 0, &bytes_returned, nullptr);
  if (lock_ok) {
    windows_exclusive_access_held_ = true;
    windows_exclusive_access_last_error_ = 0;
    XELOGI(
        "DiscOmnidriveDevice::TryAcquireWindowsExclusiveAccess: acquired "
        "exclusive access for {}",
        host_path_.string());
    return true;
  }

  windows_exclusive_access_held_ = false;
  windows_exclusive_access_last_error_ = GetLastError();
  XELOGW(
      "DiscOmnidriveDevice::TryAcquireWindowsExclusiveAccess: failed to "
      "acquire exclusive access for {}: error {}",
      host_path_.string(), windows_exclusive_access_last_error_);
  return false;
}

void DiscOmnidriveDevice::ReleaseWindowsExclusiveAccess() {
  if (!windows_exclusive_access_held_ ||
      physical_transport_handle_ == INVALID_HANDLE_VALUE ||
      physical_transport_backend_ != PhysicalTransportBackend::kWinSpti) {
    return;
  }

  CDROM_EXCLUSIVE_ACCESS unlock_request = {};
  unlock_request.RequestType = ExclusiveAccessUnlockDevice;
  unlock_request.Flags = 0;

  DWORD bytes_returned = 0;
  if (!DeviceIoControl(static_cast<HANDLE>(physical_transport_handle_),
                       IOCTL_CDROM_EXCLUSIVE_ACCESS, &unlock_request,
                       sizeof(unlock_request), nullptr, 0, &bytes_returned,
                       nullptr)) {
    windows_exclusive_access_last_error_ = GetLastError();
    XELOGW(
        "DiscOmnidriveDevice::ReleaseWindowsExclusiveAccess: failed to "
        "release exclusive access for {}: error {}",
        host_path_.string(), windows_exclusive_access_last_error_);
  } else {
    windows_exclusive_access_last_error_ = 0;
  }

  windows_exclusive_access_held_ = false;
}

// NOTE: unverified/uncompiled on this Linux-only build environment. Mirrors
// InitializePhysicalDriveStateOnceLinux()'s TEST UNIT READY + INQUIRY
// preflight using the same SPTI plumbing already used by
// ProbeFirmwareIdentityByInquiry()'s WIN32 branch. READ CAPACITY / READ DISC
// STRUCTURE are not implemented for Windows (not implemented there today
// either), so preflight is considered complete once TUR + INQUIRY have run.
bool DiscOmnidriveDevice::InitializePhysicalDriveStateOnceWindows() const {
  if (physical_drive_state_attempted_) {
    return physical_drive_state_ready_;
  }
  physical_drive_state_attempted_ = true;
  physical_drive_state_ready_ = false;
  physical_drive_state_failed_command_ = "none";

  if (physical_transport_handle_ == INVALID_HANDLE_VALUE ||
      physical_transport_backend_ != PhysicalTransportBackend::kWinSpti) {
    return false;
  }

  HANDLE handle = static_cast<HANDLE>(physical_transport_handle_);
  const uint8_t cdb_test_unit_ready[6] = {0, 0, 0, 0, 0, 0};

  SptdWithSense sptd = {};
  sptd.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
  sptd.sptd.CdbLength = sizeof(cdb_test_unit_ready);
  sptd.sptd.SenseInfoLength = sizeof(sptd.sense_buffer);
  sptd.sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
  sptd.sptd.DataTransferLength = 0;
  sptd.sptd.TimeOutValue = 30;
  sptd.sptd.DataBuffer = nullptr;
  sptd.sptd.SenseInfoOffset = offsetof(SptdWithSense, sense_buffer);
  std::memcpy(sptd.sptd.Cdb, cdb_test_unit_ready, sizeof(cdb_test_unit_ready));

  DWORD returned = 0;
  const bool ioctl_ok =
      DeviceIoControl(handle, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd,
                      sizeof(sptd), &sptd, sizeof(sptd), &returned, nullptr);
  if (!ioctl_ok) {
    physical_drive_state_failed_command_ = "TEST UNIT READY";
    XELOGE(
        "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceWindows: "
        "physical preflight failed for {} at required command {}: error {}",
        host_path_.string(), physical_drive_state_failed_command_,
        GetLastError());
    return false;
  }

  if (sptd.sptd.ScsiStatus != 0) {
    // Sense data is in sptd.sense_buffer; check the same fixed-format
    // sense_key(byte2)/asc(byte12)/ascq(byte13) fields as the Linux path.
    const uint8_t sense_key = sptd.sense_buffer[2] & 0x0F;
    const uint8_t asc = sptd.sense_buffer[12];
    const uint8_t ascq = sptd.sense_buffer[13];
    constexpr uint8_t kSenseKeyNotReady = 0x02;
    constexpr uint8_t kAscMediumNotPresent = 0x3A;
    if (sense_key == kSenseKeyNotReady && asc == kAscMediumNotPresent) {
      // Drive is present/working, just has no disc inserted.
      media_inserted_ = false;
      XELOGI(
          "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceWindows: "
          "no media present for {} (NOT READY/MEDIUM NOT PRESENT)",
          host_path_.string());
    } else {
      physical_drive_state_failed_command_ = "TEST UNIT READY";
      XELOGE(
          "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceWindows: "
          "physical preflight failed for {} at required command {} "
          "(sense_key=0x{:02X} asc=0x{:02X} ascq=0x{:02X})",
          host_path_.string(), physical_drive_state_failed_command_, sense_key,
          asc, ascq);
      return false;
    }
  } else {
    media_inserted_ = true;
  }

  const InquiryProbeResult firmware_probe = ProbeFirmwareIdentityByInquiry();
  omnidrive_firmware_confirmed_ = firmware_probe.omnidrive_detected;
  omnidrive_version_.major = firmware_probe.omnidrive_version.major;
  omnidrive_version_.minor = firmware_probe.omnidrive_version.minor;
  omnidrive_version_.build = firmware_probe.omnidrive_version.build;
  identity_summary_ =
      fmt::format("{} {} rev{}{}", firmware_probe.vendor,
                  firmware_probe.product, firmware_probe.revision,
                  firmware_probe.omnidrive_detected
                      ? fmt::format(" (OmniDrive v{}.{}.{})",
                                    firmware_probe.omnidrive_version.major,
                                    firmware_probe.omnidrive_version.minor,
                                    firmware_probe.omnidrive_version.build)
                      : std::string());
  XELOGI(
      "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceWindows: "
      "omnidrive_firmware_confirmed={} for {}",
      omnidrive_firmware_confirmed_, host_path_.string());

  // READ CAPACITY / READ DISC STRUCTURE are not implemented for Windows
  // (same simplified scope as the Linux "no media" early-return path).
  physical_drive_state_ready_ = true;
  XELOGI(
      "DiscOmnidriveDevice::InitializePhysicalDriveStateOnceWindows: "
      "physical preflight succeeded for {}",
      host_path_.string());
  return true;
}
#endif  // XE_PLATFORM_WIN32

bool DiscOmnidriveDevice::ReadFromPhysicalTransport(
    uint32_t address, uint32_t transfer_length, bool raw_addressing, bool fua,
    bool descramble, OmniDriveSubchannel subchannels, bool c2, uint8_t* buffer,
    size_t buffer_length, TransportFailureKind* out_failure_kind) const {
  if (out_failure_kind) {
    *out_failure_kind = TransportFailureKind::kNone;
  }
#if XE_PLATFORM_LINUX
  if (!has_physical_transport_ || physical_transport_fd_ < 0 || !buffer) {
    return false;
  }

  const bool parse_relevant_read = IsParseRelevantRead(transfer_length);
  const OmniDriveAddress translated_issue =
      TranslateAddress(address, raw_addressing);

  if (!InitializePhysicalDriveStateOnce()) {
    XELOGE(
        "DiscOmnidriveDevice::ReadFromPhysicalTransport: physical preflight "
        "not ready for {} (failed_command={})",
        host_path_.string(), physical_drive_state_failed_command_);
    return false;
  }

  const size_t sector_size = physical_sector_size();
  if (sector_size == 0 ||
      transfer_length > (std::numeric_limits<size_t>::max)() / sector_size) {
    return false;
  }
  const size_t read_length = static_cast<size_t>(transfer_length) * sector_size;
  if (buffer_length < read_length ||
      read_length > (std::numeric_limits<uint32_t>::max)()) {
    return false;
  }

  const bool frame_extraction_active =
      disc_type_ == OmniDriveDiscType::kDVD && !raw_dump_mode_;

  size_t physical_read_length = read_length;
  if (frame_extraction_active) {
    const size_t transfer_length_size = static_cast<size_t>(transfer_length);
    if (transfer_length_size >
        (std::numeric_limits<size_t>::max)() / kOmniDriveDvdFrameSize) {
      return false;
    }
    physical_read_length = transfer_length_size * kOmniDriveDvdFrameSize;
  }

  if (physical_read_length > (std::numeric_limits<uint32_t>::max)()) {
    return false;
  }

  std::vector<uint8_t> frame_buffer;
  uint8_t* sg_buffer = buffer;
  if (frame_extraction_active) {
    frame_buffer.resize(physical_read_length);
    sg_buffer = frame_buffer.data();
  }

  const uint32_t sg_transfer_length =
      static_cast<uint32_t>(physical_read_length);

  const bool force_descramble =
      disc_type_ == OmniDriveDiscType::kDVD && !raw_dump_mode_;
  const bool effective_descramble = force_descramble || descramble;
  if (force_descramble && !descramble) {
    XELOGD(
        "DiscOmnidriveDevice::ReadFromPhysicalTransport: forcing "
        "descramble=true for DVD non-raw read (incoming descramble=false)");
  }

  CDB12_ReadOmniDrive omnidrive_cdb = BuildReadOmniDriveCdb(
      address, transfer_length, disc_type_, raw_addressing, fua,
      effective_descramble, subchannels, c2);
  const uint8_t* cdb_ptr = reinterpret_cast<const uint8_t*>(&omnidrive_cdb);
  const uint8_t cdb_len = sizeof(omnidrive_cdb);

  auto run_sg_io = [&](const uint8_t* cdb_ptr, uint8_t cdb_len,
                       uint8_t* dxfer_buffer, uint32_t dxfer_len,
                       bool log_failures,
                       TransportFailureKind* failure_kind = nullptr) -> bool {
    uint8_t local_sense_buffer[32] = {0};
    sg_io_hdr_t local_io_hdr = {};
    local_io_hdr.interface_id = 'S';
    local_io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    local_io_hdr.cmd_len = cdb_len;
    local_io_hdr.mx_sb_len = sizeof(local_sense_buffer);
    local_io_hdr.dxfer_len = dxfer_len;
    local_io_hdr.dxferp = dxfer_buffer;
    local_io_hdr.cmdp = const_cast<uint8_t*>(cdb_ptr);
    local_io_hdr.sbp = local_sense_buffer;
    local_io_hdr.timeout = 8000;

    if (ioctl(physical_transport_fd_, SG_IO, &local_io_hdr) != 0) {
      const int ioctl_errno = errno;
      const bool size_related = ioctl_errno == EINVAL || ioctl_errno == ENOMEM;
      if (failure_kind) {
        *failure_kind = size_related ? TransportFailureKind::kSizeRelated
                                     : TransportFailureKind::kIoError;
      }
      if (log_failures && !size_related) {
        XELOGW(
            "DiscOmnidriveDevice::ReadFromPhysicalTransport: SG_IO failed: {}",
            std::strerror(ioctl_errno));
      }
      return false;
    }

    if ((local_io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK ||
        local_io_hdr.status != 0 || local_io_hdr.host_status != 0 ||
        local_io_hdr.driver_status != 0) {
      bool size_related = false;
      if (local_io_hdr.sb_len_wr >= 14 &&
          (local_sense_buffer[0] & 0x7F) == 0x70) {
        const uint8_t sense_key = local_sense_buffer[2] & 0x0F;
        const uint8_t asc = local_sense_buffer[12];
        // Treat invalid-field reports as adaptive-size failures.
        size_related = sense_key == 0x05 && (asc == 0x24 || asc == 0x26);
      }
      if (failure_kind) {
        *failure_kind = size_related ? TransportFailureKind::kSizeRelated
                                     : TransportFailureKind::kIoError;
      }
      if (log_failures && !size_related) {
        XELOGW(
            "DiscOmnidriveDevice::ReadFromPhysicalTransport: SG_IO "
            "status=0x{:02X} host=0x{:04X} driver=0x{:04X} sense_len={}",
            local_io_hdr.status, local_io_hdr.host_status,
            local_io_hdr.driver_status, local_io_hdr.sb_len_wr);
      }
      return false;
    }

    if (local_io_hdr.resid != 0) {
      if (failure_kind) {
        *failure_kind = TransportFailureKind::kIoError;
      }
      if (log_failures) {
        XELOGW(
            "DiscOmnidriveDevice::ReadFromPhysicalTransport: short transfer "
            "resid={} for request_len={}",
            local_io_hdr.resid, dxfer_len);
      }
      return false;
    }

    return true;
  };

  const bool read12_primary_active = disc_type_ == OmniDriveDiscType::kDVD &&
                                     !raw_dump_mode_ &&
                                     !omnidrive_firmware_confirmed_;
  if (read12_primary_active) {
    CDB12_Read12 read12_cdb = BuildRead12Cdb(address, transfer_length, fua);
    const uint8_t* read12_cdb_ptr =
        reinterpret_cast<const uint8_t*>(&read12_cdb);
    const uint8_t read12_cdb_len = sizeof(read12_cdb);

    XELOGD(
        "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
        "command_path=READ12_PRIMARY opcode=0xA8 address={} blocks={} "
        "raw_dump_mode={} disc_type=DVD",
        address, transfer_length, raw_dump_mode_);
    if (translated_issue.lba >= 0) {
      RecordDiskIssue(translated_issue.sector, transfer_length);
    }
    if (run_sg_io(read12_cdb_ptr, read12_cdb_len, buffer,
                  static_cast<uint32_t>(read_length), false,
                  out_failure_kind)) {
      if (parse_relevant_read) {
        const uint64_t payload_hash = XXH3_64bits(buffer, read_length);
        XELOGD(
            "DiscOmnidriveDevice::ReadFromPhysicalTransport: parse_read "
            "path=READ12 addr={} blocks={} frame_extraction={} payload_hash="
            "0x{:016X} preview=[{}]",
            address, transfer_length, frame_extraction_active ? "on" : "off",
            payload_hash, HexPreview(buffer, read_length));
      }
      XELOGD(
          "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
          "final_path=READ12 opcode=0xA8 address={} blocks={} success=true",
          address, transfer_length);
      return true;
    }

    if (transfer_length > 1) {
      XELOGW(
          "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
          "command_path=READ12_PRIMARY failed for address={} blocks={}; "
          "deferring retry to smaller upper-layer chunks",
          address, transfer_length);
      return false;
    }

    XELOGW(
        "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
        "command_path=READ12_PRIMARY failed; falling back to "
        "READ_OMNIDRIVE opcode=0xC0 frame_extraction(+12 main_data)");
  }

  XELOGD(
      "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
      "command_path=READ_OMNIDRIVE opcode=0xC0 address={} blocks={} "
      "raw_dump_mode={} disc_type={} descramble_in={} descramble_effective={} "
      "frame_extraction={}",
      address, transfer_length, raw_dump_mode_,
      disc_type_ == OmniDriveDiscType::kDVD ? "DVD" : "CD", descramble,
      effective_descramble, frame_extraction_active ? "active" : "off");

  if (translated_issue.lba >= 0) {
    RecordDiskIssue(translated_issue.sector, transfer_length);
  }

  if (!run_sg_io(cdb_ptr, cdb_len, sg_buffer, sg_transfer_length, true,
                 out_failure_kind)) {
    return false;
  }

  if (frame_extraction_active) {
    for (uint32_t i = 0; i < transfer_length; ++i) {
      const size_t frame_base = static_cast<size_t>(i) * kOmniDriveDvdFrameSize;
      const size_t out_base =
          static_cast<size_t>(i) * kOmniDriveDvdMainDataSize;
      std::memcpy(
          buffer + out_base,
          frame_buffer.data() + frame_base + kOmniDriveDvdMainDataOffset,
          kOmniDriveDvdMainDataSize);
    }
  }

  if (parse_relevant_read) {
    const uint64_t payload_hash = XXH3_64bits(buffer, read_length);
    const uint64_t raw_hash =
        frame_extraction_active
            ? XXH3_64bits(frame_buffer.data(), physical_read_length)
            : payload_hash;
    XELOGD(
        "DiscOmnidriveDevice::ReadFromPhysicalTransport: parse_read "
        "path=READ_OMNIDRIVE addr={} blocks={} frame_extraction={} "
        "payload_hash=0x{:016X} raw_hash=0x{:016X} preview=[{}]",
        address, transfer_length, frame_extraction_active ? "on" : "off",
        payload_hash, raw_hash, HexPreview(buffer, read_length));
  }

  XELOGD(
      "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
      "final_path=READ_OMNIDRIVE opcode=0xC0 frame_extraction={} address={} "
      "blocks={} success=true",
      frame_extraction_active ? "active" : "off", address, transfer_length);

  return true;
#elif XE_PLATFORM_WIN32
  if (!has_physical_transport_ || !buffer ||
      physical_transport_handle_ == INVALID_HANDLE_VALUE) {
    return false;
  }

  const size_t sector_size = physical_sector_size();
  if (sector_size == 0 ||
      transfer_length > (std::numeric_limits<size_t>::max)() / sector_size) {
    return false;
  }
  const size_t read_length = static_cast<size_t>(transfer_length) * sector_size;
  if (buffer_length < read_length ||
      read_length > (std::numeric_limits<DWORD>::max)()) {
    return false;
  }

  const bool frame_extraction_active =
      disc_type_ == OmniDriveDiscType::kDVD && !raw_dump_mode_;
  size_t physical_read_length = read_length;
  if (frame_extraction_active) {
    const size_t transfer_length_size = static_cast<size_t>(transfer_length);
    if (transfer_length_size >
        (std::numeric_limits<size_t>::max)() / kOmniDriveDvdFrameSize) {
      return false;
    }
    physical_read_length = transfer_length_size * kOmniDriveDvdFrameSize;
    if (physical_read_length > (std::numeric_limits<DWORD>::max)()) {
      return false;
    }
  }

  std::vector<uint8_t> frame_buffer;
  uint8_t* spti_buffer = buffer;
  if (frame_extraction_active) {
    frame_buffer.resize(physical_read_length);
    spti_buffer = frame_buffer.data();
  }

  HANDLE handle = static_cast<HANDLE>(physical_transport_handle_);

  const OmniDriveAddress translated_issue =
      TranslateAddress(address, raw_addressing);

  if (physical_transport_backend_ ==
      PhysicalTransportBackend::kLinuxStyleFile) {
    const OmniDriveAddress translated =
        TranslateAddress(address, raw_addressing);
    if (translated.lba < 0) {
      return false;
    }

    LARGE_INTEGER seek_offset;
    seek_offset.QuadPart = static_cast<LONGLONG>(translated.offset);
    if (!SetFilePointerEx(handle, seek_offset, nullptr, FILE_BEGIN)) {
      XELOGW(
          "DiscOmnidriveDevice::ReadFromPhysicalTransport: Wine Linux-style "
          "seek failed for {}: error {}",
          host_path_.string(), GetLastError());
      return false;
    }

    RecordDiskIssue(translated.sector, transfer_length);

    DWORD bytes_read = 0;
    if (!ReadFile(handle, buffer, static_cast<DWORD>(read_length), &bytes_read,
                  nullptr)) {
      XELOGW(
          "DiscOmnidriveDevice::ReadFromPhysicalTransport: Wine Linux-style "
          "read failed for {}: error {}",
          host_path_.string(), GetLastError());
      return false;
    }
    if (bytes_read != static_cast<DWORD>(read_length)) {
      XELOGW(
          "DiscOmnidriveDevice::ReadFromPhysicalTransport: Wine Linux-style "
          "short read {} / {} for {}",
          bytes_read, read_length, host_path_.string());
      return false;
    }

    return true;
  }

  if (physical_transport_backend_ != PhysicalTransportBackend::kWinSpti) {
    return false;
  }

  auto run_spti = [&](const uint8_t* cdb_ptr, uint8_t cdb_len,
                      uint8_t* data_buffer, ULONG data_length,
                      bool log_failures,
                      TransportFailureKind* failure_kind = nullptr) -> bool {
    SptdWithSense local_sptd = {};
    local_sptd.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    local_sptd.sptd.CdbLength = cdb_len;
    local_sptd.sptd.SenseInfoLength = sizeof(local_sptd.sense_buffer);
    local_sptd.sptd.DataIn = SCSI_IOCTL_DATA_IN;
    local_sptd.sptd.DataTransferLength = data_length;
    local_sptd.sptd.TimeOutValue = 30;
    local_sptd.sptd.DataBuffer = data_buffer;
    local_sptd.sptd.SenseInfoOffset = offsetof(SptdWithSense, sense_buffer);
    std::memcpy(local_sptd.sptd.Cdb, cdb_ptr, cdb_len);

    DWORD local_returned = 0;
    if (!DeviceIoControl(handle, IOCTL_SCSI_PASS_THROUGH_DIRECT, &local_sptd,
                         sizeof(local_sptd), &local_sptd, sizeof(local_sptd),
                         &local_returned, nullptr)) {
      const DWORD io_error = GetLastError();
      const bool size_related = io_error == ERROR_INVALID_PARAMETER ||
                                io_error == ERROR_INSUFFICIENT_BUFFER ||
                                io_error == ERROR_NOT_ENOUGH_MEMORY;
      if (failure_kind) {
        *failure_kind = size_related ? TransportFailureKind::kSizeRelated
                                     : TransportFailureKind::kIoError;
      }
      if (log_failures && !size_related) {
        XELOGW(
            "DiscOmnidriveDevice::ReadFromPhysicalTransport: SPTI failed for "
            "{}: error {}",
            host_path_.string(), io_error);
      }
      return false;
    }

    if (local_sptd.sptd.ScsiStatus != 0) {
      bool size_related = false;
      if (local_sptd.sptd.SenseInfoLength >= 14 &&
          (local_sptd.sense_buffer[0] & 0x7F) == 0x70) {
        const uint8_t sense_key = local_sptd.sense_buffer[2] & 0x0F;
        const uint8_t asc = local_sptd.sense_buffer[12];
        size_related = sense_key == 0x05 && (asc == 0x24 || asc == 0x26);
      }
      if (failure_kind) {
        *failure_kind = size_related ? TransportFailureKind::kSizeRelated
                                     : TransportFailureKind::kIoError;
      }
      if (log_failures && !size_related) {
        XELOGW(
            "DiscOmnidriveDevice::ReadFromPhysicalTransport: SPTI "
            "status=0x{:02X} for {}",
            local_sptd.sptd.ScsiStatus, host_path_.string());
      }
      return false;
    }

    return true;
  };

  const bool force_descramble =
      disc_type_ == OmniDriveDiscType::kDVD && !raw_dump_mode_;
  const bool effective_descramble = force_descramble || descramble;
  if (force_descramble && !descramble) {
    XELOGD(
        "DiscOmnidriveDevice::ReadFromPhysicalTransport: forcing "
        "descramble=true for DVD non-raw read (incoming descramble=false)");
  }

  const bool use_read12_primary = frame_extraction_active && !raw_addressing;
  if (use_read12_primary) {
    CDB12_Read12 read12_cdb = BuildRead12Cdb(address, transfer_length, fua);
    const uint8_t* read12_cdb_ptr =
        reinterpret_cast<const uint8_t*>(&read12_cdb);
    const uint8_t read12_cdb_len = sizeof(read12_cdb);

    XELOGD(
        "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
        "command_path=READ12_PRIMARY opcode=0xA8 address={} blocks={} "
        "raw_dump_mode={} disc_type=DVD",
        address, transfer_length, raw_dump_mode_);

    if (translated_issue.lba >= 0) {
      RecordDiskIssue(translated_issue.sector, transfer_length);
    }

    if (run_spti(read12_cdb_ptr, read12_cdb_len, buffer,
                 static_cast<ULONG>(read_length), false, out_failure_kind)) {
      XELOGD(
          "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
          "final_path=READ12 opcode=0xA8 address={} blocks={} success=true",
          address, transfer_length);
      return true;
    }

    const TransportFailureKind failure_kind =
        out_failure_kind ? *out_failure_kind : TransportFailureKind::kNone;
    if (transfer_length > 1) {
      if (failure_kind != TransportFailureKind::kSizeRelated) {
        XELOGW(
            "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
            "command_path=READ12_PRIMARY failed for address={} blocks={}; "
            "deferring retry to smaller upper-layer chunks",
            address, transfer_length);
      }
      return false;
    }

    if (failure_kind != TransportFailureKind::kSizeRelated) {
      XELOGW(
          "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
          "command_path=READ12_PRIMARY failed; falling back to "
          "READ_OMNIDRIVE opcode=0xC0 frame_extraction(+12 main_data)");
    }
  }

  CDB12_ReadOmniDrive omnidrive_cdb = BuildReadOmniDriveCdb(
      address, transfer_length, disc_type_, raw_addressing, fua,
      effective_descramble, subchannels, c2);
  const uint8_t* cdb_ptr = reinterpret_cast<const uint8_t*>(&omnidrive_cdb);
  const uint8_t cdb_len = sizeof(omnidrive_cdb);
  XELOGD(
      "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
      "command_path=READ_OMNIDRIVE opcode=0xC0 address={} blocks={} "
      "raw_dump_mode={} disc_type={} descramble_in={} descramble_effective={} "
      "frame_extraction={}",
      address, transfer_length, raw_dump_mode_,
      disc_type_ == OmniDriveDiscType::kDVD ? "DVD" : "CD", descramble,
      effective_descramble, frame_extraction_active ? "active" : "off");

  if (translated_issue.lba >= 0) {
    RecordDiskIssue(translated_issue.sector, transfer_length);
  }

  if (!run_spti(cdb_ptr, cdb_len, spti_buffer,
                static_cast<ULONG>(physical_read_length), true,
                out_failure_kind)) {
    return false;
  }

  if (frame_extraction_active) {
    for (uint32_t i = 0; i < transfer_length; ++i) {
      const size_t frame_base = static_cast<size_t>(i) * kOmniDriveDvdFrameSize;
      const size_t out_base =
          static_cast<size_t>(i) * kOmniDriveDvdMainDataSize;
      std::memcpy(
          buffer + out_base,
          frame_buffer.data() + frame_base + kOmniDriveDvdMainDataOffset,
          kOmniDriveDvdMainDataSize);
    }
  }

  XELOGD(
      "DiscOmnidriveDevice::ReadFromPhysicalTransport: "
      "final_path=READ_OMNIDRIVE opcode=0xC0 frame_extraction={} address={} "
      "blocks={} success=true",
      frame_extraction_active ? "active" : "off", address, transfer_length);

  return true;
#else
  return false;
#endif  // XE_PLATFORM_LINUX
}

DiscOmnidriveDevice::Error DiscOmnidriveDevice::Verify(ParseState* state) {
  static constexpr size_t likely_offsets[] = {
      0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000,
  };

  bool magic_found = false;
  for (size_t i = 0; i < xe::countof(likely_offsets); ++i) {
    state->game_offset = likely_offsets[i];
    if (VerifyMagic(state, state->game_offset + (32 * kGdfSectorSize))) {
      magic_found = true;
      break;
    }
  }
  if (!magic_found) {
    return Error::kErrorFileMismatch;
  }

  const size_t fs_offset = state->game_offset + (32 * kGdfSectorSize);
  std::array<uint8_t, kGdfSectorSize> fs_sector{};
  if (!ReadMetadataBytesFromPhysicalTransport(
          fs_offset, std::span<uint8_t>(fs_sector.data(), fs_sector.size()))) {
    return Error::kErrorReadError;
  }

  state->root_sector = xe::load<uint32_t>(fs_sector.data() + 20);
  state->root_size = xe::load<uint32_t>(fs_sector.data() + 24);
  if (state->root_size < 13 || state->root_size > 32_MiB) {
    return Error::kErrorDamagedFile;
  }

  if (state->root_sector >
      (std::numeric_limits<size_t>::max)() / kGdfSectorSize) {
    return Error::kErrorDamagedFile;
  }
  const size_t root_offset =
      state->game_offset + (state->root_sector * kGdfSectorSize);
  if (root_offset < state->game_offset) {
    return Error::kErrorDamagedFile;
  }
  state->root_offset = root_offset;

  if (state->size != (std::numeric_limits<size_t>::max)()) {
    if (state->root_offset >= state->size ||
        state->root_size > state->size - state->root_offset) {
      return Error::kErrorReadError;
    }
  }

  return Error::kSuccess;
}

bool DiscOmnidriveDevice::VerifyMagic(ParseState* state, size_t offset) {
  if (offset > (std::numeric_limits<size_t>::max)() - 20) {
    return false;
  }
  if (state->size != (std::numeric_limits<size_t>::max)() &&
      offset >= state->size) {
    return false;
  }

  std::array<uint8_t, 20> magic{};
  if (!ReadMetadataBytesFromPhysicalTransport(
          offset, std::span<uint8_t>(magic.data(), magic.size()))) {
    return false;
  }

  return std::memcmp(magic.data(), "MICROSOFT*XBOX*MEDIA", magic.size()) == 0;
}

DiscOmnidriveDevice::Error DiscOmnidriveDevice::ReadAllEntries(
    ParseState* state, const uint8_t* root_buffer) {
  auto root_entry = std::make_unique<DiscDriveEntry>(this, nullptr, "", 0, 0);
  root_entry->SetMetadata(kFileAttributeDirectory, 0, 0, 0, 0, 0);
  root_entry_ = std::move(root_entry);

  auto* root_drive_entry = static_cast<DiscDriveEntry*>(root_entry_.get());
  if (!ReadEntry(state, root_buffer, state->root_size, 0, root_drive_entry)) {
    root_entry_.reset();
    return Error::kErrorDamagedFile;
  }

  return Error::kSuccess;
}

bool DiscOmnidriveDevice::ReadEntry(ParseState* state, const uint8_t* buffer,
                                    size_t buffer_length,
                                    uint16_t entry_ordinal,
                                    DiscDriveEntry* parent) {
  if (!buffer || !parent) {
    return false;
  }

  const size_t entry_offset = static_cast<size_t>(entry_ordinal) * 4;
  if (entry_offset > buffer_length || buffer_length - entry_offset < 14) {
    return false;
  }

  const uint8_t* p = buffer + entry_offset;

  const uint16_t node_l = xe::load<uint16_t>(p + 0);
  const uint16_t node_r = xe::load<uint16_t>(p + 2);
  const size_t sector = xe::load<uint32_t>(p + 4);
  const size_t length = xe::load<uint32_t>(p + 8);
  const uint8_t attributes = xe::load<uint8_t>(p + 12);
  const uint8_t name_length = xe::load<uint8_t>(p + 13);
  const size_t bytes_after_header = buffer_length - entry_offset - 14;
  if (name_length > bytes_after_header) {
    return false;
  }
  const auto* name_buffer = reinterpret_cast<const char*>(p + 14);

  if (node_l && !ReadEntry(state, buffer, buffer_length, node_l, parent)) {
    return false;
  }

  auto ansi_name = std::string(name_buffer, name_length);
  auto name = xe::win1252_to_utf8(ansi_name);
  if (name.empty()) {
    name = ansi_name;
  }

  size_t data_offset = 0;
  size_t data_size = 0;
  if (!(attributes & kFileAttributeDirectory)) {
    if (sector > (std::numeric_limits<size_t>::max)() / kGdfSectorSize) {
      return false;
    }
    const size_t offset = state->game_offset + (sector * kGdfSectorSize);
    if (offset < state->game_offset) {
      return false;
    }
    data_offset = offset;
    data_size = length;
  }

  auto entry =
      DiscDriveEntry::Create(this, parent, name, data_offset, data_size);
  entry->SetMetadata(attributes | kFileAttributeReadOnly, length,
                     xe::round_up(length, bytes_per_sector()),
                     kUnixEpochAsFiletime, kUnixEpochAsFiletime,
                     kUnixEpochAsFiletime);

  if (attributes & kFileAttributeDirectory) {
    if (length) {
      if (length > 32_MiB ||
          sector > (std::numeric_limits<size_t>::max)() / kGdfSectorSize) {
        return false;
      }
      const size_t folder_offset =
          state->game_offset + (sector * kGdfSectorSize);
      if (folder_offset < state->game_offset) {
        return false;
      }
      std::vector<uint8_t> folder_buffer(length);
      if (!ReadMetadataBytesFromPhysicalTransport(
              folder_offset,
              std::span<uint8_t>(folder_buffer.data(), folder_buffer.size()))) {
        return false;
      }
      if (!ReadEntry(state, folder_buffer.data(), length, 0, entry.get())) {
        return false;
      }
    }
  }

  parent->AddChild(std::move(entry));

  if (node_r && !ReadEntry(state, buffer, buffer_length, node_r, parent)) {
    return false;
  }

  return true;
}

}  // namespace xe::vfs
