/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/console_app_main.h"
#include "xenia/base/logging.h"
#include "xenia/vfs/devices/disc_omnidrive_device.h"

namespace xe::app {

void ShowSecuritySectorSanityWarningOnce() {
  // Standalone probe does not surface interactive UI warnings.
}

}  // namespace xe::app

namespace xe::vfs {
namespace {

enum class ProbeExitCode : int {
  kSuccess = 0,
  kParseFailure = 1,
  kInitializeFailure = 2,
  kReadFailure = 3,
  kCompareFailure = 4,
  kCompareIoFailure = 5,
};

enum class ParseResult {
  kNoMatch,
  kSuccess,
  kError,
};

struct ProbeOptions {
  std::string device_path = "/dev/sg1";
  std::string compare_iso_path;
  OmniDriveDiscType disc_type = OmniDriveDiscType::kDVD;
  uint32_t address = 0;
  uint32_t block_count = 1;
  uint32_t layer0_last = 0;
  bool layer0_last_available = false;
  int64_t compare_iso_base_lba = 0;
  bool compare_iso_auto_base = false;
  bool raw_addressing = false;
  bool raw_dump_mode = false;
  bool fua = false;
  bool descramble = false;
  bool descramble_explicitly_set = false;
  OmniDriveSubchannel subchannel = OmniDriveSubchannel::kNone;
  bool c2 = false;
  bool sanity_read_next = false;
};

void PrintUsage(const char* exe_name) {
  XELOGI(
      "Usage: {} [--device_path PATH] [--disc_type cd|dvd] "
      "[--address N] [--block_count N] [--raw_addressing BOOL] "
      "[--layer0_last N] [--compare_iso PATH] [--compare_iso_base_lba N] "
      "[--compare_iso_auto_base BOOL] "
      "[--sanity_read_next BOOL]",
      exe_name);
  XELOGI(
      "Optional: [--raw_dump_mode BOOL] [--fua BOOL] [--descramble BOOL] "
      "[--subchannel none|q16|raw96] [--c2 BOOL]");
  XELOGI(
      "Example: {} --device_path /dev/sg1 --disc_type dvd --address 0 "
      "--block_count 1 --raw_addressing false",
      exe_name);
  std::fprintf(
      stdout,
      "Usage: %s [--device_path PATH] [--disc_type cd|dvd] [--address N] "
      "[--block_count N] [--raw_addressing BOOL] [--layer0_last N] "
      "[--compare_iso PATH] [--compare_iso_base_lba N] "
      "[--compare_iso_auto_base BOOL] [--sanity_read_next BOOL]\n",
      exe_name);
  std::fprintf(
      stdout,
      "Optional: [--raw_dump_mode BOOL] [--fua BOOL] [--descramble BOOL] "
      "[--subchannel none|q16|raw96] [--c2 BOOL]\n");
  std::fprintf(stdout,
               "Example: %s --device_path /dev/sg1 --disc_type dvd --address 0 "
               "--block_count 1 --raw_addressing false\n",
               exe_name);
}

std::string ToLower(std::string value) {
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return value;
}

bool ParseBool(std::string_view text, bool* out_value) {
  const std::string lower = ToLower(std::string(text));
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
    *out_value = true;
    return true;
  }
  if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
    *out_value = false;
    return true;
  }
  return false;
}

