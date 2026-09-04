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

#include <algorithm>
#include <cstdint>

#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// One EVENT_WRITE_ZPD occlusion query sample counter report.
//
// Z-Pass Done (ZPD) reports are a headache to emulate for a few reasons:
//
// 1. D3D has two ZPD occlusion query APIs, and some titles use both.
//    - A conventional query brackets a draw interval within BEGIN and END calls
//      then reads the result back with GetData.
//    - QueryBatch writes a cumulative counter snapshot for every Issue call.
//      Lock readies the snapshot and results are gotten by subtracting adjacent
//      slots. So N intervals need N + 1 reports:
//        Issue, draw_A, Issue, draw_B, Issue
//
// 2. Xenos doesn't have a hardware counter for each query. EVENT_WRITE_ZPD
//    writes the counters to RB_SAMPLE_COUNT_ADDR, and D3D subtracts the BEGIN
//    report from the END report in software to get the sample counts. So we
//    have to track every interval between writes, including ones that aren't
//    bracketed by BEGIN and END.
//
// 3. Each report contains four counters, each with A and B lanes:
//    - ZFail: samples that fail depth
//    - ZPass: samples that pass depth
//    - StencilFail: samples that fail stencil
//    - Total: ZFail + ZPass + StencilFail
//
//    D3D sums A and B. The exact meaning of the A/B lane split still isn't
//    known. But for every 50 titles that merely ask for the summed ZPass,
//    there's one example like 425307EC that masks each lane to 24 bits before
//    summing, so we need to evenly split the counts. Samples rejected by hi-Z
//    or hi-stencil aren't included in ZFail or StencilFail respectively.
//
// Modern PC GPUs can only count ZPass. For finicky titles that need the other
// counts, ROV & FSI can count them all in shader at the cost of heavy atomics
// (see occlusion_query_full_counters) and RTV & FBO can supplement ZPass with
// pre-test coverage for an approximate Total, but can't distinguish ZFail from
// StencilFail.
struct XenosZPDReport {
  // Host counter buffer slot layout, one slot per pool query index.
  enum CounterIndex : uint32_t {
    kTotal,
    kZFail,
    kZPass,
    kStencilFail,
    kCount,
  };
  static constexpr uint32_t kCounterSizeBytes = kCount * sizeof(uint32_t);

  uint64_t z_fail = 0;
  uint64_t z_pass = 0;
  uint64_t stencil_fail = 0;
  uint64_t total() const { return z_fail + z_pass + stencil_fail; }

  bool operator==(const XenosZPDReport& other) const = default;

  XenosZPDReport& operator+=(const XenosZPDReport& other) {
    z_fail += other.z_fail;
    z_pass += other.z_pass;
    stencil_fail += other.stencil_fail;
    return *this;
  }

  // Native host occlusion query. ZPass only.
  static XenosZPDReport FromNativeQuery(uint64_t passed) {
    XenosZPDReport report;
    report.z_pass = passed;
    return report;
  }
  // Native query ZPass plus the pre-test coverage counted from Total, if
  // occlusion_query_full_counters.
  // Rejected samples go in ZFail - the host can't tell them from StencilFail.
  static XenosZPDReport FromNativeQueryAndTotal(uint64_t passed,
                                                uint64_t coverage) {
    XenosZPDReport report;
    report.z_pass = passed;
    report.z_fail = std::max(coverage, passed) - passed;
    return report;
  }

  // Counter slot filled by the interlock shaders.
  static XenosZPDReport FromCounterSlot(const uint32_t* slot) {
    XenosZPDReport report;
    report.z_fail = slot[kZFail];
    report.z_pass = slot[kZPass];
    report.stencil_fail = slot[kStencilFail];
    return report;
  }

  // Divides host counts by the draw scale area, rounding to nearest.
  XenosZPDReport Normalized(uint32_t scale_area) const {
    auto normalize = [scale_area](uint64_t count) {
      return scale_area <= 1 || !count
                 ? count
                 : std::max<uint64_t>(1,
                                      (count + (scale_area >> 1)) / scale_area);
    };
    XenosZPDReport report;
    report.z_fail = normalize(z_fail);
    report.z_pass = normalize(z_pass);
    report.stencil_fail = normalize(stencil_fail);
    return report;
  }

  // Writes the report to guest memory, each counter split across the A and B
  // lanes. Low 32 bits only, the hardware counters wrap and so do we.
  // GetData (usually) wakes on the ZPass lanes and QueryBatch Lock on ZPass_A
  // or StencilFail_B, so those four are written last, in one copy.
  void WriteTo(xenos::xe_gpu_depth_sample_counts* guest) const {
    auto lane_a = [](uint64_t count) {
      return uint32_t(count) - (uint32_t(count) >> 1);
    };
    auto lane_b = [](uint64_t count) { return uint32_t(count) >> 1; };
    guest->Total_A = lane_a(total());
    guest->Total_B = lane_b(total());
    guest->ZFail_A = lane_a(z_fail);
    guest->ZFail_B = lane_b(z_fail);
    xenos::xe_gpu_depth_sample_counts values;
    values.ZPass_A = lane_a(z_pass);
    values.ZPass_B = lane_b(z_pass);
    values.StencilFail_A = lane_a(stencil_fail);
    values.StencilFail_B = lane_b(stencil_fail);
    std::memcpy(
        &guest->ZPass_A, &values.ZPass_A,
        sizeof(values) - offsetof(xenos::xe_gpu_depth_sample_counts, ZPass_A));
  }
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_XENOS_ZPD_REPORT_H_
