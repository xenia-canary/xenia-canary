/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_video.h"

#include "xenia/base/byte_order_mac.h"
#include "xenia/base/clock.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator_mac.h"
#include "xenia/gpu/command_processor_mac.h"
#include "xenia/gpu/graphics_system_mac.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules_mac.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_rtl.h"
#include "xenia/kernel/xthread_mac.h"
#include "xenia/xbox.h"

DEFINE_int32(
    video_standard, 1,
    "Enables switching between different video signals.\n   1=NTSC\n   "
    "2=NTSC-J\n   3=PAL\n",
    "Video");

DEFINE_bool(use_50Hz_mode, false, "Enables usage of PAL-50 mode.", "Video");
DEFINE_bool(interlaced, false, "Toggles interlaced mode.", "Video");
DEFINE_bool(
    video_force_ringbuffer_kick_on_vdswap, false,
    "Debug: Force a ring buffer write pointer update when VdSwap writes "
    "swap commands. Use only for debugging missing CP_RB_WPTR writes.",
    "Video");
DEFINE_uint32(video_force_ringbuffer_kick_on_vdswap_if_stale_ms, 0,
              "Debug: Force a ring buffer write pointer update on VdSwap if "
              "no CP_RB_WPTR update has been observed within this time window "
              "(milliseconds). 0 disables the fallback.",
              "Video");
DEFINE_uint32(
    video_force_asic_id, 0,
    "Debug: Force VdGetGraphicsAsicID to return this value when non-zero.",
    "Video");
DEFINE_bool(video_log_vdswap_verbose, true,
            "Log detailed VdSwap activity and swap packet contents.", "Video");
DEFINE_int32(video_log_vdswap_every_n, 1,
             "Log every N VdSwap calls when verbose logging is enabled.",
             "Video");
DEFINE_bool(video_log_vdswap_dump_ring, true,
            "Dump VdSwap ring buffer packet dwords after writing.", "Video");
DEFINE_bool(video_log_vdswap_dump_fetch, true,
            "Dump VdSwap GPU fetch constant contents.", "Video");
DEFINE_int32(video_log_vdswap_dump_dwords, 64,
             "Number of dwords to dump from the VdSwap ring buffer packet.",
             "Video");
DEFINE_bool(video_log_vd_display_queries, true,
            "Log display mode and video query calls.", "Video");
DEFINE_bool(video_log_vd_system_cmd_buffer, true,
            "Log system command buffer calls and stub values.", "Video");
DEFINE_bool(video_log_vd_callbacks, true,
            "Log VdSetGraphicsInterruptCallback and related calls.", "Video");
DEFINE_bool(video_log_vd_breadcrumbs, true,
            "Extra verbose breadcrumbs for Vd* GPU initialization paths.",
            "Video");
DEFINE_bool(video_force_vd_global_device, false,
            "Debug: Force VdGlobalDevice to a non-zero dummy allocation.",
            "Video");
DEFINE_uint32(video_force_vd_global_device_size, 256,
              "Debug: Size of the dummy VdGlobalDevice allocation (bytes).",
              "Video");
DEFINE_bool(video_force_vd_global_xam_device, false,
            "Debug: Force VdGlobalXamDevice to a non-zero dummy allocation.",
            "Video");
DEFINE_uint32(video_force_vd_global_xam_device_size, 256,
              "Debug: Size of the dummy VdGlobalXamDevice allocation (bytes).",
              "Video");
DEFINE_bool(
    video_allow_bootstrap_fallback_hacks, false,
    "Debug: Allow fallback initialization nudges from Vd* handlers when "
    "guest bootstrap is missing (disabled by default).",
    "Video");
DEFINE_bool(video_log_vd_edram_training, true,
            "Debug: Log VdInitializeEDRAM / VdRetrainEDRAM / HSIO training "
            "path calls with caller context.",
            "Video");
DEFINE_int32(
    video_vd_edram_stub_return, 1,
    "Return value for VdInitializeEDRAM / VdRetrainEDRAM / "
    "VdRetrainEDRAMWorker stubs (1 is often expected by bool-based callers).",
    "Video");
DECLARE_bool(trace_gpu_bootstrap);

namespace {
const char* BoolToString(bool value) { return value ? "YES" : "NO"; }

void LogCommandProcessorBreadcrumb(const char* tag,
                                   xe::gpu::GraphicsSystem* graphics_system) {
  if (!cvars::video_log_vd_breadcrumbs) {
    return;
  }
  if (!graphics_system) {
    XELOGI("DEBUG: BREADCRUMB: {} graphics_system=<null>", tag ? tag : "Vd");
    return;
  }
  auto* cp = graphics_system->command_processor();
  if (!cp) {
    XELOGI("DEBUG: BREADCRUMB: {} command_processor=<null>", tag ? tag : "Vd");
    return;
  }
  auto dbg = cp->GetRingBufferDebugState();
  XELOGI("DEBUG: BREADCRUMB: {} CP state", tag ? tag : "Vd");
    XELOGI(
        "cp.worker_running={} setup_complete={} setup_failed={} "
        "ring_buffer_initialized={} init_ack={}",
         dbg.worker_running ? "YES": "NO",
      dbg.worker_setup_complete ? "YES": "NO",
      dbg.worker_setup_failed ? "YES": "NO",
         dbg.ring_buffer_initialized ? "YES": "NO",
         dbg.initialization_acknowledged ? "YES": "NO");
  XELOGI("cp.primary_buffer_ptr=0x{:08X} size=0x{:08X}",
         dbg.primary_buffer_ptr, dbg.primary_buffer_size);
  XELOGI(
      "cp.read_ptr=0x{:08X} write_ptr=0x{:08X} rptr_wb=0x{:08X} "
      "rptr_freq=0x{:08X}",
      dbg.read_ptr_index, dbg.write_ptr_index, dbg.read_ptr_writeback_ptr,
      dbg.read_ptr_update_freq);
  XELOGI("cp.swap_packets={} last_swap_ms={} frontbuffer=0x{:08X}",
         dbg.swap_packet_count, dbg.last_swap_packet_ms,
         dbg.last_swap_frontbuffer_ptr);
  XELOGI("cp.swap_size={}x{}", dbg.last_swap_width, dbg.last_swap_height);
  XELOGI("cp.ring_watch_hits={} last_write_ms={} last_write_phys=0x{:08X}",
         dbg.ring_buffer_watch_hits, dbg.ring_buffer_last_write_ms,
         dbg.ring_buffer_last_write_phys);
  XELOGI("cp.events: write_ptr_event={} init_complete_event={}",
         dbg.has_write_ptr_event ? "YES": "NO",
         dbg.has_init_complete_event ? "YES": "NO");
}

void DumpDwords(const char* label, const void* data, uint32_t count) {
  if (!data || !count) {
    XELOGI("{}: <empty>", label ? label : "dwords");
    return;
  }
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  XELOGI("{} ({} dwords):", label ? label : "dwords", count);
  for (uint32_t i = 0; i < count; i += 8) {
    uint32_t end = std::min<uint32_t>(i + 7, count - 1);
    uint32_t values[8] = {};
    for (uint32_t j = 0; j < 8; ++j) {
      uint32_t index = i + j;
      if (index < count) {
        values[j] =
            xe::load_and_swap<uint32_t>(bytes + index * sizeof(uint32_t));
      }
    }
    XELOGI(
        "[{:02X}-{:02X}] {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
        "{:08X}",
        i, end, values[0], values[1], values[2], values[3], values[4],
        values[5], values[6], values[7]);
  }
}

void DumpRingBufferWindow(const char* label, const void* data,
                          uint32_t ring_dwords, uint32_t start_dword,
                          uint32_t count) {
  if (!data || ring_dwords == 0 || count == 0) {
    XELOGI("{}: <invalid ring buffer>", label ? label : "ring buffer");
    return;
  }
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  uint32_t clamped_count = std::min<uint32_t>(count, ring_dwords);
  XELOGI("{} window (start={}, count={}, ring_dwords={}):",
         label ? label : "ring buffer", start_dword, clamped_count,
         ring_dwords);
  for (uint32_t i = 0; i < clamped_count; ++i) {
    uint32_t dword_index = (start_dword + i) % ring_dwords;
    uint32_t value =
        xe::load_and_swap<uint32_t>(bytes + dword_index * sizeof(uint32_t));
    XELOGI("rb[{:04X}] = {:08X}", dword_index, value);
  }
}

void DumpSwapFetch(const xe::gpu::xenos::xe_gpu_texture_fetch_t& fetch,
                   const char* label) {
  if (!cvars::video_log_vdswap_dump_fetch) {
    return;
  }
  XELOGI("{} fetch dwords:", label ? label : "VdSwap");
  XELOGI("dword_0=0x{:08X}", fetch.dword_0);
  XELOGI("dword_1=0x{:08X}", fetch.dword_1);
  XELOGI("dword_2=0x{:08X}", fetch.dword_2);
  XELOGI("dword_3=0x{:08X}", fetch.dword_3);
  XELOGI("dword_4=0x{:08X}", fetch.dword_4);
  XELOGI("dword_5=0x{:08X}", fetch.dword_5);
  XELOGI("size_2d.width={} height={} stack_depth={}", fetch.size_2d.width,
         fetch.size_2d.height, fetch.size_2d.stack_depth);
  XELOGI("format=0x{:X} endianness=0x{:X} swizzle=0x{:X}",
         static_cast<uint32_t>(fetch.format),
         static_cast<uint32_t>(fetch.endianness), fetch.swizzle);
  XELOGI("base_address=0x{:08X} mip_address=0x{:08X}", fetch.base_address,
         fetch.mip_address);
  XELOGI("pitch=0x{:X} tiled={} packed_mips={}", fetch.pitch,
         BoolToString(fetch.tiled), BoolToString(fetch.packed_mips));
}

void GetCurrentThreadPcAndId(uint32_t* pc_out, uint32_t* thread_id_out) {
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  auto* thread = xe::kernel::XThread::GetCurrentThread();
  if (thread) {
    thread_id = thread->thread_id();
    auto* thread_state = thread->thread_state();
    auto* ctx = thread_state ? thread_state->context() : nullptr;
    if (ctx) {
      pc = ctx->current_pc;
    }
  }
  if (pc_out) {
    *pc_out = pc;
  }
  if (thread_id_out) {
    *thread_id_out = thread_id;
  }
}

uint32_t GetCurrentThreadLr() {
  auto* thread = xe::kernel::XThread::GetCurrentThread();
  if (!thread) {
    return 0;
  }
  auto* thread_state = thread->thread_state();
  auto* ctx = thread_state ? thread_state->context() : nullptr;
  return ctx ? static_cast<uint32_t>(ctx->lr) : 0;
}

void RecordTrackedGpuExportEntry(uint16_t ordinal, uint32_t pc,
                                 uint32_t thread_id) {
  xe::kernel::xboxkrnl::RecordGpuBootstrapExportEntry(
      ordinal, pc, GetCurrentThreadLr(), thread_id);
}
}  // namespace