bool ParseUint32(std::string_view text, uint32_t* out_value) {
  if (text.empty()) {
    return false;
  }
  try {
    size_t parsed_length = 0;
    const uint64_t value = std::stoull(std::string(text), &parsed_length, 0);
    if (parsed_length != text.size() ||
        value > (std::numeric_limits<uint32_t>::max)()) {
      return false;
    }
    *out_value = static_cast<uint32_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseInt64(std::string_view text, int64_t* out_value) {
  if (text.empty()) {
    return false;
  }
  try {
    size_t parsed_length = 0;
    const int64_t value = std::stoll(std::string(text), &parsed_length, 0);
    if (parsed_length != text.size()) {
      return false;
    }
    *out_value = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseDiscType(std::string_view text, OmniDriveDiscType* out_value) {
  const std::string lower = ToLower(std::string(text));
  if (lower == "cd") {
    *out_value = OmniDriveDiscType::kCD;
    return true;
  }
  if (lower == "dvd") {
    *out_value = OmniDriveDiscType::kDVD;
    return true;
  }
  return false;
}

bool ParseSubchannel(std::string_view text, OmniDriveSubchannel* out_value) {
  const std::string lower = ToLower(std::string(text));
  if (lower == "none") {
    *out_value = OmniDriveSubchannel::kNone;
    return true;
  }
  if (lower == "q16") {
    *out_value = OmniDriveSubchannel::kQ16;
    return true;
  }
  if (lower == "raw96") {
    *out_value = OmniDriveSubchannel::kRaw96;
    return true;
  }
  return false;
}

bool NextArgumentValue(const std::vector<std::string>& args, size_t* index,
                       const char* flag, std::string_view* out_value) {
  const std::string_view arg = args[*index];
  const std::string_view flag_view(flag);
  if (arg == flag_view) {
    if (*index + 1 >= args.size()) {
      return false;
    }
    ++(*index);
    *out_value = args[*index];
    return true;
  }

  if (arg.size() > flag_view.size() &&
      arg.compare(0, flag_view.size(), flag) == 0 &&
      arg[flag_view.size()] == '=') {
    *out_value = arg.substr(flag_view.size() + 1);
    return true;
  }

  return false;
}

bool ParseProbeOptions(const std::vector<std::string>& args,
                       ProbeOptions* out_options) {
  if (args.size() <= 1) {
    XELOGE("No arguments provided.");
    std::fprintf(stderr, "Parse error: no arguments provided.\n");
    return false;
  }

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(args[0].c_str());
      return false;
    }

    auto parse_value = [&](const char* flag, auto parse_fn, auto* target,
                           const char* expected) -> ParseResult {
      const size_t flag_length = std::strlen(flag);
      const bool exact_match = arg == flag;
      const bool match_with_equals = arg.size() > flag_length &&
                                     arg.compare(0, flag_length, flag) == 0 &&
                                     arg[flag_length] == '=';
      if (!exact_match && !match_with_equals) {
        return ParseResult::kNoMatch;
      }

      std::string_view value;
      if (!NextArgumentValue(args, &i, flag, &value)) {
        XELOGE("Missing value for {}", flag);
        std::fprintf(stderr, "Parse error: missing value for %s.\n", flag);
        return ParseResult::kError;
      }
      if (!parse_fn(value, target)) {
        XELOGE("Invalid value '{}' for {} (expected {})", value, flag,
               expected);
        std::fprintf(
            stderr, "Parse error: invalid value '%.*s' for %s (expected %s).\n",
            static_cast<int>(value.size()), value.data(), flag, expected);
        return ParseResult::kError;
      }
      return ParseResult::kSuccess;
    };

    ParseResult parse_result = ParseResult::kNoMatch;
    parse_result = parse_value(
        "--device_path",
        [](std::string_view value, std::string* out_value) {
          out_value->assign(value);
          return !out_value->empty();
        },
        &out_options->device_path, "non-empty path");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result = parse_value("--disc_type", ParseDiscType,
                               &out_options->disc_type, "cd|dvd");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result = parse_value(
        "--compare_iso",
        [](std::string_view value, std::string* out_value) {
          out_value->assign(value);
          return !out_value->empty();
        },
        &out_options->compare_iso_path, "non-empty path");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result = parse_value("--compare_iso_base_lba", ParseInt64,
                               &out_options->compare_iso_base_lba,
                               "int64 (decimal or 0x hex)");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result = parse_value(
        "--layer0_last",
        [](std::string_view value, ProbeOptions* options) {
          uint32_t parsed = 0;
          if (!ParseUint32(value, &parsed)) {
            return false;
          }
          options->layer0_last = parsed;
          options->layer0_last_available = true;
          return true;
        },
        out_options, "uint32");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result =
        parse_value("--compare_iso_auto_base", ParseBool,
                    &out_options->compare_iso_auto_base, "true|false");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result =
        parse_value("--address", ParseUint32, &out_options->address, "uint32");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result = parse_value("--block_count", ParseUint32,
                               &out_options->block_count, "uint32 > 0");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result = parse_value("--raw_addressing", ParseBool,
                               &out_options->raw_addressing, "true|false");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result = parse_value("--raw_dump_mode", ParseBool,
                               &out_options->raw_dump_mode, "true|false");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result =
        parse_value("--fua", ParseBool, &out_options->fua, "true|false");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result = parse_value("--descramble", ParseBool,
                               &out_options->descramble, "true|false");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      out_options->descramble_explicitly_set = true;
      continue;
    }

    parse_result = parse_value("--subchannel", ParseSubchannel,
                               &out_options->subchannel, "none|q16|raw96");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result =
        parse_value("--c2", ParseBool, &out_options->c2, "true|false");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    parse_result = parse_value("--sanity_read_next", ParseBool,
                               &out_options->sanity_read_next, "true|false");
    if (parse_result == ParseResult::kError) {
      return false;
    }
    if (parse_result == ParseResult::kSuccess) {
      continue;
    }

    XELOGE("Unknown argument: {}", arg);
    std::fprintf(stderr, "Parse error: unknown argument '%.*s'.\n",
                 static_cast<int>(arg.size()), arg.data());
    return false;
  }

  if (out_options->device_path.empty()) {
    XELOGE("--device_path must not be empty");
    std::fprintf(stderr, "Parse error: --device_path must not be empty.\n");
    return false;
  }
  if (out_options->block_count == 0) {
    XELOGE("--block_count must be greater than 0");
    std::fprintf(stderr,
                 "Parse error: --block_count must be greater than 0.\n");
    return false;
  }

  return true;
}

size_t ComputeSectorSize(OmniDriveDiscType disc_type, bool raw_dump_mode) {
  if (!raw_dump_mode) {
    return 2048;
  }
  switch (disc_type) {
    case OmniDriveDiscType::kCD:
      return 2352;
    case OmniDriveDiscType::kDVD:
      return 2064;
  }
  return 2048;
}

const char* DiscTypeName(OmniDriveDiscType disc_type) {
  switch (disc_type) {
    case OmniDriveDiscType::kCD:
      return "cd";
    case OmniDriveDiscType::kDVD:
      return "dvd";
  }
  return "unknown";
}

void LogFailureHints(const std::string& device_path) {
  XELOGW(
      "Hints: ensure you have read permission for {} (udev/group settings, "
      "often 'cdrom').",
      device_path);
  if (device_path.rfind("/dev/sg", 0) == 0) {
    XELOGW(
        "Hints: verify {} maps to the optical drive (for example, lsscsi -g). "
        "Non-optical sg nodes will fail this probe.",
        device_path);
  }
}

void PrintHexPreview(const uint8_t* bytes, size_t length) {
  std::string line;
  constexpr char kHex[] = "0123456789ABCDEF";
  const size_t preview_size = length < 16 ? length : 16;
  line.reserve(preview_size * 3);
  for (size_t i = 0; i < preview_size; ++i) {
    const uint8_t value = bytes[i];
    line.push_back(kHex[value >> 4]);
    line.push_back(kHex[value & 0xF]);
    if (i + 1 < preview_size) {
      line.push_back(' ');
    }
  }
  XELOGI("First {} bytes: {}", preview_size, line);
}

std::string FormatHexSlice(const uint8_t* bytes, size_t length) {
  std::ostringstream stream;
  constexpr char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < length; ++i) {
    if (i > 0) {
      stream << ' ';
    }
    const uint8_t value = bytes[i];
    stream << kHex[value >> 4] << kHex[value & 0xF];
  }
  return stream.str();
}

