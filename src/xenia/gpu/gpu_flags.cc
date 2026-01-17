/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/gpu_flags.h"

DEFINE_path(trace_gpu_prefix, "scratch/gpu/",
            "Prefix path for GPU trace files.", "GPU");
DEFINE_bool(trace_gpu_stream, false, "Trace all GPU packets.", "GPU");

DEFINE_path(
    dump_shaders, "",
    "For shader debugging, path to dump GPU shaders to as they are compiled.",
    "GPU");

DEFINE_bool(vsync, true, "Enable VSYNC.", "GPU");

DEFINE_uint64(framerate_limit, 0,
              "Maximum frames per second. 0 = Unlimited frames.\n"
              "Defaults to 60, when set to 0, and VSYNC is enabled.",
              "GPU");
UPDATE_from_uint64(framerate_limit, 2024, 8, 31, 20, 60);

DEFINE_bool(
    gpu_allow_invalid_fetch_constants, true,
    "Allow texture and vertex fetch constants with invalid type - generally "
    "unsafe because the constant may contain completely invalid values, but "
    "may be used to bypass fetch constant type errors in certain games until "
    "the real reason why they're invalid is found.",
    "GPU");
DEFINE_bool(
    gpu_allow_invalid_upload_range, false,
    "Allows games to read data from pages that are marked as no access.",
    "GPU");

DEFINE_bool(
    non_seamless_cube_map, true,
    "Disable filtering between cube map faces near edges where possible "
    "(Vulkan with VK_EXT_non_seamless_cube_map) to reproduce the Direct3D 9 "
    "behavior.",
    "GPU");

// Extremely bright screen borders in 4D5307E6.
// Reading between texels with half-pixel offset in 58410954.
DEFINE_bool(
    half_pixel_offset, true,
    "Enable support of vertex half-pixel offset (D3D9 PA_SU_VTX_CNTL "
    "PIX_CENTER). Generally games are aware of the half-pixel offset, and "
    "having this enabled is the correct behavior (disabling this may "
    "significantly break post-processing in some games), but in certain games "
    "it might have been ignored, resulting in slight blurriness of UI "
    "textures, for instance, when they are read between texels rather than "
    "at texel centers, or the leftmost/topmost pixels may not be fully covered "
    "when MSAA is used with fullscreen passes.",
    "GPU");

DEFINE_int32(query_occlusion_sample_lower_threshold, 80,
             "If set to -1 no sample counts are written, games may hang. Else, "
             "the sample count of every tile will be incremented on every "
             "EVENT_WRITE_ZPD by this number. Setting this to 0 means "
             "everything is reported as occluded.",
             "GPU");
DEFINE_int32(
    query_occlusion_sample_upper_threshold, 100,
    "Set to higher number than query_occlusion_sample_lower_threshold. This "
    "value is ignored if query_occlusion_sample_lower_threshold is set to -1.",
    "GPU");

DEFINE_bool(metal_shader_disk_cache, true,
            "Cache compiled Metal shader libraries (metallib) to disk when "
            "store_shaders is enabled.",
            "GPU");

DEFINE_bool(metal_pipeline_binary_archive, true,
            "Use MTLBinaryArchive for Metal pipeline compilation caching. "
            "Requires store_shaders and a compatible OS/driver.",
            "GPU");

DEFINE_bool(metal_pipeline_disk_cache, true,
            "Store Metal render pipeline descriptor keys to disk so the binary "
            "archive can be prewarmed on the next run.",
            "GPU");

DEFINE_int32(
    metal_draw_ring_count, 128,
    "Metal per-command-buffer draw ring size (descriptor-table pages). "
    "Higher reduces ring churn but uses more memory.",
    "GPU");

DEFINE_bool(metal_use_heaps, true,
            "Use MTLHeap-backed texture allocations in Metal to reduce "
            "allocation overhead and fragmentation.",
            "GPU");
DEFINE_bool(metal_shared_memory_zero_copy, true,
            "Use MTLBuffer bytes-no-copy for guest memory on unified memory "
            "devices when possible.",
            "GPU");
DEFINE_int32(metal_heap_min_bytes, 33554432,
             "Minimum heap size (bytes) for Metal heap allocations.", "GPU");

DEFINE_bool(metal_texture_cache_use_private, true,
            "Use MTLStorageModePrivate for Metal texture cache textures when "
            "GPU upload paths support it.",
            "GPU");
DEFINE_bool(metal_texture_upload_via_blit, true,
            "Upload textures via staging buffers and GPU blit copies instead "
            "of CPU replaceRegion.",
            "GPU");

DEFINE_bool(metal_log_cache_stats, false,
            "Log Metal cache memory statistics periodically.", "GPU");
DEFINE_int32(metal_log_cache_stats_interval_seconds, 5,
             "Interval in seconds for Metal cache stats logging.", "GPU");