// TODO: This is stored in XConfig somewhere, probably in video flags.
DEFINE_bool(widescreen, true, "Toggles between 16:9 and 4:3 aspect ratio.",
            "Video");

// BT.709 on modern monitors and TVs looks the closest to the Xbox 360 connected
// to an HDTV.
DEFINE_uint32(kernel_display_gamma_type, 2,
              "Display gamma type: 0 - linear, 1 - sRGB (CRT), 2 - BT.709 "
              "(HDTV), 3 - power specified via kernel_display_gamma_power.",
              "Kernel");
UPDATE_from_uint32(kernel_display_gamma_type, 2020, 12, 31, 13, 1);
DEFINE_double(kernel_display_gamma_power, 2.22222233,
              "Display gamma to use with kernel_display_gamma_type 3.",
              "Kernel");

inline const static uint32_t GetVideoStandard() {
  if (cvars::video_standard < 1 || cvars::video_standard > 3) {
    return 1;
  }

  return cvars::video_standard;
}

inline const static float GetVideoRefreshRate() {
  return cvars::use_50Hz_mode ? 50.0f : 60.0f;
}

inline const static std::pair<uint16_t, uint16_t> GetDisplayAspectRatio() {
  if (cvars::widescreen) {
    return {16, 9};
  }

  return {4, 3};
}

static std::pair<uint32_t, uint32_t> CalculateScaledAspectRatio(uint32_t fb_x,
                                                                uint32_t fb_y) {
  // Calculate the game's final aspect ratio as it would appear on a physical
  // TV.
  auto dar = GetDisplayAspectRatio();
  uint32_t display_x = dar.first;
  uint32_t display_y = dar.second;

  auto res = xe::gpu::GraphicsSystem::GetInternalDisplayResolution();
  uint32_t res_x = res.first;
  uint32_t res_y = res.second;

  uint32_t x_factor = std::gcd(fb_x, res_x);
  res_x /= x_factor;
  fb_x /= x_factor;
  uint32_t y_factor = std::gcd(fb_y, res_y);
  res_y /= y_factor;
  fb_y /= y_factor;

  display_x = display_x * res_x - display_x * (res_x - fb_x);
  display_y *= res_x;

  display_y = display_y * res_y - display_y * (res_y - fb_y);
  display_x *= res_y;

  uint32_t aspect_factor = std::gcd(display_x, display_y);
  display_x /= aspect_factor;
  display_y /= aspect_factor;

  XELOGI(
      "Hardware scaler: width ratio {}:{}, height ratio {}:{}, final aspect "
      "ratio {}:{}",
      fb_x, res_x, fb_y, res_y, display_x, display_y);

  return {display_x, display_y};
}

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {
std::atomic<uint64_t> g_vd_swap_calls{0};
std::atomic<uint64_t> g_vd_init_rb_calls{0};
std::atomic<uint64_t> g_vd_rptr_calls{0};
std::atomic<uint64_t> g_vd_get_sys_cb_calls{0};
std::atomic<uint64_t> g_vd_set_sys_cb_calls{0};
std::atomic<bool> g_vd_interrupt_callback_set{false};
std::atomic<uint32_t> g_vd_interrupt_callback{0};
std::atomic<uint32_t> g_vd_interrupt_callback_arg{0};
std::atomic<uint32_t> g_vd_interrupt_callback_thread{0};
}  // namespace

const char* ResolveCallerModuleName(uint32_t pc) {
  if (!pc) {
    return "<unknown>";
  }
  auto* ks = kernel_state();
  auto* processor = ks ? ks->processor() : nullptr;
  auto* module = processor ? processor->LookupModule(pc) : nullptr;
  return module ? module->name().c_str() : "<unknown>";
}

// https://web.archive.org/web/20150805074003/https://www.tweakoz.com/orkid/
// http://www.tweakoz.com/orkid/dox/d3/d52/xb360init_8cpp_source.html
// https://github.com/Free60Project/xenosfb/
// https://github.com/Free60Project/xenosfb/blob/master/src/xe.h
// https://github.com/gligli/libxemit
// https://web.archive.org/web/20090428095215/https://msdn.microsoft.com/en-us/library/bb313877.aspx
// https://web.archive.org/web/20100423054747/https://msdn.microsoft.com/en-us/library/bb313961.aspx
// https://web.archive.org/web/20100423054747/https://msdn.microsoft.com/en-us/library/bb313878.aspx
// https://web.archive.org/web/20090510235238/https://msdn.microsoft.com/en-us/library/bb313942.aspx
// https://svn.dd-wrt.com/browser/src/linux/universal/linux-3.8/drivers/gpu/drm/radeon/radeon_ring.c?rev=21595
// https://www.microsoft.com/en-za/download/details.aspx?id=5313 -- "Stripped
// Down Direct3D: Xbox 360 Command Buffer and Resource Management"

// Legacy VdGetGraphicsAsicID entry (pointer variant) removed to avoid
// duplicate export registration. Use the dword_result_t variant below.

void VdGetCurrentDisplayGamma_entry(lpdword_t type_ptr, lpfloat_t power_ptr) {
  // 1 - sRGB.
  // 2 - TV (BT.709).
  // 3 - use the power written to *power_ptr.
  // Anything else - linear.
  // Used in D3D SetGammaRamp/SetPWLGamma to adjust the ramp for the display.
  *type_ptr = cvars::kernel_display_gamma_type;
  *power_ptr = float(cvars::kernel_display_gamma_power);
}
DECLARE_XBOXKRNL_EXPORT1(VdGetCurrentDisplayGamma, kVideo, kStub);

struct X_D3DPRIVATE_RECT {
  xe::be<uint32_t> x1;  // 0x0
  xe::be<uint32_t> y1;  // 0x4
  xe::be<uint32_t> x2;  // 0x8
  xe::be<uint32_t> y2;  // 0xC
};
static_assert_size(X_D3DPRIVATE_RECT, 0x10);

struct X_D3DFILTER_PARAMETERS {
  xe::be<float> nyquist;         // 0x0
  xe::be<float> flicker_filter;  // 0x4
  xe::be<float> beta;            // 0x8
};
static_assert_size(X_D3DFILTER_PARAMETERS, 0xC);

struct X_D3DPRIVATE_SCALER_PARAMETERS {
  X_D3DPRIVATE_RECT scaler_source_rect;                 // 0x0
  xe::be<uint32_t> scaled_output_width;                 // 0x10
  xe::be<uint32_t> scaled_output_height;                // 0x14
  xe::be<uint32_t> vertical_filter_type;                // 0x18
  X_D3DFILTER_PARAMETERS vertical_filter_parameters;    // 0x1C
  xe::be<uint32_t> horizontal_filter_type;              // 0x28
  X_D3DFILTER_PARAMETERS horizontal_filter_parameters;  // 0x2C
};
static_assert_size(X_D3DPRIVATE_SCALER_PARAMETERS, 0x38);

struct X_DISPLAY_INFO {
  xe::be<uint16_t> front_buffer_width;               // 0x0
  xe::be<uint16_t> front_buffer_height;              // 0x2
  // uint8_t front_buffer_color_format;              // 0x4
  // uint8_t front_buffer_pixel_format;              // 0x5
  uint32_t : 32;                                      // 0x4
  X_D3DPRIVATE_SCALER_PARAMETERS scaler_parameters;  // 0x8
  xe::be<uint16_t> display_window_overscan_left;     // 0x40
  xe::be<uint16_t> display_window_overscan_top;      // 0x42
  xe::be<uint16_t> display_window_overscan_right;    // 0x44
  xe::be<uint16_t> display_window_overscan_bottom;   // 0x46
  xe::be<uint16_t> display_width;                    // 0x48
  xe::be<uint16_t> display_height;                   // 0x4A
  xe::be<float> display_refresh_rate;                // 0x4C
  xe::be<uint32_t> display_interlaced;               // 0x50
  // uint8_t display_color_format;                   // 0x54
  uint16_t : 16;                                      // 0x54
  xe::be<uint16_t> actual_display_width;             // 0x56
};
static_assert_size(X_DISPLAY_INFO, 0x58);

void VdGetCurrentDisplayInformation_entry(
    pointer_t<X_DISPLAY_INFO> display_info) {
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode, false);

  display_info.Zero();
  display_info->front_buffer_width = (uint16_t)mode.display_width;
  display_info->front_buffer_height = (uint16_t)mode.display_height;

  display_info->scaler_parameters.scaler_source_rect.x2 = mode.display_width;
  display_info->scaler_parameters.scaler_source_rect.y2 = mode.display_height;
  display_info->scaler_parameters.scaled_output_width = mode.display_width;
  display_info->scaler_parameters.scaled_output_height = mode.display_height;
  display_info->scaler_parameters.horizontal_filter_type = 1;
  display_info->scaler_parameters.vertical_filter_type = 1;

  display_info->display_window_overscan_left = 320;
  display_info->display_window_overscan_top = 180;
  display_info->display_window_overscan_right = 320;
  display_info->display_window_overscan_bottom = 180;
  display_info->display_width = (uint16_t)mode.display_width;
  display_info->display_height = (uint16_t)mode.display_height;
  display_info->display_refresh_rate = mode.refresh_rate;
  display_info->display_interlaced = mode.is_interlaced;
  display_info->actual_display_width = (uint16_t)mode.display_width;
  if (cvars::video_log_vd_display_queries &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    auto* thread = XThread::GetCurrentThread();
    XELOGI("DEBUG: VdGetCurrentDisplayInformation called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("display {}x{} refresh={} interlaced={} widescreen={}",
           uint32_t(mode.display_width), uint32_t(mode.display_height),
           float(mode.refresh_rate),
           uint32_t(mode.is_interlaced) ? "YES": "NO",
           uint32_t(mode.is_widescreen) ? "YES": "NO");
  }
}
DECLARE_XBOXKRNL_EXPORT1(VdGetCurrentDisplayInformation, kVideo, kStub);