constexpr size_t kSecuritySectorPayloadSize = 2048;
constexpr size_t kSecuritySectorRangeEntrySize = 9;
constexpr size_t kSecuritySectorRangeEntryCount = 23;
constexpr size_t kSecuritySectorRangesAOffset = 1633;
constexpr size_t kSecuritySectorRangesBOffset = 1840;

uint32_t ParseBE24(const uint8_t* data) {
  return (uint32_t(data[0]) << 16) | (uint32_t(data[1]) << 8) |
         uint32_t(data[2]);
}

bool IsAllZero(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (data[i] != 0) {
      return false;
    }
  }
  return true;
}

struct SecuritySectorSanityReport {
  bool size_ok = false;
  uint8_t detected_version = 0;
  size_t expected_range_entry_count = 0;
  bool byte_768_ok = false;
  bool byte_769_ok = false;
  bool byte_1632_ok = false;
  bool ranges_equal = false;
  bool ranges_equality_check_skipped = false;
  bool has_non_zero_entry = false;
  bool entries_ordered = true;
  size_t first_unordered_entry = (std::numeric_limits<size_t>::max)();
  std::string first_failure_reason;
  bool sane = false;
};

SecuritySectorSanityReport AnalyzeSecuritySector(
    const std::vector<uint8_t>& payload) {
  SecuritySectorSanityReport report;
  report.size_ok = payload.size() == kSecuritySectorPayloadSize;
  if (!report.size_ok) {
    report.first_failure_reason = "payload_size!=2048";
    return report;
  }

  report.detected_version = payload[768];
  if (report.detected_version == 1) {
    report.expected_range_entry_count = 23;
  } else if (report.detected_version == 2) {
    report.expected_range_entry_count = 21;
  }

  report.byte_768_ok = report.expected_range_entry_count != 0;
  report.byte_769_ok = report.expected_range_entry_count != 0 &&
                       payload[769] == report.expected_range_entry_count;
  report.byte_1632_ok = report.expected_range_entry_count != 0 &&
                        payload[1632] == report.expected_range_entry_count;
  if (!report.byte_768_ok || !report.byte_769_ok || !report.byte_1632_ok) {
    report.first_failure_reason = "header_marker_mismatch";
    return report;
  }

  const size_t ranges_bytes =
      report.expected_range_entry_count * kSecuritySectorRangeEntrySize;
  const uint8_t* ranges_a = payload.data() + kSecuritySectorRangesAOffset;
  const uint8_t* ranges_b = payload.data() + kSecuritySectorRangesBOffset;
  if (report.detected_version == 2) {
    report.ranges_equality_check_skipped = true;
    report.ranges_equal = true;
  } else {
    report.ranges_equal = std::memcmp(ranges_a, ranges_b, ranges_bytes) == 0;
    if (!report.ranges_equal) {
      report.first_failure_reason = "range_tables_mismatch";
      return report;
    }
  }

  for (size_t i = 0; i < report.expected_range_entry_count; ++i) {
    const uint8_t* entry = ranges_a + i * kSecuritySectorRangeEntrySize;
    const bool zero_entry = IsAllZero(entry, kSecuritySectorRangeEntrySize);
    if (zero_entry) {
      continue;
    }

    report.has_non_zero_entry = true;
    const uint32_t start_psn = ParseBE24(entry + 3);
    const uint32_t end_psn = ParseBE24(entry + 6);
    if (report.detected_version == 1 && start_psn > end_psn) {
      report.entries_ordered = false;
      report.first_unordered_entry = i;
      report.first_failure_reason = "range_entry_start_gt_end";
      return report;
    }
  }

  if (!report.has_non_zero_entry) {
    report.first_failure_reason = "no_non_zero_ranges";
    return report;
  }

  report.sane = true;
  return report;
}

