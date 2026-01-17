/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GPU_FLAGS_H_
#define XENIA_GPU_GPU_FLAGS_H_
#include "xenia/base/cvar.h"

DECLARE_path(trace_gpu_prefix);
DECLARE_bool(trace_gpu_stream);

DECLARE_path(dump_shaders);

DECLARE_bool(vsync);

DECLARE_uint64(framerate_limit);

DECLARE_bool(gpu_allow_invalid_fetch_constants);

DECLARE_bool(non_seamless_cube_map);

DECLARE_bool(half_pixel_offset);

DECLARE_int32(query_occlusion_sample_lower_threshold);

DECLARE_int32(query_occlusion_sample_upper_threshold);

DECLARE_bool(metal_shader_disk_cache);
DECLARE_bool(metal_pipeline_binary_archive);
DECLARE_bool(metal_pipeline_disk_cache);
DECLARE_int32(metal_draw_ring_count);
DECLARE_bool(metal_use_heaps);
DECLARE_bool(metal_shared_memory_zero_copy);
DECLARE_int32(metal_heap_min_bytes);
DECLARE_bool(metal_texture_cache_use_private);
DECLARE_bool(metal_texture_upload_via_blit);
DECLARE_bool(metal_log_cache_stats);
DECLARE_int32(metal_log_cache_stats_interval_seconds);

DECLARE_bool(disassemble_pm4);

#define XE_GPU_FINE_GRAINED_DRAW_SCOPES 1

#endif  // XENIA_GPU_GPU_FLAGS_H_
