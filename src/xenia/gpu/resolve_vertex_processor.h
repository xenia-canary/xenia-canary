/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_RESOLVE_VERTEX_PROCESSOR_H_
#define XENIA_GPU_RESOLVE_VERTEX_PROCESSOR_H_

#include <cstdint>
#include <vector>

#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {

// Runs the vertex shader on the CPU to get rectangles to copy or clear.
//
// Only auto-indexed rectangle lists are supported so far. Resolves on the
// normal D3D path read their three float2 vertices directly from vf0, whereas
// this uses the shader's positions and lets it discard rectangles.
// Any inputs written by the GPU still need to be read back first.
class ResolveVertexProcessor {
 public:
  ResolveVertexProcessor(const RegisterFile& register_file,
                         const Memory& memory, TraceWriter& trace_writer)
      : register_file_(register_file),
        shader_interpreter_(register_file, memory) {
    shader_interpreter_.SetTraceWriter(&trace_writer);
    shader_interpreter_.SetExportSink(&position_export_sink_);
  }

  // Collects the draw's rectangles before the backend starts copying/clearing.
  // The command processor must analyze the shader's ucode before calling this.
  // Unsupported draws or texture fetches return false and discard the list,
  // and empty list on success means there's nothing to copy/clear.
  bool Process(const Shader& vertex_shader);
  const std::vector<draw_util::ResolveRectangle>& rectangles() const {
    return rectangles_;
  }

 private:
  // Receives each vertex's position and kill flag from the interpreter.
  // The transform and vertex kill mode are applied later by Process.
  struct PositionExportSink : ShaderInterpreter::ExportSink {
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;
    void Reset() {
      position_mask = 0;
      vertex_killed = false;
    }

    float position[4] = {};
    // Written components tracked so missing exports don't
    // recycle the last vertex's position.
    uint32_t position_mask = 0;
    bool vertex_killed = false;
  };

  const RegisterFile& register_file_;

  PositionExportSink position_export_sink_;
  ShaderInterpreter shader_interpreter_;

  std::vector<draw_util::ResolveRectangle> rectangles_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_RESOLVE_VERTEX_PROCESSOR_H_