void DumpAnnotatedSecuritySector(const std::vector<uint8_t>& payload,
                                 const SecuritySectorSanityReport& report) {
  XELOGI("Security sector payload size={} expected={} pass={}", payload.size(),
         kSecuritySectorPayloadSize, report.size_ok);
  std::fprintf(stdout,
               "Security sector payload size=%zu expected=%zu pass=%s\n",
               payload.size(), kSecuritySectorPayloadSize,
               report.size_ok ? "true" : "false");
  if (!report.size_ok) {
    return;
  }

  XELOGI("Byte[768]={} expected=1|2 pass={}", payload[768], report.byte_768_ok);
  std::fprintf(stdout, "Byte[768]=%u expected=1|2 pass=%s\n",
               static_cast<unsigned int>(payload[768]),
               report.byte_768_ok ? "true" : "false");
  XELOGI("Byte[769]={} expected={} (from version={}) pass={}", payload[769],
         report.expected_range_entry_count, report.detected_version,
         report.byte_769_ok);
  std::fprintf(stdout, "Byte[769]=%u expected=%zu (from version=%u) pass=%s\n",
               static_cast<unsigned int>(payload[769]),
               report.expected_range_entry_count,
               static_cast<unsigned int>(report.detected_version),
               report.byte_769_ok ? "true" : "false");
  XELOGI("Byte[1632]={} expected={} (from version={}) pass={}", payload[1632],
         report.expected_range_entry_count, report.detected_version,
         report.byte_1632_ok);
  std::fprintf(stdout, "Byte[1632]=%u expected=%zu (from version=%u) pass=%s\n",
               static_cast<unsigned int>(payload[1632]),
               report.expected_range_entry_count,
               static_cast<unsigned int>(report.detected_version),
               report.byte_1632_ok ? "true" : "false");

  const size_t ranges_bytes =
      report.expected_range_entry_count * kSecuritySectorRangeEntrySize;
  if (report.ranges_equality_check_skipped) {
    XELOGI(
        "Ranges A/B equality: a_offset={} b_offset={} entries={} entry_size={} "
        "bytes={} pass=skipped (version={})",
        kSecuritySectorRangesAOffset, kSecuritySectorRangesBOffset,
        report.expected_range_entry_count, kSecuritySectorRangeEntrySize,
        ranges_bytes, report.detected_version);
    std::fprintf(stdout,
                 "Ranges A/B equality: a_offset=%zu b_offset=%zu entries=%zu "
                 "entry_size=%zu bytes=%zu pass=skipped (version=%u)\n",
                 kSecuritySectorRangesAOffset, kSecuritySectorRangesBOffset,
                 report.expected_range_entry_count,
                 kSecuritySectorRangeEntrySize, ranges_bytes,
                 static_cast<unsigned int>(report.detected_version));
  } else {
    XELOGI(
        "Ranges A/B equality: a_offset={} b_offset={} entries={} entry_size={} "
        "bytes={} pass={}",
        kSecuritySectorRangesAOffset, kSecuritySectorRangesBOffset,
        report.expected_range_entry_count, kSecuritySectorRangeEntrySize,
        ranges_bytes, report.ranges_equal);
    std::fprintf(stdout,
                 "Ranges A/B equality: a_offset=%zu b_offset=%zu entries=%zu "
                 "entry_size=%zu bytes=%zu pass=%s\n",
                 kSecuritySectorRangesAOffset, kSecuritySectorRangesBOffset,
                 report.expected_range_entry_count,
                 kSecuritySectorRangeEntrySize, ranges_bytes,
                 report.ranges_equal ? "true" : "false");
  }

  const uint8_t* ranges_a = payload.data() + kSecuritySectorRangesAOffset;
  for (size_t i = 0; i < report.expected_range_entry_count; ++i) {
    const uint8_t* entry = ranges_a + i * kSecuritySectorRangeEntrySize;
    const bool zero_entry = IsAllZero(entry, kSecuritySectorRangeEntrySize);
    const uint32_t start_psn = ParseBE24(entry + 3);
    const uint32_t end_psn = ParseBE24(entry + 6);
    const bool ordered = start_psn <= end_psn;
    XELOGI(
        "Entry {:02}: raw=[{}] start_psn(be24,+3)={} end_psn(be24,+6)={} "
        "zero_entry={} start_le_end={}",
        i, FormatHexSlice(entry, kSecuritySectorRangeEntrySize), start_psn,
        end_psn, zero_entry, ordered);
    std::fprintf(stdout,
                 "Entry %02zu: raw=[%s] start_psn(be24,+3)=%u "
                 "end_psn(be24,+6)=%u zero_entry=%s start_le_end=%s\n",
                 i,
                 FormatHexSlice(entry, kSecuritySectorRangeEntrySize).c_str(),
                 start_psn, end_psn, zero_entry ? "true" : "false",
                 ordered ? "true" : "false");
  }

  XELOGI("Sanity summary: sane={} first_failure_reason={}", report.sane,
         report.first_failure_reason.empty() ? "none"
                                             : report.first_failure_reason);
  std::fprintf(stdout, "Sanity summary: sane=%s first_failure_reason=%s\n",
               report.sane ? "true" : "false",
               report.first_failure_reason.empty()
                   ? "none"
                   : report.first_failure_reason.c_str());
}