void VdQueryVideoMode(X_VIDEO_MODE* video_mode,
                      [[maybe_unused]] bool is_internal_resolution) {
  // TODO(benvanik): get info from actual display.
  std::memset(video_mode, 0, sizeof(X_VIDEO_MODE));

  auto display_res = gpu::GraphicsSystem::GetInternalDisplayResolution();

  video_mode->display_width = display_res.first;
  video_mode->display_height = display_res.second;
  video_mode->is_interlaced = cvars::interlaced;
  video_mode->is_widescreen = cvars::widescreen;
  video_mode->is_hi_def = video_mode->display_width >= 0x500;
  video_mode->refresh_rate = GetVideoRefreshRate();
  video_mode->video_standard = GetVideoStandard();
  video_mode->pixel_rate = 0x8A;
  video_mode->widescreen_flag = cvars::widescreen ? 0x01 : 0x03;
}

void VdQueryRealVideoMode_entry(pointer_t<X_VIDEO_MODE> video_mode) {
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  VdQueryVideoMode(video_mode, true);
  if (cvars::video_log_vd_display_queries &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    auto* thread = XThread::GetCurrentThread();
    XELOGI("DEBUG: VdQueryRealVideoMode called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("display {}x{} refresh={} interlaced={} widescreen={}",
           uint32_t(video_mode->display_width),
           uint32_t(video_mode->display_height),
           float(video_mode->refresh_rate),
           uint32_t(video_mode->is_interlaced) ? "YES": "NO",
           uint32_t(video_mode->is_widescreen) ? "YES": "NO");
  }
}
DECLARE_XBOXKRNL_EXPORT1(VdQueryRealVideoMode, kVideo, kStub);

void VdQueryVideoMode_entry(pointer_t<X_VIDEO_MODE> video_mode) {
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  VdQueryVideoMode(video_mode, false);
  if (cvars::video_log_vd_display_queries &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    auto* thread = XThread::GetCurrentThread();
    XELOGI("DEBUG: VdQueryVideoMode called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("display {}x{} refresh={} interlaced={} widescreen={}",
           uint32_t(video_mode->display_width),
           uint32_t(video_mode->display_height),
           float(video_mode->refresh_rate),
           uint32_t(video_mode->is_interlaced) ? "YES": "NO",
           uint32_t(video_mode->is_widescreen) ? "YES": "NO");
  }
}
DECLARE_XBOXKRNL_EXPORT1(VdQueryVideoMode, kVideo, kStub);

dword_result_t VdQueryVideoFlags_entry() {
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode, false);

  uint32_t flags = 0;
  flags |= mode.is_widescreen ? 1 : 0;
  flags |= mode.display_width >= 1280 ? 2 : 0;
  flags |= mode.display_width >= 1920 ? 4 : 0;

  if (cvars::video_log_vd_display_queries &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    auto* thread = XThread::GetCurrentThread();
    XELOGI("DEBUG: VdQueryVideoFlags called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("flags=0x{:08X} (widescreen={}, hd1280={}, hd1920={})", flags,
           mode.is_widescreen ? "YES": "NO",
           mode.display_width >= 1280 ? "YES": "NO",
           mode.display_width >= 1920 ? "YES": "NO");
  }
  return flags;
}
DECLARE_XBOXKRNL_EXPORT1(VdQueryVideoFlags, kVideo, kStub);

dword_result_t VdSetDisplayMode_entry(dword_t flags) {
  // Often 0x40000000.

  // 0?ccf000 00000000 00000000 000000r0

  // r: 0x00000002 |     1
  // f: 0x08000000 |    27
  // c: 0x30000000 | 28-29
  // ?: 0x40000000 |    30

  // r: 1 = Resolution is 720x480 or 720x576
  // f: 1 = Texture format is k_2_10_10_10 or k_2_10_10_10_AS_16_16_16_16
  // c: Color space (0 = RGB, 1 = ?, 2 = ?)
  // ?: (always set?)

  // Return 1 (success) so games that check the return value proceed; returning
  // 0 can leave titles stuck in a wait loop with a black screen.
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  // uint64_t now_ms = xe::Clock::QueryHostUptimeMillis();
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  if (::cvars::trace_gpu_bootstrap) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x{:03X} "
        "name=VdSetDisplayMode args[r3]=0x{:08X}",
        ResolveCallerModuleName(pc), pc, thread_id,
        static_cast<unsigned int>(ordinals::VdSetDisplayMode), flags.value());
  }
  if (cvars::video_log_vd_display_queries &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    auto* thread = XThread::GetCurrentThread();
    XELOGI("DEBUG: VdSetDisplayMode called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("flags=0x{:08X}", flags.value());
  }
  return 1;
}
DECLARE_XBOXKRNL_EXPORT1(VdSetDisplayMode, kVideo, kStub);

dword_result_t VdSetDisplayModeOverride_entry(dword_t width, dword_t height,
                                              double_t refresh_rate,
                                              unknown_t unk3, unknown_t unk4) {
  // refresh_rate = 0, 50, 59.9, etc.
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  if (cvars::video_log_vd_display_queries &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    auto* thread = XThread::GetCurrentThread();
    XELOGI("DEBUG: VdSetDisplayModeOverride called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("width={} height={} refresh_rate={}", width.value(),
           height.value(), refresh_rate.value());
  }
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(VdSetDisplayModeOverride, kVideo, kStub);

dword_result_t VdInitializeEngines_entry(unknown_t unk0, function_t callback,
                                         lpvoid_t arg, lpdword_t pfp_ptr,
                                         lpdword_t me_ptr) {
  // r3 = 0x4F810000
  // r4 = function ptr (cleanup callback?) -> Likely interrupt callback in some
  // games? r5 = function arg r6 = PFP Microcode r7 = ME Microcode
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x01C2, pc, thread_id);  // VdInitializeEngines
  auto* thread = XThread::GetCurrentThread();
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (graphics_system) {
    graphics_system->NotifyVdInitializeEnginesCall(pc, thread_id);
  }
  XELOGW(
      "RING BUFFER: VdInitializeEngines call count={} pc={:08X} thread_id={} "
      "callback={:08X} arg={:08X} pfp_ptr={:08X} me_ptr={:08X}",
      call_id, pc, thread_id, callback.value(), arg.guest_address(),
      pfp_ptr.guest_address(), me_ptr.guest_address());

  if (cvars::video_log_vd_display_queries &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    XELOGI("DEBUG: VdInitializeEngines called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("unk0=0x{:08X} callback=0x{:08X} arg=0x{:08X}", unk0.value(),
           callback.value(), arg.guest_address());
    XELOGI("pfp_ptr=0x{:08X} me_ptr=0x{:08X}", pfp_ptr.guest_address(),
           me_ptr.guest_address());
  }

  XELOGI("DEBUG: BREADCRUMB: VdInitializeEngines called");

  // Attempt to register interrupt callback if provided
  if (callback) {
    if (graphics_system) {
      XELOGI(
          "Registering interrupt callback from VdInitializeEngines: "
          "0x{:08X}",
          callback.value());
      graphics_system->SetInterruptCallback(callback, arg);

      // Also notify for debugging
      graphics_system->NotifyVdSetGraphicsInterruptCallbackCall(pc, thread_id);
    }
  }

  return 1;
}
DECLARE_XBOXKRNL_EXPORT1(VdInitializeEngines, kVideo, kStub);

void VdShutdownEngines_entry() {
  // Ignored for now.
  // Games seem to call an Initialize/Shutdown pair to query info, then
  // re-initialize.
}
DECLARE_XBOXKRNL_EXPORT1(VdShutdownEngines, kVideo, kStub);

dword_result_t VdGetGraphicsAsicID_entry() {
  // Games compare for < 0x10 and do VdInitializeEDRAM, else other
  // (retrain/etc).
  uint32_t forced = cvars::video_force_asic_id;
  uint32_t asic_id = forced ? forced : 0x11;
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x01BC, pc, thread_id);  // VdGetGraphicsAsicID
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (graphics_system) {
    graphics_system->NotifyVdGetGraphicsAsicIDCall(pc, thread_id);
  }
  XELOGW(
      "RING BUFFER: VdGetGraphicsAsicID call count={} pc={:08X} thread_id={} "
      "forced={} return={:08X}",
      call_id, pc, thread_id, forced ? "YES": "NO", asic_id);
  if (call_id <= 5 || (call_id % 120) == 0) {
    auto* thread = XThread::GetCurrentThread();
    XELOGI("DEBUG: VdGetGraphicsAsicID called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    if (forced) {
      XELOGW("forcing ASIC ID override: 0x{:08X}", forced);
    }
    XELOGI("returning ASIC ID: 0x{:08X}", asic_id);
  }
  return asic_id;
}
DECLARE_XBOXKRNL_EXPORT1(VdGetGraphicsAsicID, kVideo, kStub);

