/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/resolve_vertex_processor.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/registers.h"

namespace xe {
namespace gpu {

void ResolveVertexProcessor::PositionExportSink::Export(
    ucode::ExportRegister export_register, const float* value,
    uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    for (uint32_t i = 0; i < xe::countof(position); ++i) {
      if (value_mask & (uint32_t(1) << i)) {
        position[i] = value[i];
      }
    }

    position_mask |= value_mask;

  } else if (export_register ==
                 ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex &&
             (value_mask & 0b0100)) {
    vertex_killed = (xe::memory::Reinterpret<uint32_t>(value[2]) &
                     ~(UINT32_C(1) << 31)) != 0;
  }
}

bool ResolveVertexProcessor::Process(const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");
  rectangles_.clear();

  const RegisterFile& regs = register_file_;
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  if (vgt_draw_initiator.prim_type != xenos::PrimitiveType::kRectangleList ||
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    XELOGE("Unsupported resolve primitive type {} or vertex index source {}",
           uint32_t(vgt_draw_initiator.prim_type),
           uint32_t(vgt_draw_initiator.source_select));
    return false;
  }

  assert_true(vertex_shader.type() == xenos::ShaderType::kVertex);
  assert_true(vertex_shader.is_ucode_analyzed());
  shader_interpreter_.SetShader(vertex_shader, true);

  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();

  float viewport_scale[2] = {
      pa_cl_vte_cntl.vport_x_scale_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE)
          : 1.0f,
      pa_cl_vte_cntl.vport_y_scale_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE)
          : 1.0f};
  float viewport_offset[2] = {
      pa_cl_vte_cntl.vport_x_offset_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET)
          : 0.0f,
      pa_cl_vte_cntl.vport_y_offset_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET)
          : 0.0f};

  bool vertex_kill_or = regs.Get<reg::PA_CL_CLIP_CNTL>().vtx_kill_or;
  auto rb_copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  float* temp_registers = shader_interpreter_.temp_registers();

  rectangles_.reserve(vgt_draw_initiator.num_indices / 3);

  for (uint32_t i = 0; i + 2 < vgt_draw_initiator.num_indices; i += 3) {
    draw_util::ResolveRectangle rectangle = {FLT_MAX, FLT_MAX, -FLT_MAX,
                                             -FLT_MAX};
    bool rectangle_valid = true;
    uint32_t vertices_killed = 0;

    for (uint32_t j = 0; j < 3; ++j) {
      position_export_sink_.Reset();
      std::memset(temp_registers, 0,
                  sizeof(float) * 4 * xenos::kMaxShaderTempRegisters);
      temp_registers[0] = float(std::min(
          max_index, std::max(min_index, (i + j + index_offset) &
                                             xenos::kVertexIndexMask)));

      shader_interpreter_.Execute();

      if (shader_interpreter_.was_texture_fetch_unsupported()) {
        XELOGE("Unsupported texture fetch in resolve vertex shader");
        rectangles_.clear();
        return false;
      }

      vertices_killed += uint32_t(position_export_sink_.vertex_killed);

      if ((position_export_sink_.position_mask & 0b0011) != 0b0011 ||
          (!pa_cl_vte_cntl.vtx_xy_fmt &&
           !(position_export_sink_.position_mask & 0b1000))) {
        rectangle_valid = false;
        continue;
      }

      float position[2];
      for (uint32_t k = 0; k < xe::countof(position); ++k) {
        position[k] = position_export_sink_.position[k];
        if (!pa_cl_vte_cntl.vtx_xy_fmt) {
          if (pa_cl_vte_cntl.vtx_w0_fmt) {
            position[k] /= position_export_sink_.position[3];
          } else {
            position[k] *= position_export_sink_.position[3];
          }
        }
        position[k] = position[k] * viewport_scale[k] + viewport_offset[k];
      }

      // The resolve vertex shader may use NaN positions to discard rectangles.
      if (!std::isfinite(position[0]) || !std::isfinite(position[1])) {
        rectangle_valid = false;
        continue;
      }

      rectangle.left = std::min(rectangle.left, position[0]);
      rectangle.top = std::min(rectangle.top, position[1]);
      rectangle.right = std::max(rectangle.right, position[0]);
      rectangle.bottom = std::max(rectangle.bottom, position[1]);
    }

    if (!rectangle_valid ||
        (vertex_kill_or ? vertices_killed != 0 : vertices_killed == 3) ||
        rectangle.left >= rectangle.right ||
        rectangle.top >= rectangle.bottom) {
      continue;
    }

    // Merge consecutive rectangles sharing a full edge unless they clear EDRAM.
    if (!rectangles_.empty() && !rb_copy_control.color_clear_enable &&
        !rb_copy_control.depth_clear_enable) {
      auto& previous = rectangles_.back();

      if (previous.top == rectangle.top &&
          previous.bottom == rectangle.bottom &&
          (previous.right == rectangle.left ||
           rectangle.right == previous.left)) {
        previous.left = std::min(previous.left, rectangle.left);
        previous.right = std::max(previous.right, rectangle.right);
        continue;
      }

      if (previous.left == rectangle.left &&
          previous.right == rectangle.right &&
          (previous.bottom == rectangle.top ||
           rectangle.bottom == previous.top)) {
        previous.top = std::min(previous.top, rectangle.top);
        previous.bottom = std::max(previous.bottom, rectangle.bottom);
        continue;
      }
    }

    rectangles_.push_back(rectangle);
  }

  return true;
}

}  // namespace gpu
}  // namespace xe
