/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GPU_FLAGS_MAC_H_
#define XENIA_GPU_GPU_FLAGS_MAC_H_
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

DECLARE_bool(disassemble_pm4);
DECLARE_bool(gpu_log_pm4_swap_verbose);
DECLARE_int32(gpu_log_pm4_swap_every_n);
DECLARE_bool(gpu_log_pm4_indirect_verbose);
DECLARE_int32(gpu_log_pm4_indirect_every_n);
DECLARE_int32(gpu_log_pm4_indirect_dump_dwords);
DECLARE_bool(gpu_ignore_unimplemented_opcode);
DECLARE_bool(gpu_log_interrupts);
DECLARE_int32(gpu_log_interrupts_every_n);
DECLARE_bool(trace_gpu_bootstrap);

#define XE_GPU_FINE_GRAINED_DRAW_SCOPES 1

#endif  // XENIA_GPU_GPU_FLAGS_MAC_H_