dword_result_t VdEnableDisableClockGating_entry(dword_t enabled) {
  // Ignored, as it really doesn't matter.
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(VdEnableDisableClockGating, kVideo, kStub);

DEFINE_bool(video_force_set_graphics_interrupt_callback, false,
            "Debug: Force graphics interrupt callback registration.", "Video");
DEFINE_uint32(video_force_graphics_interrupt_callback_address, 0,
              "Debug: Override callback address for "
              "VdSetGraphicsInterruptCallback (0 = use guest).",
              "Video");

void VdSetGraphicsInterruptCallback_entry(function_t callback,
                                          lpvoid_t user_data) {
  // callback takes 2 params
  // r3 = bool 0/1 - 0 is normal interrupt, 1 is some acquire/lock mumble
  // r4 = user_data (r4 of VdSetGraphicsInterruptCallback)
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  uint64_t now_ms = xe::Clock::QueryHostUptimeMillis();
  uint32_t lr = GetCurrentThreadLr();
  RecordTrackedGpuExportEntry(0x01D5, pc,
                              thread_id);  // VdSetGraphicsInterruptCallback
  static std::atomic<bool> executed_once_logged{false};
  bool expected_exec_log = false;
  if (executed_once_logged.compare_exchange_strong(expected_exec_log, true,
                                                   std::memory_order_relaxed)) {
    XELOGW(
        "VdSetGraphicsInterruptCallback EXECUTED: cb={:08X} arg={:08X} "
        "thread={} pc={:08X} lr={:08X}",
        callback.value(), user_data.guest_address(), thread_id, pc, lr);
  }
  g_vd_interrupt_callback.store(callback.value(), std::memory_order_relaxed);
  g_vd_interrupt_callback_arg.store(user_data.guest_address(),
                                    std::memory_order_relaxed);
  g_vd_interrupt_callback_thread.store(thread_id, std::memory_order_relaxed);
  g_vd_interrupt_callback_set.store(callback.value() != 0,
                                    std::memory_order_relaxed);
  if (::cvars::trace_gpu_bootstrap) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x{:03X} "
        "name=VdSetGraphicsInterruptCallback args[r3..r4]=0x{:08X},0x{:08X}",
        ResolveCallerModuleName(pc), pc, thread_id,
        static_cast<unsigned int>(ordinals::VdSetGraphicsInterruptCallback), callback.value(),
        user_data.guest_address());
  }
  XELOGW(
      "RING BUFFER: VdSetGraphicsInterruptCallback call count={} pc={:08X} "
      "thread_id={} callback={:08X} user_data={:08X}",
      call_id, pc, thread_id, callback.value(), user_data.guest_address());
  if (::cvars::video_log_vd_callbacks &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    auto* thread = XThread::GetCurrentThread();
    XELOGI("DEBUG: VdSetGraphicsInterruptCallback called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("callback=0x{:08X} user_data=0x{:08X}", callback.value(),
           user_data.guest_address());
  }
  XELOGI(
      "DEBUG: BREADCRUMB: VdSetGraphicsInterruptCallback called to register "
      "0x{:08X}",
      callback.value());

  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (graphics_system) {
    auto* command_processor = graphics_system->command_processor();
    auto rb_state = command_processor
                        ? command_processor->GetRingBufferDebugState()
                        : xe::gpu::CommandProcessor::RingBufferDebugState{};
    uint64_t swap_packets =
        command_processor ? command_processor->swap_packet_count() : 0;
    uint32_t current_callback = graphics_system->GetInterruptCallback();
    if (call_id <= 5 || (call_id % 120) == 0) {
      XELOGW(
          "RING BUFFER: callback-exec timing call_id={} now_ms={} "
          "pc={:08X} thread_id={} ring_init={} init_ack={} "
          "rb_base={:08X} rb_size={:08X} swap_packets={} "
          "before_first_swap={} callback_already_set={}",
          call_id, now_ms, pc, thread_id,
          rb_state.ring_buffer_initialized ? "YES": "NO",
          rb_state.initialization_acknowledged ? "YES": "NO",
          rb_state.primary_buffer_ptr, rb_state.primary_buffer_size,
          swap_packets, swap_packets == 0 ? "YES": "NO",
          current_callback ? "YES": "NO");
    }
    static std::atomic<bool> first_callback_execution_logged{false};
    bool expected = false;
    if (first_callback_execution_logged.compare_exchange_strong(expected, true)) {
      XELOGW(
          "RING BUFFER: FIRST VdSetGraphicsInterruptCallback execution "
          "pc={:08X} thread_id={} callback={:08X} user_data={:08X} now_ms={} "
          "ring_init={} init_ack={} rb_base={:08X} rb_size={:08X} "
          "swap_packets_seen={} before_first_swap={}",
          pc, thread_id, callback.value(), user_data.guest_address(), now_ms,
          rb_state.ring_buffer_initialized ? "YES": "NO",
          rb_state.initialization_acknowledged ? "YES": "NO",
          rb_state.primary_buffer_ptr, rb_state.primary_buffer_size, swap_packets,
          swap_packets == 0 ? "YES": "NO");
      if (swap_packets != 0) {
        XELOGW(
            "RING BUFFER: callback-order warning: first "
            "VdSetGraphicsInterruptCallback happened after swap_packets={}",
            swap_packets);
      }
    }
    if (::cvars::video_log_vd_breadcrumbs) {
      XELOGI(
          "DEBUG: BREADCRUMB: VdSetGraphicsInterruptCallback ENTRY pc={:08X} "
          "thread_id={} cb=0x{:08X} user_data=0x{:08X} force_set={} "
          "force_cb=0x{:08X}",
          pc, thread_id, callback.value(), user_data.guest_address(),
          cvars::video_force_set_graphics_interrupt_callback ? "YES": "NO",
          cvars::video_force_graphics_interrupt_callback_address);
      LogCommandProcessorBreadcrumb("VdSetGraphicsInterruptCallback",
                                    graphics_system);
    }
    graphics_system->NotifyVdSetGraphicsInterruptCallbackCall(pc, thread_id,
                                  lr);
    uint32_t forced_cb = cvars::video_force_graphics_interrupt_callback_address;
    if (cvars::video_force_set_graphics_interrupt_callback && forced_cb) {
      XELOGW(
          "DEBUG: Forced VdSetGraphicsInterruptCallback: override cb=0x{:08X} "
          "(guest cb=0x{:08X})",
          forced_cb, callback.value());
      graphics_system->SetInterruptCallback(forced_cb, user_data);
    } else {
      graphics_system->SetInterruptCallback(callback, user_data);
    }
  } else {
    XELOGW("DEBUG: VdSetGraphicsInterruptCallback: graphics_system is null");
  }

  // Additional fallback check
  auto graphics_system_fallback = kernel_state()->emulator()->graphics_system();
  if (graphics_system_fallback &&
      graphics_system_fallback->GetInterruptCallback() == 0) {
    if (::cvars::video_allow_bootstrap_fallback_hacks) {
      XELOGW("DEBUG: Interrupt callback may not be set, attempting fallback");
      graphics_system_fallback->AttemptFallbackInitialization();
    } else if (::cvars::trace_gpu_bootstrap) {
      XELOGW(
          "DEBUG: Bootstrap trace: callback still unset after "
          "VdSetGraphicsInterruptCallback (fallback hacks disabled)");
    }
  }
}
DECLARE_XBOXKRNL_EXPORT1(VdSetGraphicsInterruptCallback, kVideo, kImplemented);

DEFINE_bool(video_force_init_ring_buffer_success, false,
            "Debug: Force VdInitializeRingBuffer success even with bad args.",
            "Video");
DEFINE_uint32(video_force_init_ring_buffer_size_log2, 18,
              "Debug: Forced ring buffer size_log2 for VdInitializeRingBuffer.",
              "Video");

void VdInitializeRingBuffer_entry(lpvoid_t ptr, int_t size_log2) {
  // r3 = result of MmGetPhysicalAddress
  // r4 = log2(size)
  // Buffer pointers are from MmAllocatePhysicalMemory with WRITE_COMBINE.

  uint64_t call_count = xboxkrnl::g_vd_init_rb_calls.fetch_add(1) + 1;
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x01C3, pc, thread_id);  // VdInitializeRingBuffer
  int32_t size_log2_value = size_log2.value();
  bool size_log2_valid = size_log2_value >= 0 && size_log2_value <= 28;
  uint32_t size_bytes = size_log2_valid
                            ? (uint32_t(1) << (size_log2_value + 3))
                            : 0;
  if (::cvars::trace_gpu_bootstrap) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x{:03X} "
        "name=VdInitializeRingBuffer args[r3..r4]=0x{:08X},0x{:08X}",
        ResolveCallerModuleName(pc), pc, thread_id,
        static_cast<unsigned int>(ordinals::VdInitializeRingBuffer), ptr.value(), size_log2.value());
  }
  XELOGW(
      "RING BUFFER: VdInitializeRingBuffer call count={} pc={:08X} "
      "thread_id={} ptr={:08X} size_log2={} size_bytes={:08X} size_valid={}",
      call_count, pc, thread_id, ptr.value(), size_log2_value, size_bytes,
      size_log2_valid ? "YES": "NO");
  auto* thread = XThread::GetCurrentThread();
  XELOGI("DEBUG: BREADCRUMB: VdInitializeRingBuffer called (count={})", call_count);
  XELOGI("DEBUG: CRITICAL: VdInitializeRingBuffer called from game (count={})",
         call_count);
  XELOGI("caller thread handle={:08X} id={} main={}",
         thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
         thread && thread->main_thread() ? "YES": "NO");
  XELOGI("ptr (physical addr) = {:08X}", ptr.value());
  XELOGI("size_log2 = {}", size_log2.value());
  XELOGI("Calculated size = {} bytes", size_bytes);
  if (::cvars::trace_gpu_bootstrap && ptr.value() != 0 && size_log2_valid &&
      size_log2_value > 0) {
    uint32_t rb_size = size_bytes;
    void* rb_host = kernel_memory()->TranslatePhysical(ptr.value());
    XELOGI(
        "DEBUG: VdInitializeRingBuffer mapping: rb_base={:08X} rb_size={:08X} "
        "host={} mapped={}",
        ptr.value(), rb_size, rb_host, rb_host ? "YES": "NO");
  }

  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (!graphics_system) {
    XELOGE("DEBUG: FATAL: graphics_system is NULL!");
    XELOGE("Ring buffer CANNOT be initialized - emulator state broken");
    if (!cvars::video_force_init_ring_buffer_success) {
      return;
    }
  }
  if (::cvars::video_log_vd_breadcrumbs) {
    XELOGI(
        "DEBUG: BREADCRUMB: VdInitializeRingBuffer ENTRY pc={:08X} thread_id={} "
        "ptr=0x{:08X} size_log2={} force_init={} force_size_log2={}",
        pc, thread_id, ptr.value(), size_log2.value(),
        cvars::video_force_init_ring_buffer_success ? "YES": "NO",
        cvars::video_force_init_ring_buffer_size_log2);
    LogCommandProcessorBreadcrumb("VdInitializeRingBuffer", graphics_system);
  }
  if (graphics_system) {
    graphics_system->NotifyVdInitializeRingBufferCall(pc, thread_id);
  }

  XELOGI("DEBUG: Calling graphics_system->InitializeRingBuffer...");
  if (graphics_system) {
    if (ptr.value() == 0 || size_log2.value() == 0) {
      if (cvars::video_force_init_ring_buffer_success) {
        uint32_t size_log2_override =
            cvars::video_force_init_ring_buffer_size_log2;
        uint32_t size_bytes = uint32_t(1) << (size_log2_override + 3);
        uint32_t forced_ptr = kernel_state()->memory()->SystemHeapAlloc(
            size_bytes, 4096, kSystemHeapPhysical);
        if (forced_ptr) {
          XELOGW("DEBUG: Forced VdInitializeRingBuffer: ptr=0x{:08X} size_log2={}",
                 forced_ptr, size_log2_override);
          graphics_system->InitializeRingBuffer(forced_ptr, size_log2_override);
        } else {
          XELOGE("DEBUG: Forced VdInitializeRingBuffer: allocation failed");
        }
      } else {
        XELOGE(
            "DEBUG: VdInitializeRingBuffer: invalid args ptr=0x{:08X} size_log2={}",
            ptr.value(), size_log2.value());
        return;
      }
    } else {
      graphics_system->InitializeRingBuffer(ptr, size_log2);
    }
    XELOGI("DEBUG: graphics_system->InitializeRingBuffer returned successfully");
    XELOGI("Ring buffer should now be initialized");
  } else if (cvars::video_force_init_ring_buffer_success) {
    XELOGW(
        "DEBUG: Forced VdInitializeRingBuffer: skipping due to null "
        "graphics_system");
  }

  // Additional fallback check if initialization failed
  auto* cp = graphics_system ? graphics_system->command_processor() : nullptr;
  if (cp && !cp->IsRingBufferInitialized()) {
    if (::cvars::video_allow_bootstrap_fallback_hacks) {
      XELOGW(
          "DEBUG: Ring buffer initialization may have failed, attempting fallback");
      graphics_system->AttemptFallbackInitialization();
    } else if (::cvars::trace_gpu_bootstrap) {
      XELOGW(
          "DEBUG: Bootstrap trace: ring buffer still not initialized after "
          "VdInitializeRingBuffer (fallback hacks disabled)");
    }
  }
}
DECLARE_XBOXKRNL_EXPORT1(VdInitializeRingBuffer, kVideo, kImplemented);

