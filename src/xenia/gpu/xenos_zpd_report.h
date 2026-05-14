/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_XENOS_ZPD_REPORT_H_
#define XENIA_GPU_XENOS_ZPD_REPORT_H_

#include <cstddef>
#include <cstdint>

#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// Guest memory helpers for occlusion query ZPD reports.
struct XenosZPDReport {
  static constexpr uint32_t kRecordSizeBytes = 0x20;
  static constexpr uint32_t kRecordAlignMask = ~(kRecordSizeBytes - 1);

  // Each slot holds one BEGIN record and one END record.
  // END is at the slot base, and BEGIN is +0x20.
  static constexpr uint32_t kSlotSizeBytes = 0x40;
  static constexpr uint32_t kSlotAlignMask = ~(kSlotSizeBytes - 1);

  static constexpr uint32_t GetRecordBase(uint32_t address) {
    return address & kRecordAlignMask;
  }

  static constexpr uint32_t GetSlotBase(uint32_t address) {
    return address & kSlotAlignMask;
  }

  static constexpr uint32_t GetBeginRecordBase(uint32_t address) {
    return GetSlotBase(address) + kRecordSizeBytes;
  }

  static constexpr uint32_t GetEndRecordBase(uint32_t address) {
    return GetSlotBase(address);
  }

  static constexpr bool IsBeginRecord(uint32_t address) {
    uint32_t record_base = GetRecordBase(address);
    return record_base && record_base == GetBeginRecordBase(record_base);
  }

  static constexpr bool IsEndRecord(uint32_t address) {
    uint32_t record_base = GetRecordBase(address);
    return record_base && record_base == GetEndRecordBase(record_base);
  }

  // Boundary detection only looks at ZPass_A first, then ZFail_A.
  // Some titles (4D5307E8) have unique, non-zero values in the B fields, which
  // aren't understood well enough to count them yet.
  // Proper support might need separate queries for both A & B.
  static bool HasPendingSentinel(
      const xenos::xe_gpu_depth_sample_counts* report) {
    constexpr uint32_t kSentinelLE = 0xEDFEFFFFu;
    constexpr uint32_t kSentinelBE = 0xFFFFFEEDu;

    if (report->ZPass_A == kSentinelLE || report->ZPass_A == kSentinelBE) {
      return true;
    }
    if (report->ZFail_A == kSentinelLE || report->ZFail_A == kSentinelBE) {
      return true;
    }
    return false;
  }

  // Total_A mirrors ZPass_A and the rest are zeroed out since host queries can
  // only provide a passing count. This is still enough to satisfy most titles.
  static void WriteSampleCount(xenos::xe_gpu_depth_sample_counts* report,
                               uint32_t sample_count, bool saturate = true) {
    if (saturate) {
      sample_count = SaturateSampleCount(sample_count);
    }

    report->Total_A = sample_count;
    report->Total_B = 0;
    report->ZFail_A = 0;
    report->ZFail_B = 0;
    report->ZPass_A = sample_count;
    report->ZPass_B = 0;
    report->StencilFail_A = 0;
    report->StencilFail_B = 0;
  }

  static uint32_t SaturateSampleCount(uint32_t sample_count) {
    double saturation = std::clamp(
        static_cast<double>(cvars::occlusion_query_sample_count_saturation),
        0.0, 1.0);

    if (sample_count == 0 || saturation >= 1.0) {
      return sample_count;
    }
    if (saturation <= 0.0) {
      return 1;
    }

    // Preserve lower sample counts and only compress the higher range. A knee
    // of 32 is a conservative, non-authoritative threshold, acting as a safety
    // valve for titles using occlusion culling. May need to be revisited.
    const double knee = 32.0;
    if (static_cast<double>(sample_count) <= knee) {
      return sample_count;
    }

    const double attenuation = 1.0 - saturation;
    const double exponent = 1.0 - (1.0 - 0.35) * (attenuation * attenuation *
                                                  (3.0 - 2.0 * attenuation));
    double saturated_count =
        knee + std::pow(static_cast<double>(sample_count) - knee, exponent);

    return static_cast<uint32_t>(saturated_count + 0.5);
  }

  // Fake mode for titles (425307EC, 4D5309B1) that use QueryBatch and expect
  // the sample count to accumulate across multiple records.
  static uint32_t QueryBatchFakeSamples(uint32_t& sample_count) {
    int32_t lower = cvars::occlusion_query_fake_lower_threshold;
    uint32_t base = lower > 0 ? static_cast<uint32_t>(lower) : 0;
    uint32_t range =
        static_cast<uint32_t>(cvars::occlusion_query_querybatch_range);

    if (sample_count - base >= range) {
      sample_count = base;
    }

    uint32_t current_sample_count = sample_count++;
    if (sample_count - base >= range) {
      sample_count = base;
    }
    return current_sample_count;
  }

  static void WriteReportDelta(xenos::xe_gpu_depth_sample_counts* begin_report,
                               xenos::xe_gpu_depth_sample_counts* end_report,
                               uint32_t begin_value, uint32_t delta_value,
                               bool write_begin_report) {
    delta_value = SaturateSampleCount(delta_value);
    uint32_t end_value = begin_value + delta_value;

    if (write_begin_report && begin_report && end_report != begin_report) {
      WriteSampleCount(begin_report, begin_value, false);
    }
    WriteSampleCount(end_report, end_value, false);
  }
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_XENOS_ZPD_REPORT_H_