bool LoadIsoRange(const std::string& iso_path, uint64_t byte_offset,
                  size_t read_size, std::vector<uint8_t>* out_bytes) {
  std::ifstream iso_stream(iso_path, std::ios::binary);
  if (!iso_stream) {
    XELOGE("ISO compare failed: could not open {}", iso_path);
    std::fprintf(stderr, "ISO compare failed: could not open %s\n",
                 iso_path.c_str());
    return false;
  }

  iso_stream.seekg(0, std::ios::end);
  if (!iso_stream.good()) {
    XELOGE("ISO compare failed: could not seek end of {}", iso_path);
    std::fprintf(stderr, "ISO compare failed: could not seek end of %s\n",
                 iso_path.c_str());
    return false;
  }

  const std::streamoff iso_size_streamoff = iso_stream.tellg();
  if (iso_size_streamoff < 0) {
    XELOGE("ISO compare failed: invalid size for {}", iso_path);
    std::fprintf(stderr, "ISO compare failed: invalid size for %s\n",
                 iso_path.c_str());
    return false;
  }
  const uint64_t iso_size = static_cast<uint64_t>(iso_size_streamoff);

  const uint64_t requested_end = byte_offset + static_cast<uint64_t>(read_size);
  if (requested_end < byte_offset || requested_end > iso_size) {
    XELOGE("ISO compare failed: requested range [{}..{}) exceeds ISO size {}",
           byte_offset, requested_end, iso_size);
    std::fprintf(stderr,
                 "ISO compare failed: requested range [%llu..%llu) exceeds ISO "
                 "size %llu\n",
                 static_cast<unsigned long long>(byte_offset),
                 static_cast<unsigned long long>(requested_end),
                 static_cast<unsigned long long>(iso_size));
    return false;
  }

  iso_stream.seekg(static_cast<std::streamoff>(byte_offset), std::ios::beg);
  if (!iso_stream.good()) {
    XELOGE("ISO compare failed: could not seek to {} in {}", byte_offset,
           iso_path);
    std::fprintf(stderr, "ISO compare failed: could not seek to %llu in %s\n",
                 static_cast<unsigned long long>(byte_offset),
                 iso_path.c_str());
    return false;
  }

  out_bytes->resize(read_size);
  iso_stream.read(reinterpret_cast<char*>(out_bytes->data()),
                  static_cast<std::streamsize>(read_size));
  if (iso_stream.gcount() != static_cast<std::streamsize>(read_size) ||
      !iso_stream.good()) {
    XELOGE("ISO compare failed: short read for {} (read {} of {} bytes)",
           iso_path, static_cast<size_t>(iso_stream.gcount()), read_size);
    std::fprintf(
        stderr,
        "ISO compare failed: short read for %s (read %zu of %zu bytes)\n",
        iso_path.c_str(), static_cast<size_t>(iso_stream.gcount()), read_size);
    return false;
  }

  return true;
}