DEFINE_bool(video_force_enable_rptr_writeback, false,
            "Debug: Force VdEnableRingBufferRPtrWriteBack success.", "Video");

void VdEnableRingBufferRPtrWriteBack_entry(lpvoid_t ptr,
                                           int_t block_size_log2) {
  // r4 = log2(block size), 6, usually --- <=19
  uint64_t call_count = g_vd_rptr_calls.fetch_add(1) + 1;
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x01B6, pc,
                              thread_id);  // VdEnableRingBufferRPtrWriteBack
  if (::cvars::trace_gpu_bootstrap) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x{:03X} "
        "name=VdEnableRingBufferRPtrWriteBack args[r3..r4]=0x{:08X},0x{:08X}",
        ResolveCallerModuleName(pc), pc, thread_id,
        static_cast<unsigned int>(ordinals::VdEnableRingBufferRPtrWriteBack),
        ptr.value(), block_size_log2.value());
  }
  auto* thread = XThread::GetCurrentThread();
  XELOGI("DEBUG: BREADCRUMB: VdEnableRingBufferRPtrWriteBack called (count={})",
         call_count);
  XELOGI("DEBUG: CRITICAL: VdEnableRingBufferRPtrWriteBack called (count={})",
         call_count);
  XELOGI("caller thread handle={:08X} id={} main={}",
         thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
         thread && thread->main_thread() ? "YES": "NO");
  XELOGI("ptr (guest addr) = {:08X}", ptr.guest_address());
  XELOGI("block_size_log2 = {}", block_size_log2.value());
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  void* rptr_wb_host = nullptr;
  bool rptr_wb_host_mapped = false;
  if (ptr.value() != 0) {
    rptr_wb_host = kernel_memory()->TranslatePhysical(ptr.value());
    rptr_wb_host_mapped = rptr_wb_host != nullptr;
  }
  if (::cvars::trace_gpu_bootstrap) {
    XELOGI("DEBUG: Vd RPtr writeback mapping: ptr={:08X} host={} mapped={}",
           ptr.value(), rptr_wb_host, rptr_wb_host_mapped ? "YES": "NO");
  }
  if (::cvars::video_log_vd_breadcrumbs) {
    XELOGI(
        "DEBUG: BREADCRUMB: VdEnableRingBufferRPtrWriteBack ENTRY pc={:08X} "
        "thread_id={} ptr=0x{:08X} block_log2={} force_enable={}",
        pc, thread_id, ptr.guest_address(), block_size_log2.value(),
        cvars::video_force_enable_rptr_writeback ? "YES": "NO");
    LogCommandProcessorBreadcrumb("VdEnableRingBufferRPtrWriteBack",
                                  graphics_system);
  }
  if (graphics_system) {
    graphics_system->NotifyVdEnableRingBufferRPtrWriteBackCall(pc, thread_id);
  }
  if (!graphics_system) {
    XELOGE("DEBUG: VdEnableRingBufferRPtrWriteBack: graphics_system is NULL");
    if (!cvars::video_force_enable_rptr_writeback) {
      return;
    }
  }
  if (ptr.value() == 0 || block_size_log2.value() == 0) {
    if (cvars::video_force_enable_rptr_writeback) {
      uint32_t block_log2 = 6;
      uint32_t size_bytes = uint32_t(1) << block_log2;
      uint32_t forced_ptr = kernel_state()->memory()->SystemHeapAlloc(
          size_bytes, 4096, kSystemHeapPhysical);
      if (forced_ptr && graphics_system) {
        XELOGW(
            "DEBUG: Forced VdEnableRingBufferRPtrWriteBack: ptr=0x{:08X} "
            "block_size_log2={}",
            forced_ptr, block_log2);
        graphics_system->EnableReadPointerWriteBack(forced_ptr, block_log2);
      } else {
        XELOGE("DEBUG: Forced VdEnableRingBufferRPtrWriteBack: allocation failed");
      }
    } else {
      XELOGE(
          "DEBUG: VdEnableRingBufferRPtrWriteBack: invalid args ptr=0x{:08X} "
          "block_size_log2={}",
          ptr.value(), block_size_log2.value());
      return;
    }
  } else if (graphics_system) {
    graphics_system->EnableReadPointerWriteBack(ptr, block_size_log2);
  }
  if (::cvars::trace_gpu_bootstrap && graphics_system &&
      graphics_system->command_processor()) {
    auto dbg = graphics_system->command_processor()->GetRingBufferDebugState();
    XELOGI(
        "DEBUG: Vd RPtr writeback applied: cp.rptr_wb=0x{:08X} cp.rptr_freq=0x{:08X}",
        dbg.read_ptr_writeback_ptr, dbg.read_ptr_update_freq);
  }
  XELOGI("DEBUG: VdEnableRingBufferRPtrWriteBack completed");
}
DECLARE_XBOXKRNL_EXPORT1(VdEnableRingBufferRPtrWriteBack, kVideo, kImplemented);

DEFINE_bool(video_force_system_command_buffer, false,
            "Debug: Override default VdGetSystemCommandBuffer allocation size.",
            "Video");
DEFINE_uint32(video_force_system_command_buffer_size, 0x2000,
              "Debug: Size in bytes for forced system command buffer.",
              "Video");

void VdGetSystemCommandBuffer_entry(lpunknown_t p0_ptr, lpunknown_t p1_ptr) {
  uint64_t call_count = g_vd_get_sys_cb_calls.fetch_add(1) + 1;
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x01BD, pc,
                              thread_id);  // VdGetSystemCommandBuffer
  if (::cvars::trace_gpu_bootstrap) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x{:03X} "
        "name=VdGetSystemCommandBuffer args[r3..r4]=0x{:08X},0x{:08X}",
        ResolveCallerModuleName(pc), pc, thread_id,
        static_cast<unsigned int>(ordinals::VdGetSystemCommandBuffer), p0_ptr.guest_address(),
        p1_ptr.guest_address());
  }
  XELOGW(
      "RING BUFFER: VdGetSystemCommandBuffer call count={} pc={:08X} "
      "thread_id={} out_addr_ptr={:08X} out_size_ptr={:08X}",
      call_count, pc, thread_id, p0_ptr.guest_address(), p1_ptr.guest_address());
  auto* thread = XThread::GetCurrentThread();
  XELOGI("DEBUG: BREADCRUMB: VdGetSystemCommandBuffer called (count={})",
         call_count);
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (graphics_system) {
    graphics_system->NotifyVdGetSystemCommandBufferCall(pc, thread_id);
    if (::cvars::video_log_vd_breadcrumbs) {
      XELOGI(
          "DEBUG: BREADCRUMB: VdGetSystemCommandBuffer ENTRY pc={:08X} "
          "thread_id={} p0_ptr=0x{:08X} p1_ptr=0x{:08X} force_sys_buf={} "
          "size=0x{:X}",
          pc, thread_id, p0_ptr.guest_address(), p1_ptr.guest_address(),
          cvars::video_force_system_command_buffer ? "YES": "NO",
          cvars::video_force_system_command_buffer_size);
      LogCommandProcessorBreadcrumb("VdGetSystemCommandBuffer",
                                    graphics_system);
    }
  }
  if (::cvars::video_log_vd_system_cmd_buffer &&
      (call_count <= 5 || (call_count % 120) == 0)) {
    XELOGI("DEBUG: VdGetSystemCommandBuffer called (count={})", call_count);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("p0_ptr=0x{:08X} p1_ptr=0x{:08X}", p0_ptr.guest_address(),
           p1_ptr.guest_address());
  }
  // Return a real persistent system command buffer by default, as many titles
  // rely on it for guest-driven present flow before VdSwap.
  uint32_t requested_size = cvars::video_force_system_command_buffer
                                ? cvars::video_force_system_command_buffer_size
                                : 0x2000;
  if (requested_size < 0x100) {
    requested_size = 0x100;
  }
  uint32_t sys_cb_ptr = 0;
  uint32_t sys_cb_size = 0;

  if (graphics_system) {
    graphics_system->EnsureSystemCommandBufferReady(requested_size,
                                                    "VdGetSystemCommandBuffer");
    sys_cb_ptr = graphics_system->GetSystemCommandBuffer();
    sys_cb_size = graphics_system->GetSystemCommandBufferSize();
  }

  if (sys_cb_ptr == 0 || sys_cb_size < requested_size) {
    uint32_t new_ptr = kernel_state()->memory()->SystemHeapAlloc(
        requested_size, 4096, kSystemHeapPhysical);
    if (!new_ptr) {
      XELOGE(
          "VdGetSystemCommandBuffer: failed to allocate system command buffer "
          "(size=0x{:X})",
          requested_size);
    } else {
      sys_cb_ptr = new_ptr;
      sys_cb_size = requested_size;
      if (graphics_system) {
        graphics_system->SetSystemCommandBuffer(
            sys_cb_ptr, sys_cb_size, "VdGetSystemCommandBuffer");
      }
      if (cvars::video_force_system_command_buffer) {
        XELOGW("DEBUG: Forced VdGetSystemCommandBuffer: addr={:08X} size=0x{:X}",
               sys_cb_ptr, sys_cb_size);
      } else {
        XELOGI(
            "DEBUG: VdGetSystemCommandBuffer: allocated persistent buffer "
            "addr={:08X} size=0x{:X}",
            sys_cb_ptr, sys_cb_size);
      }
    }
  }

  p0_ptr.Zero(0x94);
  if (sys_cb_ptr != 0 && sys_cb_size != 0) {
    xe::store_and_swap<uint32_t>(p0_ptr, sys_cb_ptr);
    xe::store_and_swap<uint32_t>(p1_ptr, sys_cb_size);
  } else {
    // Keep a deterministic fallback if allocation fails.
    xe::store_and_swap<uint32_t>(p0_ptr, 0xBEEF0000);
    xe::store_and_swap<uint32_t>(p1_ptr, 0xBEEF0001);
  }
  if (::cvars::trace_gpu_bootstrap) {
    uint32_t out_addr = xe::load_and_swap<uint32_t>(p0_ptr);
    uint32_t out_size = xe::load_and_swap<uint32_t>(p1_ptr);
    void* out_host = out_addr ? kernel_memory()->TranslatePhysical(out_addr)
                              : nullptr;
    XELOGI(
        "DEBUG: VdGetSystemCommandBuffer return: out_addr={:08X} out_size=0x{:X} "
        "host={} mapped={}",
        out_addr, out_size, out_host, out_host ? "YES": "NO");
  }

  // Additional fallback check
  auto graphics_system_fallback = kernel_state()->emulator()->graphics_system();
  if (graphics_system_fallback &&
      graphics_system_fallback->GetSystemCommandBuffer() == 0) {
    if (::cvars::video_allow_bootstrap_fallback_hacks) {
      XELOGW("DEBUG: System command buffer may not be set, attempting fallback");
      graphics_system_fallback->AttemptFallbackInitialization();
    } else if (::cvars::trace_gpu_bootstrap) {
      XELOGW(
          "DEBUG: Bootstrap trace: system command buffer remains unset after "
          "VdGetSystemCommandBuffer (fallback hacks disabled)");
    }
  }
}
DECLARE_XBOXKRNL_EXPORT1(VdGetSystemCommandBuffer, kVideo, kStub);

void VdSetSystemCommandBufferGpuIdentifierAddress_entry(lpunknown_t unk) {
  // r3 = 0x2B10(d3d?) + 8
  uint64_t call_count = g_vd_set_sys_cb_calls.fetch_add(1) + 1;
  auto* thread = XThread::GetCurrentThread();
  XELOGI("DEBUG: VdSetSystemCommandBufferGpuIdentifierAddress called (count={})",
         call_count);
  XELOGI("caller thread handle={:08X} id={} main={}",
         thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
         thread && thread->main_thread() ? "YES": "NO");
  XELOGI("unk=0x{:08X}", unk.guest_address());

  if (unk) {
    xe::store_and_swap<uint32_t>(unk, 0x12345678);
    XELOGI("Wrote dummy GPU identifier 0x12345678 to guest address");
  }
}
DECLARE_XBOXKRNL_EXPORT1(VdSetSystemCommandBufferGpuIdentifierAddress, kVideo,
                         kStub);

// VdVerifyMEInitCommand
// r3
// r4 = 19
// no op?

dword_result_t VdInitializeScalerCommandBuffer_entry(
    dword_t scaler_source_xy,      // ((uint16_t)y << 16) | (uint16_t)x
    dword_t scaler_source_wh,      // ((uint16_t)h << 16) | (uint16_t)w
    dword_t scaled_output_xy,      // ((uint16_t)y << 16) | (uint16_t)x
    dword_t scaled_output_wh,      // ((uint16_t)h << 16) | (uint16_t)w
    dword_t front_buffer_wh,       // ((uint16_t)h << 16) | (uint16_t)w
    dword_t vertical_filter_type,  // 7?
    pointer_t<X_D3DFILTER_PARAMETERS> vertical_filter_params,    //
    dword_t horizontal_filter_type,                              // 7?
    pointer_t<X_D3DFILTER_PARAMETERS> horizontal_filter_params,  //
    lpvoid_t unk9,                                               //
    lpvoid_t dest_ptr,  // Points to the first 80000000h where the memcpy
                        // sources from.
    dword_t dest_count  // Count in words.
) {
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  if (call_id <= 5 || (call_id % 120) == 0) {
    auto* thread = XThread::GetCurrentThread();
    XELOGI("DEBUG: VdInitializeScalerCommandBuffer called (count={})", call_id);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("DEBUG: BREADCRUMB: VdInitializeScalerCommandBuffer called");
    XELOGI("scaler_source_xy=0x{:08X} scaler_source_wh=0x{:08X}",
           scaler_source_xy.value(), scaler_source_wh.value());
    XELOGI("scaled_output_xy=0x{:08X} scaled_output_wh=0x{:08X}",
           scaled_output_xy.value(), scaled_output_wh.value());
    XELOGI("front_buffer_wh=0x{:08X} dest_ptr=0x{:08X} dest_count={}",
           front_buffer_wh.value(), dest_ptr.guest_address(),
           dest_count.value());
  }
  // We could fake the commands here, but I'm not sure the game checks for
  // anything but success (non-zero ret).
  // For now, we just fill it with NOPs.
  auto dest = dest_ptr.as_array<uint32_t>();
  for (size_t i = 0; i < dest_count; ++i) {
    dest[i] = 0x80000000;
  }

  uint32_t fb_x = (scaled_output_wh >> 16) & 0xFFFF;
  uint32_t fb_y = scaled_output_wh & 0xFFFF;
  auto aspect = CalculateScaledAspectRatio(fb_x, fb_y);

  auto graphics_system = kernel_state()->emulator()->graphics_system();
  graphics_system->SetScaledAspectRatio(aspect.first, aspect.second);

  return (uint32_t)dest_count;
}
DECLARE_XBOXKRNL_EXPORT2(VdInitializeScalerCommandBuffer, kVideo, kImplemented,
                         kSketchy);

struct BufferScaling {
  xe::be<uint16_t> fb_width;
  xe::be<uint16_t> fb_height;
  xe::be<uint16_t> bb_width;
  xe::be<uint16_t> bb_height;
};
void AppendParam(StringBuffer* string_buffer, pointer_t<BufferScaling> param) {
  string_buffer->AppendFormat(
      "{:08X}(scale {}x{} -> {}x{}))", param.guest_address(),
      uint16_t(param->bb_width), uint16_t(param->bb_height),
      uint16_t(param->fb_width), uint16_t(param->fb_height));
}

dword_result_t VdCallGraphicsNotificationRoutines_entry(
    unknown_t unk0, pointer_t<BufferScaling> args_ptr) {
  assert_true(unk0 == 1);

  // TODO(benvanik): what does this mean, I forget:
  // callbacks get 0, r3, r4
  return 0;
}
DECLARE_XBOXKRNL_EXPORT2(VdCallGraphicsNotificationRoutines, kVideo,
                         kImplemented, kSketchy);

dword_result_t VdIsHSIOTrainingSucceeded_entry() {
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x01C6, pc,
                              thread_id);  // VdIsHSIOTrainingSucceeded
  if (::cvars::trace_gpu_bootstrap ||
      ::cvars::video_log_vd_edram_training) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x1C6 name=VdIsHSIOTrainingSucceeded",
        ResolveCallerModuleName(pc), pc, thread_id);
  }
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (graphics_system) {
    graphics_system->NotifyVdIsHSIOTrainingSucceededCall(pc, thread_id);
    if (::cvars::video_log_vd_breadcrumbs) {
      LogCommandProcessorBreadcrumb("VdIsHSIOTrainingSucceeded",
                                    graphics_system);
    }
  }
  XELOGI("DEBUG: BREADCRUMB: VdIsHSIOTrainingSucceeded called");
  return 1;
}
DECLARE_XBOXKRNL_EXPORT1(VdIsHSIOTrainingSucceeded, kVideo, kStub);

dword_result_t VdPersistDisplay_entry(unknown_t unk0, lpdword_t unk1_ptr) {
  // unk1_ptr needs to be populated with a pointer passed to
  // MmFreePhysicalMemory(1, *unk1_ptr).
  if (unk1_ptr) {
    auto heap = kernel_memory()->LookupHeapByType(true, 16 * 1024);
    uint32_t unk1_value;
    heap->Alloc(64, 32, kMemoryAllocationReserve | kMemoryAllocationCommit,
                kMemoryProtectNoAccess, false, &unk1_value);
    *unk1_ptr = unk1_value;
  }

  return 1;
}
DECLARE_XBOXKRNL_EXPORT2(VdPersistDisplay, kVideo, kImplemented, kSketchy);

dword_result_t VdInitializeEDRAM_entry(unknown_t unk0, unknown_t unk1,
                                       unknown_t unk2, unknown_t unk3,
                                       unknown_t unk4, unknown_t unk5) {
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x0268, pc, thread_id);  // VdInitializeEDRAM
  if (::cvars::trace_gpu_bootstrap ||
      ::cvars::video_log_vd_edram_training) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x268 name=VdInitializeEDRAM "
        "args[r3..r8]=0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X}",
        ResolveCallerModuleName(pc), pc, thread_id, unk0.value(), unk1.value(),
        unk2.value(), unk3.value(), unk4.value(), unk5.value());
  }
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (graphics_system) {
    graphics_system->NotifyVdInitializeEDRAMCall(pc, thread_id);
    if (::cvars::video_log_vd_breadcrumbs) {
      XELOGI("DEBUG: BREADCRUMB: VdInitializeEDRAM called (count={})", call_id);
      LogCommandProcessorBreadcrumb("VdInitializeEDRAM", graphics_system);
    }
  }
  int32_t ret = ::cvars::video_vd_edram_stub_return;
  if (::cvars::video_log_vd_edram_training &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    XELOGI("DEBUG: VdInitializeEDRAM returning {}", ret);
  }
  return static_cast<uint32_t>(ret);
}
DECLARE_XBOXKRNL_EXPORT1(VdInitializeEDRAM, kVideo, kStub);

dword_result_t VdRetrainEDRAMWorker_entry(unknown_t unk0, unknown_t unk1,
                                          unknown_t unk2, unknown_t unk3,
                                          unknown_t unk4, unknown_t unk5) {
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x026A, pc, thread_id);  // VdRetrainEDRAMWorker
  if (::cvars::trace_gpu_bootstrap ||
      ::cvars::video_log_vd_edram_training) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x26A name=VdRetrainEDRAMWorker "
        "args[r3..r8]=0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X}",
        ResolveCallerModuleName(pc), pc, thread_id, unk0.value(), unk1.value(),
        unk2.value(), unk3.value(), unk4.value(), unk5.value());
  }
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (graphics_system) {
    graphics_system->NotifyVdRetrainEDRAMWorkerCall(pc, thread_id);
    if (::cvars::video_log_vd_breadcrumbs) {
      XELOGI("DEBUG: BREADCRUMB: VdRetrainEDRAMWorker called (count={})", call_id);
      LogCommandProcessorBreadcrumb("VdRetrainEDRAMWorker", graphics_system);
    }
  }
  int32_t ret = ::cvars::video_vd_edram_stub_return;
  if (::cvars::video_log_vd_edram_training &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    XELOGI("DEBUG: VdRetrainEDRAMWorker returning {}", ret);
  }
  return static_cast<uint32_t>(ret);
}
DECLARE_XBOXKRNL_EXPORT1(VdRetrainEDRAMWorker, kVideo, kStub);