bool ComputeCompareByteOffset(const ProbeOptions& options, size_t sector_size,
                              uint64_t* out_byte_offset) {
  int64_t translated_lba = static_cast<int64_t>(options.address);
  bool layer0_last_used = false;
  if (options.raw_addressing && options.disc_type == OmniDriveDiscType::kDVD) {
    translated_lba = static_cast<int64_t>(options.address) - 0x30000;
    if (translated_lba < 0 && options.layer0_last_available) {
      translated_lba += 2 * (static_cast<int64_t>(options.layer0_last) + 1);
      layer0_last_used = true;
    }
  }

  if (translated_lba < 0) {
    XELOGE(
        "Compare failed: translated LBA is negative (disc_type={} "
        "address=0x{:X} raw_addressing={} translated_lba={} "
        "layer0_last_used={}). "
        "Provide --layer0_last for wrapped DVD raw addressing or adjust "
        "compare options.",
        DiscTypeName(options.disc_type), options.address,
        options.raw_addressing, translated_lba, layer0_last_used);
    std::fprintf(
        stderr,
        "Compare failed: translated LBA is negative (disc_type=%s address=0x%X "
        "raw_addressing=%s translated_lba=%lld layer0_last_used=%s). "
        "Provide --layer0_last for wrapped DVD raw addressing or adjust "
        "compare options.\n",
        DiscTypeName(options.disc_type), options.address,
        options.raw_addressing ? "true" : "false",
        static_cast<long long>(translated_lba),
        layer0_last_used ? "true" : "false");
    return false;
  }

  const int64_t iso_lba = translated_lba + options.compare_iso_base_lba;
  if (iso_lba < 0) {
    XELOGE(
        "Compare failed: ISO LBA is negative (translated_lba={} "
        "compare_iso_base_lba={} iso_lba={}).",
        translated_lba, options.compare_iso_base_lba, iso_lba);
    std::fprintf(stderr,
                 "Compare failed: ISO LBA is negative (translated_lba=%lld "
                 "compare_iso_base_lba=%lld iso_lba=%lld).\n",
                 static_cast<long long>(translated_lba),
                 static_cast<long long>(options.compare_iso_base_lba),
                 static_cast<long long>(iso_lba));
    return false;
  }

  const uint64_t iso_lba_u64 = static_cast<uint64_t>(iso_lba);
  if (iso_lba_u64 > (std::numeric_limits<uint64_t>::max)() /
                        static_cast<uint64_t>(sector_size)) {
    XELOGE("Compare failed: byte offset overflow (iso_lba={} sector_size={})",
           iso_lba, sector_size);
    std::fprintf(stderr,
                 "Compare failed: byte offset overflow (iso_lba=%lld "
                 "sector_size=%zu).\n",
                 static_cast<long long>(iso_lba), sector_size);
    return false;
  }

  *out_byte_offset = iso_lba_u64 * static_cast<uint64_t>(sector_size);

  XELOGI(
      "Compare parameters: disc_type={} raw_addressing={} address={} "
      "translated_lba={} layer0_last_available={} layer0_last={} "
      "layer0_last_used={} compare_iso_base_lba={} iso_lba={} sector_size={} "
      "byte_offset={}",
      DiscTypeName(options.disc_type), options.raw_addressing, options.address,
      translated_lba, options.layer0_last_available, options.layer0_last,
      layer0_last_used, options.compare_iso_base_lba, iso_lba, sector_size,
      *out_byte_offset);
  std::fprintf(stderr,
               "Compare parameters: disc_type=%s raw_addressing=%s address=%u "
               "translated_lba=%lld layer0_last_available=%s layer0_last=%u "
               "layer0_last_used=%s compare_iso_base_lba=%lld iso_lba=%lld "
               "sector_size=%zu byte_offset=%llu\n",
               DiscTypeName(options.disc_type),
               options.raw_addressing ? "true" : "false", options.address,
               static_cast<long long>(translated_lba),
               options.layer0_last_available ? "true" : "false",
               options.layer0_last, layer0_last_used ? "true" : "false",
               static_cast<long long>(options.compare_iso_base_lba),
               static_cast<long long>(iso_lba), sector_size,
               static_cast<unsigned long long>(*out_byte_offset));

  return true;
}

bool CompareAndReport(const std::vector<uint8_t>& device_data,
                      const std::vector<uint8_t>& iso_data) {
  if (device_data.size() != iso_data.size()) {
    XELOGE("Compare failed: buffer size mismatch (device={} iso={})",
           device_data.size(), iso_data.size());
    return false;
  }

  for (size_t i = 0; i < device_data.size(); ++i) {
    if (device_data[i] == iso_data[i]) {
      continue;
    }

    const size_t context_before = i < 8 ? i : 8;
    const size_t context_start = i - context_before;
    const size_t context_length = ((device_data.size() - context_start) < 16)
                                      ? (device_data.size() - context_start)
                                      : 16;

    XELOGE("COMPARE MISMATCH at +0x{:X} (device=0x{:02X} iso=0x{:02X})", i,
           static_cast<uint32_t>(device_data[i]),
           static_cast<uint32_t>(iso_data[i]));
    XELOGE("Context @ +0x{:X} ({} bytes)", context_start, context_length);
    XELOGE("Device: {}",
           FormatHexSlice(device_data.data() + context_start, context_length));
    XELOGE("ISO   : {}",
           FormatHexSlice(iso_data.data() + context_start, context_length));
    std::fprintf(
        stderr,
        "COMPARE MISMATCH at +0x%zX (device=0x%02X iso=0x%02X)\nContext @ "
        "+0x%zX (%zu bytes)\nDevice: %s\nISO   : %s\n",
        i, static_cast<unsigned int>(device_data[i]),
        static_cast<unsigned int>(iso_data[i]), context_start, context_length,
        FormatHexSlice(device_data.data() + context_start, context_length)
            .c_str(),
        FormatHexSlice(iso_data.data() + context_start, context_length)
            .c_str());
    return false;
  }

  XELOGI("COMPARE MATCH: {} bytes", device_data.size());
  return true;
}

}  // namespace