dword_result_t VdRetrainEDRAM_entry(unknown_t unk0, unknown_t unk1,
                                    unknown_t unk2, unknown_t unk3,
                                    unknown_t unk4, unknown_t unk5) {
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x0269, pc, thread_id);  // VdRetrainEDRAM
  if (::cvars::trace_gpu_bootstrap ||
      ::cvars::video_log_vd_edram_training) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x269 name=VdRetrainEDRAM "
        "args[r3..r8]=0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X}",
        ResolveCallerModuleName(pc), pc, thread_id, unk0.value(), unk1.value(),
        unk2.value(), unk3.value(), unk4.value(), unk5.value());
  }
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (graphics_system) {
    graphics_system->NotifyVdRetrainEDRAMCall(pc, thread_id);
    if (::cvars::video_log_vd_breadcrumbs) {
      XELOGI("DEBUG: BREADCRUMB: VdRetrainEDRAM called (count={})", call_id);
      LogCommandProcessorBreadcrumb("VdRetrainEDRAM", graphics_system);
    }
  }
  int32_t ret = ::cvars::video_vd_edram_stub_return;
  if (::cvars::video_log_vd_edram_training &&
      (call_id <= 5 || (call_id % 120) == 0)) {
    XELOGI("DEBUG: VdRetrainEDRAM returning {}", ret);
  }
  return static_cast<uint32_t>(ret);
}
DECLARE_XBOXKRNL_EXPORT1(VdRetrainEDRAM, kVideo, kStub);