int omnidrive_sg_probe_main(const std::vector<std::string>& args) {
#if !(XE_PLATFORM_LINUX || XE_PLATFORM_WIN32)
  XELOGE("xenia-vfs-omnidrive-sg-probe is supported only on Linux and Windows");
  std::fprintf(stderr,
               "FAIL: xenia-vfs-omnidrive-sg-probe is supported only on Linux "
               "and Windows.\n");
  return static_cast<int>(ProbeExitCode::kInitializeFailure);
#endif

  std::fprintf(stdout, "xenia-vfs-omnidrive-sg-probe: starting\n");

  ProbeOptions options;
  if (!ParseProbeOptions(args, &options)) {
    PrintUsage(args[0].c_str());
    std::fprintf(stderr, "FAIL: argument parsing failed.\n");
    return static_cast<int>(ProbeExitCode::kParseFailure);
  }

  const size_t sector_size =
      ComputeSectorSize(options.disc_type, options.raw_dump_mode);
  if (sector_size == 0 ||
      options.block_count >
          (std::numeric_limits<size_t>::max)() / sector_size) {
    XELOGE("Requested buffer size is too large: block_count={} sector_size={}",
           options.block_count, sector_size);
    std::fprintf(stderr,
                 "FAIL: requested buffer size is too large (block_count=%u "
                 "sector_size=%zu).\n",
                 options.block_count, sector_size);
    return static_cast<int>(ProbeExitCode::kParseFailure);
  }
  const size_t buffer_size =
      static_cast<size_t>(options.block_count) * sector_size;

  XELOGI(
      "Probing OmniDrive: device_path={} disc_type={} address={} "
      "block_count={} raw_addressing={} raw_dump_mode={} fua={} descramble={} "
      "subchannel={} c2={} layer0_last_available={} layer0_last={} "
      "sanity_read_next={} buffer_size={}",
      options.device_path, DiscTypeName(options.disc_type), options.address,
      options.block_count, options.raw_addressing, options.raw_dump_mode,
      options.fua, options.descramble,
      static_cast<uint32_t>(options.subchannel), options.c2,
      options.layer0_last_available, options.layer0_last,
      options.sanity_read_next, buffer_size);

  if (!options.compare_iso_path.empty() &&
      options.disc_type == OmniDriveDiscType::kDVD && !options.raw_dump_mode &&
      options.descramble_explicitly_set && !options.descramble) {
    XELOGI(
        "Startup note: --descramble=false was requested, but DVD non-raw "
        "physical reads force descramble=true in the backend for ISO-byte "
        "parity.");
  }

  DiscOmnidriveDevice device("", options.device_path, options.disc_type,
                             options.raw_dump_mode);
  if (!device.Initialize()) {
    XELOGE("Initialize failed for {}", options.device_path);
    LogFailureHints(options.device_path);
    std::fprintf(stderr, "FAIL: initialize failed for %s.\n",
                 options.device_path.c_str());
    return static_cast<int>(ProbeExitCode::kInitializeFailure);
  }
  if (options.layer0_last_available) {
    XELOGI(
        "Note: --layer0_last is ignored by this probe path (security-sector "
        "API). value={}",
        options.layer0_last);
  }
  if (options.sanity_read_next) {
    XELOGI(
        "Note: --sanity_read_next is ignored by this probe path (single "
        "security-sector API read).");
  }

  auto security_sector = device.ReadSecuritySectorFromCandidates();
  if (!security_sector) {
    XELOGE(
        "Security sector read failed: ReadSecuritySectorFromCandidates "
        "returned no payload.");
    XELOGE(
        "Dialog activation diagnosis: warning dialog does not trigger because "
        "no security sector payload was available for sanity evaluation.");
    LogFailureHints(options.device_path);
    std::fprintf(stderr,
                 "FAIL: security sector read failed (no payload available).\n");
    return static_cast<int>(ProbeExitCode::kReadFailure);
  }

  std::vector<uint8_t> buffer = *security_sector;
  XELOGI("Security sector read succeeded: {} bytes", buffer.size());
  PrintHexPreview(buffer.data(), buffer.size());

  const SecuritySectorSanityReport sanity_report =
      AnalyzeSecuritySector(buffer);
  DumpAnnotatedSecuritySector(buffer, sanity_report);
  XELOGI(
      "Dialog activation diagnosis: warning_dialog_would_trigger={} (trigger "
      "requires payload present AND sanity failure).",
      !sanity_report.sane);
  std::fprintf(stdout,
               "Dialog activation diagnosis: warning_dialog_would_trigger=%s "
               "(trigger requires payload present AND sanity failure).\n",
               !sanity_report.sane ? "true" : "false");

  if (!options.compare_iso_path.empty()) {
    if (options.compare_iso_auto_base) {
      XELOGE(
          "--compare_iso_auto_base is disabled to keep compare deterministic "
          "and Xbox 360 explicit. "
          "Use --compare_iso_base_lba and optionally --layer0_last instead.");
      std::fprintf(
          stderr,
          "--compare_iso_auto_base is disabled to keep compare deterministic "
          "and Xbox 360 explicit. "
          "Use --compare_iso_base_lba and optionally --layer0_last instead.\n");
      std::fprintf(stderr, "FAIL: argument parsing failed.\n");
      return static_cast<int>(ProbeExitCode::kParseFailure);
    }

    uint64_t byte_offset = 0;
    if (!ComputeCompareByteOffset(options, sector_size, &byte_offset)) {
      std::fprintf(stderr, "FAIL: compare parameter calculation failed.\n");
      return static_cast<int>(ProbeExitCode::kCompareIoFailure);
    }
    XELOGI("Comparing against ISO: path={} offset={} size={}",
           options.compare_iso_path, byte_offset, buffer.size());
    std::fprintf(stderr,
                 "Comparing against ISO: path=%s offset=%llu size=%zu\n",
                 options.compare_iso_path.c_str(),
                 static_cast<unsigned long long>(byte_offset), buffer.size());

    std::vector<uint8_t> iso_buffer;
    if (!LoadIsoRange(options.compare_iso_path, byte_offset, buffer.size(),
                      &iso_buffer)) {
      std::fprintf(stderr, "FAIL: ISO compare I/O failed.\n");
      return static_cast<int>(ProbeExitCode::kCompareIoFailure);
    }

    if (!CompareAndReport(buffer, iso_buffer)) {
      std::fprintf(stderr, "FAIL: compare mismatch.\n");
      return static_cast<int>(ProbeExitCode::kCompareFailure);
    }
  }

  std::fprintf(stdout, "SUCCESS: probe completed successfully.\n");
  return static_cast<int>(ProbeExitCode::kSuccess);
}

}  // namespace xe::vfs

XE_DEFINE_CONSOLE_APP_TRANSPARENT("xenia-vfs-omnidrive-sg-probe",
                                  xe::vfs::omnidrive_sg_probe_main);