void VdSwap_entry(
    lpvoid_t buffer_ptr,        // ptr into primary ringbuffer
    lpvoid_t fetch_ptr,         // frontbuffer Direct3D 9 texture header fetch
    lpunknown_t unk2,           // system writeback ptr
    lpunknown_t unk3,           // buffer from VdGetSystemCommandBuffer
    lpunknown_t unk4,           // from VdGetSystemCommandBuffer (0xBEEF0001)
    lpdword_t frontbuffer_ptr,  // ptr to frontbuffer address
    lpdword_t texture_format_ptr, lpdword_t color_space_ptr, lpdword_t width,
    lpdword_t height) {
  // All of these parameters are REQUIRED.
  assert(buffer_ptr);
  assert(fetch_ptr);
  assert(frontbuffer_ptr);
  assert(texture_format_ptr);
  assert(width);
  assert(height);

  uint32_t pc = 0;
  uint32_t thread_id = 0;
  GetCurrentThreadPcAndId(&pc, &thread_id);
  RecordTrackedGpuExportEntry(0x025B, pc, thread_id);  // VdSwap
  if (::cvars::trace_gpu_bootstrap) {
    XELOGI(
        "DEBUG: Vd export call: caller_module={} pc={:08X} thread_id={} "
        "export_module=xboxkrnl ordinal=0x{:03X} name=VdSwap "
        "args[r3..r10]=0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X},"
        "0x{:08X},0x{:08X},0x{:08X}",
        ResolveCallerModuleName(pc), pc, thread_id, static_cast<unsigned int>(ordinals::VdSwap),
        buffer_ptr.guest_address(), fetch_ptr.guest_address(),
        unk2.guest_address(), unk3.guest_address(), unk4.guest_address(),
        frontbuffer_ptr.guest_address(), texture_format_ptr.guest_address(),
        color_space_ptr.guest_address());
  }

  auto graphics_system = kernel_state()->emulator()->graphics_system();
  if (graphics_system) {
    graphics_system->NotifyVdSwapCall(pc, thread_id);
  }

  uint64_t call_count = xboxkrnl::g_vd_swap_calls.fetch_add(1) + 1;
  if (call_count <= 5 || (call_count % 120) == 0) {
    XELOGW(
        "RING BUFFER: VdSwap call count={} pc={:08X} thread_id={} "
        "buffer_ptr={:08X} fetch_ptr={:08X} frontbuffer_ptr={:08X}",
        call_count, pc, thread_id, buffer_ptr.guest_address(),
        fetch_ptr.guest_address(), frontbuffer_ptr.guest_address());
  }
  bool log_verbose = ::cvars::video_log_vdswap_verbose &&
                     (call_count <= 5 ||
                      (::cvars::video_log_vdswap_every_n > 0 &&
                       (call_count % ::cvars::video_log_vdswap_every_n) == 0));
  auto* thread = XThread::GetCurrentThread();
  if (log_verbose) {
    XELOGI("DEBUG: VdSwap called (count={})", call_count);
    XELOGI("caller thread handle={:08X} id={} main={}",
           thread ? thread->handle() : 0, thread ? thread->thread_id() : 0,
           thread && thread->main_thread() ? "YES": "NO");
    XELOGI("DEBUG: BREADCRUMB: VdSwap called (count={})", call_count);
    XELOGI("buffer_ptr guest={:08X} host={}", buffer_ptr.guest_address(),
           reinterpret_cast<void*>(buffer_ptr.host_address()));
    XELOGI("fetch_ptr guest={:08X} host={}", fetch_ptr.guest_address(),
           reinterpret_cast<void*>(fetch_ptr.host_address()));
    XELOGI("sys_wb_ptr={} sys_cmd_buf_ptr={} sys_cmd_buf_tag={}",
           reinterpret_cast<void*>(unk2.host_address()),
           reinterpret_cast<void*>(unk3.host_address()),
           reinterpret_cast<void*>(unk4.host_address()));
    XELOGI("frontbuffer_ptr guest={:08X} value={:08X}",
           frontbuffer_ptr.guest_address(), frontbuffer_ptr.value());
    XELOGI("texture_format_ptr guest={:08X} value={:08X}",
           texture_format_ptr.guest_address(), texture_format_ptr.value());
    XELOGI("color_space_ptr guest={:08X} value={:08X}",
           color_space_ptr.guest_address(), color_space_ptr.value());
    XELOGI(
        "width_ptr guest={:08X} value={} height_ptr guest={:08X} value={}",
        width.guest_address(), width.value(), height.guest_address(),
        height.value());
    if (::cvars::video_log_vd_breadcrumbs) {
      LogCommandProcessorBreadcrumb("VdSwap", graphics_system);
    }
  }

  namespace xenos = xe::gpu::xenos;

  xenos::xe_gpu_texture_fetch_t gpu_fetch;
  xe::copy_and_swap_32_unaligned(
      &gpu_fetch, reinterpret_cast<uint32_t*>(fetch_ptr.host_address()), 6);
  xenos::xe_gpu_texture_fetch_t gpu_fetch_original = gpu_fetch;
  if (log_verbose) {
    DumpSwapFetch(gpu_fetch_original, "VdSwap fetch (raw)");
  }

  // The fetch constant passed is not a true GPU fetch constant, but rather, the
  // fetch constant stored in the Direct3D 9 texture header, which contains the
  // address in one of the virtual mappings of the physical memory rather than
  // the physical address itself. We're emulating swapping in the GPU subsystem,
  // which works with GPU memory addresses (physical addresses directly) from
  // proper fetch constants like ones used to bind textures to shaders, not CPU
  // MMU addresses, so translation from virtual to physical is needed.
  uint32_t frontbuffer_virtual_address = gpu_fetch.base_address << 12;
  assert_true(*frontbuffer_ptr == frontbuffer_virtual_address);
  uint32_t frontbuffer_physical_address =
      kernel_memory()->GetPhysicalAddress(frontbuffer_virtual_address);
  assert_true(frontbuffer_physical_address != UINT32_MAX);
  if (frontbuffer_physical_address == UINT32_MAX) {
    // Xenia-specific safety check.
    XELOGE("VdSwap: Invalid front buffer virtual address 0x{:08X}",
           frontbuffer_virtual_address);
    return;
  }
  if (log_verbose) {
    XELOGI("frontbuffer_virtual=0x{:08X} physical=0x{:08X}",
           frontbuffer_virtual_address, frontbuffer_physical_address);
  }
  gpu_fetch.base_address = frontbuffer_physical_address >> 12;
  if (log_verbose) {
    DumpSwapFetch(gpu_fetch, "VdSwap fetch (physical)");
  }

  // Additional fallback check
  auto graphics_system_fallback =
      kernel_state()->emulator()->graphics_system();
  if (graphics_system_fallback && !graphics_system_fallback->IsReadyForSwap()) {
    if (::cvars::video_allow_bootstrap_fallback_hacks) {
      XELOGW("DEBUG: GPU not ready for swap, attempting fallback initialization");
      graphics_system_fallback->AttemptFallbackInitialization();
    } else if (::cvars::trace_gpu_bootstrap) {
      XELOGW(
          "DEBUG: Bootstrap trace: swap attempted while GPU not ready "
          "(fallback hacks disabled)");
    }
  }

XE_MAYBE_UNUSED
auto texture_format = gpu::xenos::TextureFormat(texture_format_ptr.value());
auto color_space = *color_space_ptr;
assert_true(texture_format == gpu::xenos::TextureFormat::k_8_8_8_8 ||
            texture_format ==
                gpu::xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16);
assert_true(color_space == 0);  // RGB(0)
assert_true(*width == 1 + gpu_fetch.size_2d.width);
assert_true(*height == 1 + gpu_fetch.size_2d.height);

if (log_verbose) {
  XELOGI("texture_format={} color_space={} size={}x{}",
         static_cast<uint32_t>(texture_format),
         static_cast<uint32_t>(color_space), static_cast<uint32_t>(*width),
         static_cast<uint32_t>(*height));
}

// The caller seems to reserve 64 words (256b) in the primary ringbuffer
// for this method to do what it needs. We just zero them out and send a
// token value. It'd be nice to figure out what this is really doing so
// that we could simulate it, though due to TCR I bet all games need to
// use this method.
buffer_ptr.Zero(64 * 4);

uint32_t offset = 0;
auto dwords = buffer_ptr.as_array<uint32_t>();

// Write in the GPU texture fetch.
dwords[offset++] =
    xenos::MakePacketType0(gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
dwords[offset++] = gpu_fetch.dword_0;
dwords[offset++] = gpu_fetch.dword_1;
dwords[offset++] = gpu_fetch.dword_2;
dwords[offset++] = gpu_fetch.dword_3;
dwords[offset++] = gpu_fetch.dword_4;
dwords[offset++] = gpu_fetch.dword_5;

dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
dwords[offset++] = xe::gpu::xenos::kSwapSignature;
dwords[offset++] = frontbuffer_physical_address;

dwords[offset++] = *width;
dwords[offset++] = *height;

// Fill the rest of the buffer with NOP packets.
for (uint32_t i = offset; i < 64; i++) {
  dwords[i] = xenos::MakePacketType2();
}

if (log_verbose && ::cvars::video_log_vdswap_dump_ring) {
  uint32_t dump_limit =
      static_cast<uint32_t>(std::max(0, ::cvars::video_log_vdswap_dump_dwords));
  uint32_t dump_dwords = std::min<uint32_t>(dump_limit, 64);
  DumpDwords("VdSwap packet dwords",
             reinterpret_cast<void*>(buffer_ptr.host_address()), dump_dwords);
}

// Debug: log ring buffer positioning and optionally force a write pointer
// kick.
if (graphics_system) {
  auto* command_processor = graphics_system->command_processor();
  uint32_t rb_base =
      command_processor ? command_processor->primary_buffer_ptr() : 0;
  uint32_t rb_size =
      command_processor ? command_processor->primary_buffer_size() : 0;
  uint32_t buffer_guest = buffer_ptr.guest_address();
  uint32_t buffer_phys = kernel_memory()->GetPhysicalAddress(buffer_guest);
  if (log_verbose) {
    XELOGI("DEBUG: VdSwap ring buffer write:");
    XELOGI("buffer_guest={:08X} buffer_phys={:08X}", buffer_guest,
           buffer_phys);
    XELOGI("ring_base={:08X} ring_size_bytes={:08X}", rb_base, rb_size);
  }
  if (rb_base && rb_size && buffer_phys != UINT32_MAX) {
    if (buffer_phys >= rb_base && buffer_phys < rb_base + rb_size) {
      uint32_t offset_bytes = buffer_phys - rb_base;
      uint32_t offset_dwords = offset_bytes / 4;
      uint32_t ring_dwords = rb_size / 4;
      uint32_t new_wptr = (offset_dwords + 64) % ring_dwords;
      if (log_verbose) {
        XELOGI(
            "offset_bytes=0x{:X} offset_dwords={} ring_dwords={} "
            "new_wptr={}",
            offset_bytes, offset_dwords, ring_dwords, new_wptr);
      }
      bool force_kick = ::cvars::video_force_ringbuffer_kick_on_vdswap;
      bool stale_kick = false;
      uint32_t stale_ms =
          ::cvars::video_force_ringbuffer_kick_on_vdswap_if_stale_ms;
      uint64_t last_wptr_ms = graphics_system->last_write_pointer_update_ms();
      uint32_t last_wptr_val = graphics_system->last_write_pointer_value();
      uint64_t now_ms = Clock::QueryHostUptimeMillis();
      uint64_t age_ms = (last_wptr_ms != 0 && now_ms >= last_wptr_ms)
                            ? (now_ms - last_wptr_ms)
                            : 0;
      if (!force_kick && stale_ms > 0) {
        if (last_wptr_ms == 0 || age_ms >= stale_ms) {
          stale_kick = true;
        }
      }
      if (command_processor) {
        uint32_t current_wptr = command_processor->write_ptr_index();
        bool wptr_changed = current_wptr != new_wptr;
        bool should_submit = wptr_changed || force_kick || stale_kick;
        if (should_submit) {
          if (force_kick) {
            XELOGW(
                "RING BUFFER: VdSwap submit via CP_RB_WPTR (debug force "
                "enabled, {:08X} -> {:08X})",
                current_wptr, new_wptr);
          } else if (stale_kick) {
            XELOGW(
                "RING BUFFER: VdSwap submit via CP_RB_WPTR (stale {} ms, "
                "last={:08X}, {:08X} -> {:08X})",
                age_ms, last_wptr_val, current_wptr, new_wptr);
          } else if (log_verbose) {
            XELOGI(
                "RING BUFFER: VdSwap submit via CP_RB_WPTR ({:08X} -> {:08X})",
                current_wptr, new_wptr);
          }
          command_processor->UpdateWritePointer(new_wptr,
                                                "VDSWAP_WPTR_KICK");
          graphics_system->RecordWritePointerUpdate(
              new_wptr,
              stale_kick ? "VdSwap-stale"
                         : (force_kick ? "VdSwap-force" : "VdSwap"));
        } else if (log_verbose) {
          XELOGI(
              "RING BUFFER: VdSwap submit skipped (WPTR unchanged at {:08X})",
              current_wptr);
        }
      } else {
        XELOGW("RING BUFFER: VdSwap submit skipped (command_processor=null)");
      }
      if (log_verbose && ::cvars::video_log_vdswap_dump_ring) {
        void* ring_host = kernel_memory()->TranslatePhysical(rb_base);
        if (ring_host) {
          uint32_t dump_limit = static_cast<uint32_t>(
              std::max(0, ::cvars::video_log_vdswap_dump_dwords));
          uint32_t window = std::min<uint32_t>(dump_limit, ring_dwords);
          uint32_t start =
              offset_dwords > (window / 2) ? (offset_dwords - window / 2) : 0;
          DumpRingBufferWindow("VdSwap ring buffer window", ring_host,
                               ring_dwords, start, window);
        } else if (log_verbose) {
          XELOGW("DEBUG: VdSwap ring buffer host mapping unavailable");
        }
      }
    } else {
      XELOGW(
          "DEBUG: VdSwap buffer is outside ring buffer range; cannot compute "
          "WPTR");
    }
  } else {
    XELOGW("DEBUG: VdSwap ring buffer info unavailable (base/size/phys)");
  }
} else {
  XELOGW("DEBUG: VdSwap: graphics_system is null");
}
}
DECLARE_XBOXKRNL_EXPORT3(VdSwap, kVideo, kImplemented, kHighFrequency,
                         kImportant);

void RegisterVideoExports(xe::cpu::ExportResolver* export_resolver,
                          KernelState* kernel_state) {
  auto memory = kernel_state->memory();
  const bool allow_fallback_hacks = ::cvars::video_allow_bootstrap_fallback_hacks;

  // VdGlobalDevice (4b)
  // Global D3D device pointer. Keep this zero by default so titles run their
  // own initialization path (VdGetSystemCommandBuffer /
  // VdInitializeRingBuffer).
  uint32_t pVdGlobalDevice =
      memory->SystemHeapAlloc(4, 32, kSystemHeapPhysical);
  export_resolver->SetVariableMapping("xboxkrnl.exe", ordinals::VdGlobalDevice,
                                      pVdGlobalDevice);
  uint32_t vd_global_device_value = 0;
  if (::cvars::video_force_vd_global_device && !allow_fallback_hacks) {
    XELOGW(
        "DEBUG: Ignoring video_force_vd_global_device because "
        "video_allow_bootstrap_fallback_hacks=false");
  } else if (::cvars::video_force_vd_global_device) {
    uint32_t vd_global_device_size = std::max<uint32_t>(
        4, static_cast<uint32_t>(::cvars::video_force_vd_global_device_size));
    uint32_t dummy_device =
        memory->SystemHeapAlloc(vd_global_device_size, 32, kSystemHeapPhysical);
    if (dummy_device != 0) {
      vd_global_device_value = dummy_device;
      std::memset(memory->TranslateVirtual(dummy_device), 0,
                  vd_global_device_size);
      XELOGW("DEBUG: Forcing VdGlobalDevice to dummy allocation {:08X} ({} bytes)",
             dummy_device, vd_global_device_size);
    } else {
      XELOGE("VdGlobalDevice dummy allocation failed");
    }
  }
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGlobalDevice),
                               vd_global_device_value);

  // VdGlobalXamDevice (4b)
  // Pointer to the XAM D3D device, which we don't have.
  uint32_t pVdGlobalXamDevice =
      memory->SystemHeapAlloc(4, 32, kSystemHeapPhysical);
  export_resolver->SetVariableMapping(
      "xboxkrnl.exe", ordinals::VdGlobalXamDevice, pVdGlobalXamDevice);
  uint32_t vd_global_xam_device_value = 0;
  if (::cvars::video_force_vd_global_xam_device && !allow_fallback_hacks) {
    XELOGW(
        "DEBUG: Ignoring video_force_vd_global_xam_device because "
        "video_allow_bootstrap_fallback_hacks=false");
  } else if (::cvars::video_force_vd_global_xam_device) {
    uint32_t dummy_size = std::max<uint32_t>(
        4,
        static_cast<uint32_t>(::cvars::video_force_vd_global_xam_device_size));
    uint32_t dummy_device =
        memory->SystemHeapAlloc(dummy_size, 32, kSystemHeapPhysical);
    vd_global_xam_device_value = dummy_device;
    XELOGW("DEBUG: Forcing VdGlobalXamDevice to dummy allocation {:08X} ({} bytes)",
           dummy_device, dummy_size);
  }
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGlobalXamDevice),
                               vd_global_xam_device_value);

  // VdGpuClockInMHz (4b)
  // GPU clock. Xenos is 500MHz. Hope nothing is relying on this timing...
  uint32_t pVdGpuClockInMHz =
      memory->SystemHeapAlloc(4, 32, kSystemHeapPhysical);
  export_resolver->SetVariableMapping("xboxkrnl.exe", ordinals::VdGpuClockInMHz,
                                      pVdGpuClockInMHz);
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGpuClockInMHz), 500);

  // VdHSIOCalibrationLock (28b)
  // CriticalSection.
  uint32_t pVdHSIOCalibrationLock =
      memory->SystemHeapAlloc(28, 32, kSystemHeapPhysical);
  export_resolver->SetVariableMapping(
      "xboxkrnl.exe", ordinals::VdHSIOCalibrationLock, pVdHSIOCalibrationLock);
  auto hsio_lock = memory->TranslateVirtual<xboxkrnl::X_RTL_CRITICAL_SECTION*>(
      pVdHSIOCalibrationLock);
  xeRtlInitializeCriticalSectionAndSpinCount(hsio_lock, pVdHSIOCalibrationLock,
                                             10000);
  if (::cvars::trace_gpu_bootstrap) {
    XELOGI(
        "DEBUG: Vd globals initialized: "
        "VdGlobalDevice(slot={:08X},value={:08X}) "
        "VdGlobalXamDevice(slot={:08X},value={:08X}) "
        "VdGpuClockInMHz(slot={:08X},value=500)",
        pVdGlobalDevice, vd_global_device_value, pVdGlobalXamDevice,
        vd_global_xam_device_value, pVdGpuClockInMHz);
  }
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
