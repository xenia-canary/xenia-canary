/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/msl_bindings.h"

#include <dispatch/dispatch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "third_party/metal-cpp/Foundation/NSProcessInfo.hpp"
#include "third_party/metal-cpp/Foundation/NSURL.hpp"
#include "third_party/metal-cpp/Metal/MTLEvent.hpp"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/metal/metal_graphics_system.h"
#include "xenia/gpu/metal/metal_shader_cache.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/ui/metal/metal_presenter.h"

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

DECLARE_bool(clear_memory_page_state);
DECLARE_bool(submit_on_primary_buffer_end);
DEFINE_int32(
    metal_pipeline_creation_threads, -1,
    "Number of threads used for SPIRV-Cross shader and render pipeline "
    "compilation in the Metal backend. -1 to calculate automatically (75% of "
    "logical CPU cores), a positive number to specify the number of threads "
    "explicitly (up to the number of logical CPU cores), 0 to disable "
    "multithreaded compilation.",
    "Metal");

namespace xe {
namespace gpu {
namespace metal {

namespace {
bool UseSpirvCrossPath() { return true; }

void GetBoundRenderTargetSize(const MetalRenderTargetCache* render_target_cache,
                              uint32_t fallback_width, uint32_t fallback_height,
                              uint32_t& width_out, uint32_t& height_out) {
  width_out = std::max(fallback_width, uint32_t(1));
  height_out = std::max(fallback_height, uint32_t(1));
  if (!render_target_cache) {
    return;
  }
  MTL::Texture* pass_size_texture = render_target_cache->GetColorTarget(0);
  if (!pass_size_texture) {
    pass_size_texture = render_target_cache->GetDepthTarget();
  }
  if (!pass_size_texture) {
    pass_size_texture = render_target_cache->GetDummyColorTarget();
  }
  if (!pass_size_texture) {
    return;
  }
  width_out =
      std::max(static_cast<uint32_t>(pass_size_texture->width()), uint32_t(1));
  height_out =
      std::max(static_cast<uint32_t>(pass_size_texture->height()), uint32_t(1));
}

void ClampScissorToBounds(draw_util::Scissor& scissor, uint32_t width,
                          uint32_t height) {
  width = std::max(width, uint32_t(1));
  height = std::max(height, uint32_t(1));

  scissor.offset[0] = std::min(scissor.offset[0], width);
  scissor.offset[1] = std::min(scissor.offset[1], height);

  uint32_t max_scissor_width = width - scissor.offset[0];
  uint32_t max_scissor_height = height - scissor.offset[1];
  scissor.extent[0] = std::min(scissor.extent[0], max_scissor_width);
  scissor.extent[1] = std::min(scissor.extent[1], max_scissor_height);
}

void LogMetalErrorDetails(const char* label, NS::Error* error) {
  if (!error) {
    return;
  }
  const char* desc = error->localizedDescription()
                         ? error->localizedDescription()->utf8String()
                         : nullptr;
  const char* failure = error->localizedFailureReason()
                            ? error->localizedFailureReason()->utf8String()
                            : nullptr;
  const char* recovery =
      error->localizedRecoverySuggestion()
          ? error->localizedRecoverySuggestion()->utf8String()
          : nullptr;
  const char* domain =
      error->domain() ? error->domain()->utf8String() : nullptr;
  int64_t code = error->code();
  XELOGE("{}: domain={} code={} desc='{}' failure='{}' recovery='{}'", label,
         domain ? domain : "<null>", code, desc ? desc : "<null>",
         failure ? failure : "<null>", recovery ? recovery : "<null>");
  NS::Dictionary* user_info = error->userInfo();
  if (user_info) {
    auto* info_desc = user_info->description();
    XELOGE("{}: userInfo={}", label,
           info_desc ? info_desc->utf8String() : "<null>");
  }
}

constexpr int64_t kMslAsyncLogIntervalNs =
    int64_t(std::chrono::nanoseconds(std::chrono::seconds(1)).count());
constexpr size_t kResolvedMemoryRangesMax = 8192;

int64_t GetSteadyTimeNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool ShouldLogRateLimited(std::atomic<int64_t>& last_log_ns,
                          int64_t interval_ns) {
  const int64_t now = GetSteadyTimeNs();
  int64_t previous = last_log_ns.load(std::memory_order_relaxed);
  while (now - previous >= interval_ns) {
    if (last_log_ns.compare_exchange_weak(previous, now,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}
void PopulatePipelineFormatsFromRenderPassDescriptor(
    MTL::RenderPassDescriptor* pass_descriptor, MTL::PixelFormat* color_formats,
    uint32_t color_count, MTL::PixelFormat* depth_format,
    MTL::PixelFormat* stencil_format, uint32_t* sample_count) {
  if (!pass_descriptor) {
    return;
  }

  auto update_sample_count = [&](MTL::Texture* texture) {
    if (!texture || !sample_count) {
      return;
    }
    NS::UInteger sc = texture->sampleCount();
    if (sc > 0) {
      *sample_count =
          std::max<uint32_t>(*sample_count, static_cast<uint32_t>(sc));
    }
  };

  if (color_formats) {
    auto* color_attachments = pass_descriptor->colorAttachments();
    for (uint32_t i = 0; i < color_count; ++i) {
      auto* attachment =
          color_attachments ? color_attachments->object(i) : nullptr;
      if (!attachment) {
        continue;
      }
      MTL::Texture* texture = attachment->texture();
      if (!texture) {
        continue;
      }
      color_formats[i] = texture->pixelFormat();
      update_sample_count(texture);
    }
  }

  if (depth_format) {
    if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
      MTL::Texture* texture = depth_attachment->texture();
      if (texture) {
        *depth_format = texture->pixelFormat();
        update_sample_count(texture);
      }
    }
  }

  if (stencil_format) {
    if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
      MTL::Texture* texture = stencil_attachment->texture();
      if (texture) {
        *stencil_format = texture->pixelFormat();
        update_sample_count(texture);
      }
    }
  }

  if (depth_format && stencil_format) {
    if (*depth_format != MTL::PixelFormatInvalid &&
        *stencil_format == MTL::PixelFormatInvalid) {
      switch (*depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          *stencil_format = *depth_format;
          break;
        default:
          break;
      }
    } else if (*stencil_format != MTL::PixelFormatInvalid &&
               *depth_format == MTL::PixelFormatInvalid) {
      switch (*stencil_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          *depth_format = *stencil_format;
          break;
        default:
          break;
      }
    }
  }
}

void EnsureDepthFormatForDepthWritingFragment(const char* pipeline_name,
                                              bool fragment_writes_depth,
                                              MTL::PixelFormat* depth_format) {
  if (!fragment_writes_depth || !depth_format ||
      *depth_format != MTL::PixelFormatInvalid) {
    return;
  }
  // Metal requires a valid depth attachment format if the fragment shader
  // writes depth even when the current render pass has no depth target bound.
  *depth_format = MTL::PixelFormatDepth32Float;
  static bool logged = false;
  if (!logged) {
    logged = true;
    XELOGW(
        "{}: fragment writes depth without a bound depth attachment; "
        "using Depth32Float pipeline fallback",
        pipeline_name);
  }
}

MTL::ComputePipelineState* CreateComputePipelineFromEmbeddedLibrary(
    MTL::Device* device, const void* metallib_data, size_t metallib_size,
    const char* debug_name) {
  if (!device || !metallib_data || !metallib_size) {
    return nullptr;
  }

  NS::Error* error = nullptr;
  dispatch_data_t data = dispatch_data_create(
      metallib_data, metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* lib = device->newLibrary(data, &error);
  dispatch_release(data);
  if (!lib) {
    XELOGE("Metal: failed to create {} library: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  // XeSL compute entrypoint name used in the embedded metallibs.
  NS::String* fn_name = NS::String::string("entry_xe", NS::UTF8StringEncoding);
  MTL::Function* fn = lib->newFunction(fn_name);
  if (!fn) {
    XELOGE("Metal: {} missing entry_xe", debug_name);
    lib->release();
    return nullptr;
  }

  MTL::ComputePipelineState* pipeline =
      device->newComputePipelineState(fn, &error);
  fn->release();
  lib->release();

  if (!pipeline) {
    XELOGE("Metal: failed to create {} pipeline: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  return pipeline;
}

constexpr uint32_t kPipelineDiskCacheMagic = 0x43504D58;  // 'XMPC'
constexpr uint32_t kPipelineDiskCacheVersion = 2;
constexpr size_t kPipelineDiskCacheMaxEntrySize = 1 << 20;

XEPACKEDSTRUCT(PipelineDiskCacheHeader, {
  uint32_t magic;
  uint32_t version;
  uint32_t reserved[2];
});

XEPACKEDSTRUCT(PipelineDiskCacheEntryHeader, {
  uint32_t entry_size;
  uint32_t reserved;
});

XEPACKEDSTRUCT(PipelineDiskCacheEntryBase, {
  uint64_t pipeline_key;
  uint64_t vertex_shader_cache_key;
  uint64_t pixel_shader_cache_key;
  uint32_t sample_count;
  uint32_t depth_format;
  uint32_t stencil_format;
  uint32_t color_formats[4];
  uint32_t normalized_color_mask;
  uint32_t alpha_to_mask_enable;
  uint32_t blendcontrol[4];
  uint32_t vertex_attribute_count;
  uint32_t vertex_layout_count;
});

static_assert(sizeof(PipelineDiskCacheHeader) == 16,
              "Unexpected pipeline disk cache header size.");
static_assert(sizeof(PipelineDiskCacheEntryHeader) == 8,
              "Unexpected pipeline disk cache entry header size.");

bool ShaderUsesVertexFetch(const Shader& shader) {
  if (!shader.vertex_bindings().empty()) {
    return true;
  }
  const Shader::ConstantRegisterMap& constant_map =
      shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    if (constant_map.vertex_fetch_bitmap[i] != 0) {
      return true;
    }
  }
  return false;
}

MTL::CompareFunction ToMetalCompareFunction(xenos::CompareFunction compare) {
  static const MTL::CompareFunction kCompareMap[8] = {
      MTL::CompareFunctionNever,         // 0
      MTL::CompareFunctionLess,          // 1
      MTL::CompareFunctionEqual,         // 2
      MTL::CompareFunctionLessEqual,     // 3
      MTL::CompareFunctionGreater,       // 4
      MTL::CompareFunctionNotEqual,      // 5
      MTL::CompareFunctionGreaterEqual,  // 6
      MTL::CompareFunctionAlways,        // 7
  };
  return kCompareMap[uint32_t(compare) & 0x7];
}

MTL::StencilOperation ToMetalStencilOperation(xenos::StencilOp op) {
  static const MTL::StencilOperation kStencilOpMap[8] = {
      MTL::StencilOperationKeep,            // 0
      MTL::StencilOperationZero,            // 1
      MTL::StencilOperationReplace,         // 2
      MTL::StencilOperationIncrementClamp,  // 3
      MTL::StencilOperationDecrementClamp,  // 4
      MTL::StencilOperationInvert,          // 5
      MTL::StencilOperationIncrementWrap,   // 6
      MTL::StencilOperationDecrementWrap,   // 7
  };
  return kStencilOpMap[uint32_t(op) & 0x7];
}

MTL::ColorWriteMask ToMetalColorWriteMask(uint32_t write_mask) {
  MTL::ColorWriteMask mtl_mask = MTL::ColorWriteMaskNone;
  if (write_mask & 0x1) {
    mtl_mask |= MTL::ColorWriteMaskRed;
  }
  if (write_mask & 0x2) {
    mtl_mask |= MTL::ColorWriteMaskGreen;
  }
  if (write_mask & 0x4) {
    mtl_mask |= MTL::ColorWriteMaskBlue;
  }
  if (write_mask & 0x8) {
    mtl_mask |= MTL::ColorWriteMaskAlpha;
  }
  return mtl_mask;
}

MTL::BlendOperation ToMetalBlendOperation(xenos::BlendOp blend_op) {
  // 8 entries for safety since 3 bits from the guest are passed directly.
  static const MTL::BlendOperation kBlendOpMap[8] = {
      MTL::BlendOperationAdd,              // 0
      MTL::BlendOperationSubtract,         // 1
      MTL::BlendOperationMin,              // 2
      MTL::BlendOperationMax,              // 3
      MTL::BlendOperationReverseSubtract,  // 4
      MTL::BlendOperationAdd,              // 5
      MTL::BlendOperationAdd,              // 6
      MTL::BlendOperationAdd,              // 7
  };
  return kBlendOpMap[uint32_t(blend_op) & 0x7];
}

MTL::BlendFactor ToMetalBlendFactorRgb(xenos::BlendFactor blend_factor) {
  // 32 because of 0x1F mask, for safety (all unknown to zero).
  static const MTL::BlendFactor kBlendFactorMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,  // ?
      /*  3 */ MTL::BlendFactorZero,  // ?
      /*  4 */ MTL::BlendFactorSourceColor,
      /*  5 */ MTL::BlendFactorOneMinusSourceColor,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationColor,
      /*  9 */ MTL::BlendFactorOneMinusDestinationColor,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendColor,  // CONSTANT_COLOR
      /* 13 */ MTL::BlendFactorOneMinusBlendColor,
      /* 14 */ MTL::BlendFactorBlendAlpha,  // CONSTANT_ALPHA
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorMap[uint32_t(blend_factor) & 0x1F];
}

MTL::BlendFactor ToMetalBlendFactorAlpha(xenos::BlendFactor blend_factor) {
  // Like the RGB map, but with color modes changed to alpha.
  static const MTL::BlendFactor kBlendFactorAlphaMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,  // ?
      /*  3 */ MTL::BlendFactorZero,  // ?
      /*  4 */ MTL::BlendFactorSourceAlpha,
      /*  5 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationAlpha,
      /*  9 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendAlpha,
      /* 13 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 14 */ MTL::BlendFactorBlendAlpha,
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorAlphaMap[uint32_t(blend_factor) & 0x1F];
}

}  // namespace

MetalCommandProcessor::MetalCommandProcessor(
    MetalGraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

std::string MetalCommandProcessor::GetTitleStateSuffix() const {
  if (!render_target_cache_) {
    return {};
  }
  std::ostringstream suffix;
  suffix << " - SPIRV-Cross";
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    suffix << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
  }
  return suffix.str();
}

MetalCommandProcessor::~MetalCommandProcessor() {
  // End any active render encoder before releasing
  // Note: Only call endEncoding if the encoder is still active
  // (not already ended by a committed command buffer)
  if (current_render_encoder_) {
    // The encoder may already be ended if the command buffer was committed
    // In that case, just release it
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  WaitForPendingCompletionHandlers();
  ShutdownMslAsyncCompilation();
  if (render_pass_descriptor_) {
    render_pass_descriptor_->release();
    render_pass_descriptor_ = nullptr;
  }
  if (render_target_texture_) {
    render_target_texture_->release();
    render_target_texture_ = nullptr;
  }
  if (depth_stencil_texture_) {
    depth_stencil_texture_->release();
    depth_stencil_texture_ = nullptr;
  }

  for (auto& pair : depth_stencil_state_cache_) {
    if (pair.second) {
      pair.second->release();
    }
  }
  depth_stencil_state_cache_.clear();

  // Release IR Converter runtime buffers and resources
  if (null_buffer_) {
    null_buffer_->release();
    null_buffer_ = nullptr;
  }
  if (null_texture_) {
    null_texture_->release();
    null_texture_ = nullptr;
  }
  if (null_sampler_) {
    null_sampler_->release();
    null_sampler_ = nullptr;
  }
  uniforms_buffer_ = nullptr;
  command_buffer_spirv_uniform_buffers_.clear();
  {
    std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
    spirv_uniforms_available_.clear();
    for (MTL::Buffer* pool_uniforms : spirv_uniforms_pool_) {
      if (pool_uniforms) {
        pool_uniforms->release();
      }
    }
    spirv_uniforms_pool_.clear();
    spirv_uniforms_pool_initialized_ = false;
  }
  if (spirv_uniforms_available_semaphore_) {
#if !OS_OBJECT_USE_OBJC
    dispatch_release(spirv_uniforms_available_semaphore_);
#endif
    spirv_uniforms_available_semaphore_ = nullptr;
  }
}

void MetalCommandProcessor::InitializeMslAsyncCompilation() {
  ShutdownMslAsyncCompilation();

  if (!cvars::async_shader_compilation) {
    return;
  }

  uint32_t logical_processor_count = std::thread::hardware_concurrency();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }

  if (cvars::metal_pipeline_creation_threads == 0) {
    return;
  }

  size_t thread_count = 0;
  if (cvars::metal_pipeline_creation_threads < 0) {
    thread_count = std::max<uint32_t>(logical_processor_count * 3 / 4, 1);
  } else {
    thread_count =
        std::min<uint32_t>(uint32_t(cvars::metal_pipeline_creation_threads),
                           logical_processor_count);
  }
  if (!thread_count) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
    msl_shader_compile_shutdown_ = false;
  }

  msl_shader_compile_threads_.reserve(thread_count);
  for (size_t i = 0; i < thread_count; ++i) {
    msl_shader_compile_threads_.emplace_back(
        [this, i]() { MslShaderCompileThread(i); });
  }

  XELOGI(
      "SPIRV-Cross: async Metal shader/pipeline compilation enabled with {} "
      "worker "
      "thread(s)",
      thread_count);
}

void MetalCommandProcessor::ShutdownMslAsyncCompilation() {
  {
    std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
    msl_shader_compile_shutdown_ = true;
  }
  msl_shader_compile_cv_.notify_all();

  for (std::thread& thread : msl_shader_compile_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  msl_shader_compile_threads_.clear();

  std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
  std::priority_queue<MslShaderCompileRequest,
                      std::vector<MslShaderCompileRequest>,
                      MslShaderCompileRequestCompare>
      empty_queue;
  std::swap(msl_shader_compile_queue_, empty_queue);
  while (!msl_pipeline_compile_queue_.empty()) {
    auto request = msl_pipeline_compile_queue_.top();
    msl_pipeline_compile_queue_.pop();
    if (request.vertex_function) {
      request.vertex_function->release();
      request.vertex_function = nullptr;
    }
    if (request.fragment_function) {
      request.fragment_function->release();
      request.fragment_function = nullptr;
    }
  }
  msl_shader_compile_pending_.clear();
  msl_shader_compile_failed_.clear();
  msl_pipeline_compile_pending_.clear();
  msl_pipeline_compile_failed_.clear();
  msl_shader_compile_busy_ = 0;
  msl_shader_compile_shutdown_ = false;
}

MetalCommandProcessor::MslShaderCompileStatus
MetalCommandProcessor::GetMslShaderCompileStatus(
    MslShader::MslTranslation* translation) {
  if (!translation) {
    return MslShaderCompileStatus::kFailed;
  }

  std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
  if (msl_shader_compile_failed_.find(translation) !=
      msl_shader_compile_failed_.end()) {
    return MslShaderCompileStatus::kFailed;
  }
  if (msl_shader_compile_pending_.find(translation) !=
      msl_shader_compile_pending_.end()) {
    return MslShaderCompileStatus::kPending;
  }
  return translation->is_valid() ? MslShaderCompileStatus::kReady
                                 : MslShaderCompileStatus::kNotQueued;
}

bool MetalCommandProcessor::EnqueueMslShaderCompilation(
    MslShader::MslTranslation* translation, bool is_ios, uint8_t priority) {
  if (!translation || !cvars::async_shader_compilation ||
      msl_shader_compile_threads_.empty()) {
    return false;
  }

  if (translation->is_valid()) {
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
    if (msl_shader_compile_failed_.find(translation) !=
        msl_shader_compile_failed_.end()) {
      return false;
    }
    if (msl_shader_compile_pending_.find(translation) !=
        msl_shader_compile_pending_.end()) {
      return true;
    }

    MslShaderCompileRequest request;
    request.translation = translation;
    request.shader_hash = translation->shader().ucode_data_hash();
    request.modification = translation->modification();
    request.is_ios = is_ios;
    request.priority = priority;
    msl_shader_compile_pending_.insert(translation);
    msl_shader_compile_queue_.push(request);
  }
  msl_shader_compile_cv_.notify_one();
  return true;
}

bool MetalCommandProcessor::EnqueueMslPipelineCompilation(
    const MslPipelineCompileRequest& request) {
  if (!cvars::async_shader_compilation || msl_shader_compile_threads_.empty() ||
      !request.vertex_function) {
    return false;
  }

  MslPipelineCompileRequest queued_request = request;
  {
    std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
    if (msl_pipeline_cache_.find(request.pipeline_key) !=
        msl_pipeline_cache_.end()) {
      return true;
    }
    if (msl_pipeline_compile_failed_.find(request.pipeline_key) !=
        msl_pipeline_compile_failed_.end()) {
      return false;
    }
    if (msl_pipeline_compile_pending_.find(request.pipeline_key) !=
        msl_pipeline_compile_pending_.end()) {
      return true;
    }

    queued_request.vertex_function->retain();
    if (queued_request.fragment_function) {
      queued_request.fragment_function->retain();
    }
    msl_pipeline_compile_pending_.insert(request.pipeline_key);
    msl_pipeline_compile_queue_.push(queued_request);
  }

  msl_shader_compile_cv_.notify_one();
  return true;
}

MTL::RenderPipelineState* MetalCommandProcessor::CreateMslPipelineState(
    const MslPipelineCompileRequest& request, std::string* error_out) {
  if (error_out) {
    error_out->clear();
  }
  if (!request.vertex_function) {
    if (error_out) {
      *error_out = "missing vertex shader function";
    }
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(request.vertex_function);
  if (request.fragment_function) {
    desc->setFragmentFunction(request.fragment_function);
  }

  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(
        request.color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(request.depth_format);
  desc->setStencilAttachmentPixelFormat(request.stencil_format);
  desc->setSampleCount(request.sample_count);
  desc->setAlphaToCoverageEnabled(request.alpha_to_mask_enable != 0);

  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    if (request.color_formats[i] == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    uint32_t rt_write_mask = (request.normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    reg::RB_BLENDCONTROL blendcontrol;
    blendcontrol.value = request.blendcontrol[i];

    MTL::BlendFactor src_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_srcblend);
    MTL::BlendFactor dst_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_destblend);
    MTL::BlendOperation op_rgb =
        ToMetalBlendOperation(blendcontrol.color_comb_fcn);
    MTL::BlendFactor src_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_srcblend);
    MTL::BlendFactor dst_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_destblend);
    MTL::BlendOperation op_alpha =
        ToMetalBlendOperation(blendcontrol.alpha_comb_fcn);

    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    color_attachment->setBlendingEnabled(blending_enabled);
    if (blending_enabled) {
      color_attachment->setSourceRGBBlendFactor(src_rgb);
      color_attachment->setDestinationRGBBlendFactor(dst_rgb);
      color_attachment->setRgbBlendOperation(op_rgb);
      color_attachment->setSourceAlphaBlendFactor(src_alpha);
      color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
      color_attachment->setAlphaBlendOperation(op_alpha);
    }
  }

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);
  desc->release();

  if (!pipeline && error_out && error) {
    NS::String* description = error->localizedDescription();
    if (description) {
      *error_out = description->utf8String();
    }
  }

  return pipeline;
}

void MetalCommandProcessor::MslShaderCompileThread(size_t thread_index) {
  while (true) {
    MslShaderCompileRequest shader_request;
    MslPipelineCompileRequest pipeline_request;
    bool process_pipeline_request = false;
    {
      std::unique_lock<std::mutex> lock(msl_shader_compile_mutex_);
      msl_shader_compile_cv_.wait(lock, [this]() {
        return msl_shader_compile_shutdown_ ||
               !msl_shader_compile_queue_.empty() ||
               !msl_pipeline_compile_queue_.empty();
      });
      if (msl_shader_compile_shutdown_) {
        return;
      }
      if (!msl_pipeline_compile_queue_.empty()) {
        process_pipeline_request = true;
        pipeline_request = msl_pipeline_compile_queue_.top();
        msl_pipeline_compile_queue_.pop();
      } else {
        shader_request = msl_shader_compile_queue_.top();
        msl_shader_compile_queue_.pop();
      }
      ++msl_shader_compile_busy_;
    }

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    if (process_pipeline_request) {
      std::string pipeline_error;
      MTL::RenderPipelineState* pipeline =
          CreateMslPipelineState(pipeline_request, &pipeline_error);
      bool compiled = pipeline != nullptr;

      {
        std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
        msl_pipeline_compile_pending_.erase(pipeline_request.pipeline_key);
        if (compiled) {
          auto insert_result = msl_pipeline_cache_.emplace(
              pipeline_request.pipeline_key, pipeline);
          if (!insert_result.second && pipeline) {
            pipeline->release();
          }
          msl_pipeline_compile_failed_.erase(pipeline_request.pipeline_key);
        } else {
          msl_pipeline_compile_failed_.insert(pipeline_request.pipeline_key);
        }
        if (msl_shader_compile_busy_) {
          --msl_shader_compile_busy_;
        }
      }

      if (pipeline_request.vertex_function) {
        pipeline_request.vertex_function->release();
      }
      if (pipeline_request.fragment_function) {
        pipeline_request.fragment_function->release();
      }

      if (!compiled &&
          ShouldLogRateLimited(msl_pipeline_compile_failure_last_log_ns_,
                               kMslAsyncLogIntervalNs)) {
        if (!pipeline_error.empty()) {
          XELOGE(
              "SPIRV-Cross: async Metal pipeline compile failed on worker {} "
              "(VS {:016X} mod {:016X}, PS {:016X} mod {:016X}): {}",
              thread_index, pipeline_request.vertex_shader_hash,
              pipeline_request.vertex_modification,
              pipeline_request.pixel_shader_hash,
              pipeline_request.pixel_modification, pipeline_error);
        } else {
          XELOGE(
              "SPIRV-Cross: async Metal pipeline compile failed on worker {} "
              "(VS {:016X} mod {:016X}, PS {:016X} mod {:016X})",
              thread_index, pipeline_request.vertex_shader_hash,
              pipeline_request.vertex_modification,
              pipeline_request.pixel_shader_hash,
              pipeline_request.pixel_modification);
        }
      }
    } else {
      bool compiled = false;
      if (shader_request.translation) {
        compiled = shader_request.translation->CompileToMsl(
            device_, shader_request.is_ios);
      }

      {
        std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
        if (shader_request.translation) {
          msl_shader_compile_pending_.erase(shader_request.translation);
          if (!compiled) {
            msl_shader_compile_failed_.insert(shader_request.translation);
          }
        }
        if (msl_shader_compile_busy_) {
          --msl_shader_compile_busy_;
        }
      }

      if (!compiled &&
          ShouldLogRateLimited(msl_shader_compile_failure_last_log_ns_,
                               kMslAsyncLogIntervalNs)) {
        XELOGE(
            "SPIRV-Cross: async Metal compile failed on worker {} (shader "
            "{:016X}, mod {:016X})",
            thread_index, shader_request.shader_hash,
            shader_request.modification);
      }
    }
    pool->release();
  }
}

MetalCommandProcessor::SpirvArgumentBufferPage::~SpirvArgumentBufferPage() {
  if (buffer) {
    buffer->release();
    buffer = nullptr;
  }
}

void MetalCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                     uint32_t length) {
  if (shared_memory_) {
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
  if (primitive_processor_) {
    primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
  }
}

void MetalCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Restore the guest EDRAM snapshot captured in the trace into the Metal
  // render-target cache so that subsequent host render targets created from
  // EDRAM (via LoadTiledData) see the same initial contents as other
  // backends like D3D12.
  if (!snapshot) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called with null "
        "snapshot");
    return;
  }
  if (!render_target_cache_) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called before render "
        "target "
        "cache initialization");
    return;
  }
  // Trace playback frame boundary: drop resolve-write tracking from previous
  // frame before restoring a new snapshot.
  ClearResolvedMemory();
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

void MetalCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  // TODO(wmarti): Add cache_clear_requested_ flag like D3D12 for deferred
  // clearing of pipeline caches, texture caches, etc.
}

void MetalCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void MetalCommandProcessor::ClearReadbackBuffers() {
  // TODO(wmarti): Implement readback buffer clearing when memexport readback
  // is added. See D3D12's readback_buffers_ and memexport_readback_buffers_.
}

ui::metal::MetalProvider& MetalCommandProcessor::GetMetalProvider() const {
  return *static_cast<ui::metal::MetalProvider*>(graphics_system_->provider());
}

uint64_t MetalCommandProcessor::GetCurrentSubmission() const {
  return submission_current_ ? submission_current_ : 1;
}

uint64_t MetalCommandProcessor::GetCompletedSubmission() const {
  return completed_command_buffers_.load(std::memory_order_relaxed);
}

void MetalCommandProcessor::MarkResolvedMemory(uint32_t base_ptr,
                                               uint32_t length) {
  if (length == 0) {
    return;
  }
  constexpr uint64_t kAddressLimit =
      uint64_t(std::numeric_limits<uint32_t>::max()) + 1ull;
  uint64_t merged_base = base_ptr;
  uint64_t merged_end =
      std::min<uint64_t>(merged_base + uint64_t(length), kAddressLimit);
  if (merged_end <= merged_base) {
    return;
  }

  for (size_t i = 0; i < resolved_memory_ranges_.size();) {
    const auto& range = resolved_memory_ranges_[i];
    const uint64_t range_base = range.base;
    const uint64_t range_end =
        std::min<uint64_t>(range_base + uint64_t(range.length), kAddressLimit);
    // Merge overlapping or adjacent ranges.
    if (merged_end + 1 < range_base || range_end + 1 < merged_base) {
      ++i;
      continue;
    }
    merged_base = std::min(merged_base, range_base);
    merged_end = std::max(merged_end, range_end);
    resolved_memory_ranges_.erase(resolved_memory_ranges_.begin() + i);
  }

  const uint64_t merged_length_64 =
      std::min<uint64_t>(merged_end - merged_base, kAddressLimit - merged_base);
  if (!merged_length_64) {
    return;
  }
  ResolvedRange merged_range = {uint32_t(merged_base),
                                uint32_t(merged_length_64)};
  auto insert_it = std::lower_bound(
      resolved_memory_ranges_.begin(), resolved_memory_ranges_.end(),
      merged_range, [](const ResolvedRange& lhs, const ResolvedRange& rhs) {
        return lhs.base < rhs.base;
      });
  resolved_memory_ranges_.insert(insert_it, merged_range);

  if (resolved_memory_ranges_.size() <= kResolvedMemoryRangesMax) {
    return;
  }

  std::sort(resolved_memory_ranges_.begin(), resolved_memory_ranges_.end(),
            [](const ResolvedRange& lhs, const ResolvedRange& rhs) {
              return lhs.base < rhs.base;
            });
  while (resolved_memory_ranges_.size() > kResolvedMemoryRangesMax) {
    size_t best_index = std::numeric_limits<size_t>::max();
    uint64_t best_gap = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i + 1 < resolved_memory_ranges_.size(); ++i) {
      const auto& left = resolved_memory_ranges_[i];
      const auto& right = resolved_memory_ranges_[i + 1];
      const uint64_t left_end = uint64_t(left.base) + uint64_t(left.length);
      const uint64_t right_base = uint64_t(right.base);
      const uint64_t gap = right_base > left_end ? right_base - left_end : 0;
      if (gap < best_gap) {
        best_gap = gap;
        best_index = i;
        if (!gap) {
          break;
        }
      }
    }
    if (best_index == std::numeric_limits<size_t>::max()) {
      break;
    }
    auto& left = resolved_memory_ranges_[best_index];
    const auto& right = resolved_memory_ranges_[best_index + 1];
    const uint64_t merged_base_64 =
        std::min<uint64_t>(left.base, uint64_t(right.base));
    const uint64_t merged_end_64 =
        std::max<uint64_t>(uint64_t(left.base) + uint64_t(left.length),
                           uint64_t(right.base) + uint64_t(right.length));
    const uint64_t merged_len_64 = std::min<uint64_t>(
        merged_end_64 - merged_base_64, kAddressLimit - merged_base_64);
    left.base = uint32_t(merged_base_64);
    left.length = uint32_t(std::max<uint64_t>(1, merged_len_64));
    resolved_memory_ranges_.erase(resolved_memory_ranges_.begin() + best_index +
                                  1);
  }
}

bool MetalCommandProcessor::IsResolvedMemory(uint32_t base_ptr,
                                             uint32_t length) const {
  const uint64_t end_ptr = uint64_t(base_ptr) + uint64_t(length);
  for (const auto& range : resolved_memory_ranges_) {
    const uint64_t range_end = uint64_t(range.base) + uint64_t(range.length);
    // Check if ranges overlap
    if (uint64_t(base_ptr) < range_end && end_ptr > uint64_t(range.base)) {
      return true;
    }
  }
  return false;
}

void MetalCommandProcessor::ClearResolvedMemory() {
  resolved_memory_ranges_.clear();
}

void MetalCommandProcessor::ForceIssueSwap() {
  // Force a swap to push any pending render target to presenter
  // This is used by trace dumps to capture output when there's no explicit swap
  if (saw_swap_) {
    return;
  }
  IssueSwap(0, render_target_width_, render_target_height_);
}

void MetalCommandProcessor::SetSwapDestSwap(uint32_t dest_base, bool swap) {
  if (!dest_base) {
    return;
  }
  if (swap_dest_swaps_by_base_.size() > 256) {
    swap_dest_swaps_by_base_.clear();
  }
  swap_dest_swaps_by_base_[dest_base] = swap;
}

bool MetalCommandProcessor::ConsumeSwapDestSwap(uint32_t dest_base,
                                                bool* swap_out) {
  if (!swap_out || !dest_base) {
    return false;
  }
  auto it = swap_dest_swaps_by_base_.find(dest_base);
  if (it == swap_dest_swaps_by_base_.end()) {
    return false;
  }
  *swap_out = it->second;
  swap_dest_swaps_by_base_.erase(it);
  return true;
}

bool MetalCommandProcessor::SetupContext() {
  saw_swap_ = false;
  last_swap_ptr_ = 0;
  last_swap_width_ = 0;
  last_swap_height_ = 0;
  swap_dest_swaps_by_base_.clear();
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  const ui::metal::MetalProvider& provider = GetMetalProvider();
  device_ = provider.GetDevice();
  command_queue_ = provider.GetCommandQueue();

  if (!device_ || !command_queue_) {
    XELOGE("MetalCommandProcessor: No Metal device or command queue available");
    return false;
  }

  wait_shared_event_ = device_->newSharedEvent();
  if (wait_shared_event_) {
    wait_shared_event_->setLabel(
        NS::String::string("XeniaWaitEvent", NS::UTF8StringEncoding));
    wait_shared_event_value_ = 0;
  } else {
    XELOGW(
        "MetalCommandProcessor: SharedEvent unavailable; falling back to "
        "waitUntilCompleted");
  }

  bool supports_apple7 = device_->supportsFamily(MTL::GPUFamilyApple7);
  bool supports_mac2 = device_->supportsFamily(MTL::GPUFamilyMac2);
  mesh_shader_supported_ = supports_apple7 || supports_mac2;

  draw_ring_count_ = std::max<int32_t>(1, ::cvars::metal_draw_ring_count);
  if (UseSpirvCrossPath()) {
    // Large per-command-buffer ring sizes have been observed to corrupt
    // SPIRV-Cross uniform/constant data; cap here and rely on multi-buffer
    // pool growth in EnsureSpirvUniformBuffer* for throughput.
    constexpr size_t kMaxSpirvRingPagesPerCommandBuffer = 8;
    if (draw_ring_count_ > kMaxSpirvRingPagesPerCommandBuffer) {
      XELOGW(
          "SPIRV-Cross: clamping per-command-buffer ring pages from {} to {} "
          "for correctness",
          draw_ring_count_, kMaxSpirvRingPagesPerCommandBuffer);
      draw_ring_count_ = kMaxSpirvRingPagesPerCommandBuffer;
    }
  }
  msl_system_constants_version_ = 1;
  msl_clip_plane_constants_version_ = 1;
  msl_tessellation_constants_version_ = 1;
  msl_constants_versioned_uniform_buffer_ = nullptr;
  msl_system_constants_written_vertex_versions_.assign(draw_ring_count_, 0);
  msl_system_constants_written_pixel_versions_.assign(draw_ring_count_, 0);
  msl_clip_plane_constants_written_vertex_versions_.assign(draw_ring_count_, 0);
  msl_tessellation_constants_written_vertex_versions_.assign(draw_ring_count_,
                                                             0);
  msl_tessellation_constants_written_pixel_versions_.assign(draw_ring_count_,
                                                            0);
  msl_current_float_constant_map_vertex_.fill(0);
  msl_current_float_constant_map_pixel_.fill(0);
  msl_float_constants_dirty_vertex_ = true;
  msl_float_constants_dirty_pixel_ = true;
  msl_bool_loop_constants_dirty_ = true;
  msl_fetch_constants_dirty_ = true;
  msl_bound_vertex_texture_binding_uid_ = 0;
  msl_bound_pixel_texture_binding_uid_ = 0;
  msl_bound_vertex_sampler_binding_uid_ = 0;
  msl_bound_pixel_sampler_binding_uid_ = 0;
  msl_bound_vertex_argument_buffer_offset_ = 0;
  msl_bound_pixel_argument_buffer_offset_ = 0;
  msl_bound_vertex_argument_buffer_offset_valid_ = false;
  msl_bound_pixel_argument_buffer_offset_valid_ = false;
  msl_last_argbuf_vertex_translation_ = nullptr;
  msl_last_argbuf_vertex_encoded_length_ = 0;
  msl_last_argbuf_vertex_layout_uid_ = 0;
  msl_last_argbuf_pixel_translation_ = nullptr;
  msl_last_argbuf_pixel_encoded_length_ = 0;
  msl_last_argbuf_pixel_layout_uid_ = 0;
  msl_bound_uniforms_buffer_ = nullptr;
  msl_bound_uniforms_vs_base_offset_ = 0;
  msl_bound_uniforms_ps_base_offset_ = 0;
  msl_bound_uniforms_offsets_valid_ = false;

  // Initialize shared memory
  shared_memory_ = std::make_unique<MetalSharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  // Initialize primitive processor (index/primitive conversion like D3D12).
  primitive_processor_ = std::make_unique<MetalPrimitiveProcessor>(
      *this, *register_file_, *memory_, trace_writer_, *shared_memory_);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize Metal primitive processor");
    return false;
  }

  texture_cache_ = std::make_unique<MetalTextureCache>(this, *register_file_,
                                                       *shared_memory_, 1, 1);
  if (!texture_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal texture cache");
    return false;
  }

  // Initialize render target cache
  render_target_cache_ = std::make_unique<MetalRenderTargetCache>(
      *register_file_, *memory_, &trace_writer_, 1, 1, *this);
  if (!render_target_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal render target cache");
    return false;
  }

  // Initialize shader translation pipeline
  if (!InitializeShaderTranslation()) {
    XELOGE("Failed to initialize shader translation");
    return false;
  }
  if (UseSpirvCrossPath()) {
    InitializeMslAsyncCompilation();
  }

  // Create render target texture for offscreen rendering
  MTL::TextureDescriptor* color_desc = MTL::TextureDescriptor::alloc()->init();
  color_desc->setTextureType(MTL::TextureType2D);
  color_desc->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
  color_desc->setWidth(render_target_width_);
  color_desc->setHeight(render_target_height_);
  color_desc->setStorageMode(MTL::StorageModePrivate);
  color_desc->setUsage(MTL::TextureUsageRenderTarget |
                       MTL::TextureUsageShaderRead);

  render_target_texture_ = device_->newTexture(color_desc);
  color_desc->release();

  if (!render_target_texture_) {
    XELOGE("Failed to create render target texture");
    return false;
  }
  render_target_texture_->setLabel(
      NS::String::string("XeniaRenderTarget", NS::UTF8StringEncoding));

  // Create depth/stencil texture
  MTL::TextureDescriptor* depth_desc = MTL::TextureDescriptor::alloc()->init();
  depth_desc->setTextureType(MTL::TextureType2D);
  depth_desc->setPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
  depth_desc->setWidth(render_target_width_);
  depth_desc->setHeight(render_target_height_);
#if XE_PLATFORM_IOS
  // This fallback depth/stencil target is transient (clear/dontcare only) and
  // never sampled, so memoryless is the most efficient iOS storage mode.
  depth_desc->setStorageMode(MTL::StorageModeMemoryless);
#else
  depth_desc->setStorageMode(MTL::StorageModePrivate);
#endif
  depth_desc->setUsage(MTL::TextureUsageRenderTarget);

  depth_stencil_texture_ = device_->newTexture(depth_desc);
  depth_desc->release();

  if (!depth_stencil_texture_) {
    XELOGE("Failed to create depth/stencil texture");
    return false;
  }
  depth_stencil_texture_->setLabel(
      NS::String::string("XeniaDepthStencil", NS::UTF8StringEncoding));

  // Create render pass descriptor
  render_pass_descriptor_ = MTL::RenderPassDescriptor::alloc()->init();

  auto color_attachment =
      render_pass_descriptor_->colorAttachments()->object(0);
  color_attachment->setTexture(render_target_texture_);
  color_attachment->setLoadAction(MTL::LoadActionClear);
  color_attachment->setStoreAction(MTL::StoreActionStore);
  color_attachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));

  auto depth_attachment = render_pass_descriptor_->depthAttachment();
  depth_attachment->setTexture(depth_stencil_texture_);
  depth_attachment->setLoadAction(MTL::LoadActionClear);
  depth_attachment->setStoreAction(MTL::StoreActionDontCare);
  depth_attachment->setClearDepth(1.0);

  auto stencil_attachment = render_pass_descriptor_->stencilAttachment();
  stencil_attachment->setTexture(depth_stencil_texture_);
  stencil_attachment->setLoadAction(MTL::LoadActionClear);
  stencil_attachment->setStoreAction(MTL::StoreActionDontCare);
  stencil_attachment->setClearStencil(0);

  // Create a null buffer for unused descriptor entries
  // This prevents shader validation errors when accessing unpopulated
  // descriptors
  null_buffer_ =
      device_->newBuffer(kNullBufferSize, MTL::ResourceStorageModeShared);
  if (!null_buffer_) {
    XELOGE("Failed to create null buffer");
    return false;
  }
  null_buffer_->setLabel(
      NS::String::string("NullBuffer", NS::UTF8StringEncoding));
  std::memset(null_buffer_->contents(), 0, kNullBufferSize);

  // Create a 1x1x1 placeholder 2D array texture for unbound texture slots
  // Xbox 360 textures are typically 2D arrays (for texture atlases, cubemaps)
  // Using 2DArray prevents "Invalid texture type" validation errors
  MTL::TextureDescriptor* null_tex_desc =
      MTL::TextureDescriptor::alloc()->init();
  null_tex_desc->setTextureType(MTL::TextureType2DArray);
  null_tex_desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  null_tex_desc->setWidth(1);
  null_tex_desc->setHeight(1);
  null_tex_desc->setArrayLength(1);  // Single slice in the array
  null_tex_desc->setStorageMode(MTL::StorageModeShared);
  null_tex_desc->setUsage(MTL::TextureUsageShaderRead);

  null_texture_ = device_->newTexture(null_tex_desc);
  null_tex_desc->release();

  if (!null_texture_) {
    XELOGE("Failed to create null texture");
    return false;
  }
  null_texture_->setLabel(
      NS::String::string("NullTexture2DArray", NS::UTF8StringEncoding));

  // Fill the 1x1x1 texture with opaque white (helps debug if sampled)
  uint32_t white_pixel = 0xFFFFFFFF;
  MTL::Region region =
      MTL::Region(0, 0, 0, 1, 1, 1);  // x,y,z origin, w,h,d size
  null_texture_->replaceRegion(region, 0, 0, &white_pixel, 4, 0);  // slice 0

  // Create a default sampler for unbound sampler slots
  // Must set supportsArgumentBuffers=YES for use in argument buffers
  MTL::SamplerDescriptor* null_smp_desc =
      MTL::SamplerDescriptor::alloc()->init();
  null_smp_desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMipFilter(MTL::SamplerMipFilterLinear);
  null_smp_desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setSupportArgumentBuffers(true);

  null_sampler_ = device_->newSamplerState(null_smp_desc);
  null_smp_desc->release();

  if (!null_sampler_) {
    XELOGE("Failed to create null sampler");
    return false;
  }

  // SPIRV-Cross path: use command-buffer-scoped uniforms buffers so CPU writes
  // to the next submission can't race with in-flight GPU reads.
  if (UseSpirvCrossPath()) {
    if (!EnsureSpirvUniformBuffer()) {
      return false;
    }
  }

  return true;
}

bool MetalCommandProcessor::InitializeShaderTranslation() {
  // Initialize DXBC shader translator (use Apple vendor ID for Metal)
  // Must query render_target_cache_ for actual runtime parameters.
  // Metal doesn't use ROV (rasterizer ordered views) path.
  bool edram_rov_used = false;

  // gamma_render_target_as_unorm8: When true, shaders include code to convert
  // linear -> gamma for 8-bit gamma render targets. When false, we use 16-bit
  // UNORM format where hardware handles gamma implicitly.
  bool gamma_render_target_as_unorm8 = !(
      edram_rov_used || render_target_cache_->gamma_render_target_as_unorm16());

  XELOGI(
      "DxbcShaderTranslator init: gamma_as_unorm8={}, msaa_2x={}, scale={}x{}",
      gamma_render_target_as_unorm8, render_target_cache_->msaa_2x_supported(),
      render_target_cache_->draw_resolution_scale_x(),
      render_target_cache_->draw_resolution_scale_y());

  // Initialize SPIRV-Cross (MSL) path.
  if (UseSpirvCrossPath()) {
    // Build SpirvShaderTranslator::Features for Metal.
    // Enable all features as a baseline, then disable what Metal doesn't need.
    SpirvShaderTranslator::Features spirv_features(true);
    // Not using fragment shader interlock — we use host render targets.
    spirv_features.fragment_shader_sample_interlock = false;
    // Barycentric interpolation not needed for current Metal path.
    spirv_features.fragment_shader_barycentric = false;
    // Metal fast-math doesn't guarantee IEEE NaN/Inf preservation.
    spirv_features.signed_zero_inf_nan_preserve_float32 = false;
    // Metal fast-math flushes denorms.
    spirv_features.denorm_flush_to_zero_float32 = true;
    // RTE rounding not guaranteed by Metal.
    spirv_features.rounding_mode_rte_float32 = false;

    spirv_shader_translator_ = std::make_unique<SpirvShaderTranslator>(
        spirv_features,
        render_target_cache_->msaa_2x_supported(),  // native_2x_msaa_with_att
        false,                                      // native_2x_msaa_no_att
        false,  // edram_fragment_shader_interlock (host RT path)
        render_target_cache_->draw_resolution_scale_x(),
        render_target_cache_->draw_resolution_scale_y());

    XELOGI(
        "SpirvShaderTranslator init (SPIRV-Cross MSL path): msaa_2x={}, "
        "scale={}x{}",
        render_target_cache_->msaa_2x_supported(),
        render_target_cache_->draw_resolution_scale_x(),
        render_target_cache_->draw_resolution_scale_y());

    if (!InitializeMslTessellation()) {
      XELOGW(
          "SPIRV-Cross: Tessellation factor pipelines failed to init; "
          "tessellated draws will be skipped");
    }
  }

  return true;
}

void MetalCommandProcessor::PrepareForWait() {
  // Flush any pending Metal command buffers before entering wait state.
  // This is critical because:
  // 1. The worker thread's autorelease pool will be drained when it exits
  // 2. Metal objects in that pool might still be referenced by in-flight
  // commands
  // 3. Releasing those objects during pool drain can hang waiting for GPU
  // completion
  //
  // By submitting and waiting for all GPU work now, we ensure clean pool
  // drainage.

  // End through the shared helper so per-encoder bind caches are reset too.
  EndRenderEncoder();

  if (current_command_buffer_) {
    uint64_t wait_value = 0;
    if (wait_shared_event_) {
      wait_value = ++wait_shared_event_value_;
      current_command_buffer_->encodeSignalEvent(wait_shared_event_,
                                                 wait_value);
    }
    if (UseSpirvCrossPath()) {
      ScheduleSpirvUniformBufferRelease(current_command_buffer_);
    }
    ScheduleSpirvArgumentBufferRelease(current_command_buffer_);
    current_command_buffer_->commit();
    if (wait_shared_event_) {
      wait_shared_event_->waitUntilSignaledValue(
          wait_value, std::numeric_limits<uint64_t>::max());
    } else {
      current_command_buffer_->waitUntilCompleted();
    }
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    current_draw_index_ = 0;
    copy_resolve_writes_pending_ = false;
  }
  DrainCommandBufferAutoreleasePool();

  // Even if we have no active command buffer, there might be GPU work from
  // previously submitted command buffers that autoreleased objects depend on.
  // Submit and wait for a dummy command buffer to ensure ALL GPU work
  // completes.
  if (command_queue_) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    // Note: commandBuffer() returns an autoreleased object per metal-cpp docs.
    // We do NOT call release() since we didn't retain() it.
    // The autorelease pool will handle cleanup.
    MTL::CommandBuffer* sync_cmd = command_queue_->commandBuffer();
    if (sync_cmd) {
      uint64_t wait_value = 0;
      if (wait_shared_event_) {
        wait_value = ++wait_shared_event_value_;
        sync_cmd->encodeSignalEvent(wait_shared_event_, wait_value);
      }
      sync_cmd->commit();
      if (wait_shared_event_) {
        wait_shared_event_->waitUntilSignaledValue(
            wait_value, std::numeric_limits<uint64_t>::max());
      } else {
        sync_cmd->waitUntilCompleted();
      }
      // Don't release - it's autoreleased and will be cleaned up by the pool
    }
    pool->release();
  }

  // Also call the base class to flush trace writer
  CommandProcessor::PrepareForWait();
}

void MetalCommandProcessor::WaitForPendingCompletionHandlers() {
  constexpr auto kMaxWait = std::chrono::seconds(5);
  const auto wait_start = std::chrono::steady_clock::now();
  while (pending_completion_handlers_.load(std::memory_order_acquire) != 0) {
    if (std::chrono::steady_clock::now() - wait_start >= kMaxWait) {
      XELOGW(
          "MetalCommandProcessor: timed out waiting for {} completion "
          "handler(s) during shutdown",
          pending_completion_handlers_.load(std::memory_order_relaxed));
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void MetalCommandProcessor::ShutdownContext() {
  // End any active render encoder before shutdown
  if (current_render_encoder_) {
    current_render_encoder_->endEncoding();
    // Don't release yet - wait until command buffer completes
  }

  // Submit and wait for any pending command buffer
  if (current_command_buffer_) {
    uint64_t wait_value = 0;
    if (wait_shared_event_) {
      wait_value = ++wait_shared_event_value_;
      current_command_buffer_->encodeSignalEvent(wait_shared_event_,
                                                 wait_value);
    }
    if (UseSpirvCrossPath()) {
      ScheduleSpirvUniformBufferRelease(current_command_buffer_);
    }
    ScheduleSpirvArgumentBufferRelease(current_command_buffer_);
    current_command_buffer_->commit();
    if (wait_shared_event_) {
      wait_shared_event_->waitUntilSignaledValue(
          wait_value, std::numeric_limits<uint64_t>::max());
    } else {
      current_command_buffer_->waitUntilCompleted();
    }
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    current_draw_index_ = 0;
    copy_resolve_writes_pending_ = false;
  }

  // Even if we have no active command buffer at this point, there may be
  // previously committed command buffers still in flight. Submit and wait for
  // a dummy command buffer to ensure all GPU work on this queue has completed
  // before tearing down resources on thread exit.
  if (command_queue_) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::CommandBuffer* sync_cmd = command_queue_->commandBuffer();
    if (sync_cmd) {
      uint64_t wait_value = 0;
      if (wait_shared_event_) {
        wait_value = ++wait_shared_event_value_;
        sync_cmd->encodeSignalEvent(wait_shared_event_, wait_value);
      }
      sync_cmd->commit();
      if (wait_shared_event_) {
        wait_shared_event_->waitUntilSignaledValue(
            wait_value, std::numeric_limits<uint64_t>::max());
      } else {
        sync_cmd->waitUntilCompleted();
      }
    }
    pool->release();
  }

  WaitForPendingCompletionHandlers();

  // Now safe to release encoder and command buffer
  if (current_render_encoder_) {
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  DrainCommandBufferAutoreleasePool();

  {
    std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
    command_buffer_spirv_argbuf_pages_.clear();
    pending_spirv_argbuf_releases_.clear();
    spirv_argbuf_pool_.clear();
  }

  if (texture_cache_) {
    texture_cache_->Shutdown();
    texture_cache_.reset();
  }

  if (primitive_processor_) {
    primitive_processor_->Shutdown();
    primitive_processor_.reset();
  }
  frame_open_ = false;

  ShutdownMslAsyncCompilation();

  // SPIRV-Cross resources.
  ShutdownMslTessellation();
  for (auto& [key, pso] : msl_pipeline_cache_) {
    if (pso) {
      pso->release();
    }
  }
  msl_pipeline_cache_.clear();
  msl_shader_cache_.clear();
  spirv_shader_translator_.reset();

  uniforms_buffer_ = nullptr;
  command_buffer_spirv_uniform_buffers_.clear();
  {
    std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
    spirv_uniforms_available_.clear();
    for (MTL::Buffer* pool_uniforms : spirv_uniforms_pool_) {
      if (pool_uniforms) {
        pool_uniforms->release();
      }
    }
    spirv_uniforms_pool_.clear();
    spirv_uniforms_pool_initialized_ = false;
  }
  if (spirv_uniforms_available_semaphore_) {
#if !OS_OBJECT_USE_OBJC
    dispatch_release(spirv_uniforms_available_semaphore_);
#endif
    spirv_uniforms_available_semaphore_ = nullptr;
  }

  shared_memory_.reset();
  if (wait_shared_event_) {
    wait_shared_event_->release();
    wait_shared_event_ = nullptr;
  }

  ClearMslShaderSourceCacheDirectory();

  CommandProcessor::ShutdownContext();
}

void MetalCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking,
                                            nullptr);

  if (!device_) {
    XELOGW("Metal shader storage init skipped (no device)");
    if (completion_callback) {
      completion_callback();
    }
    return;
  }

  std::string device_tag = "unknown";
  if (device_->name()) {
    device_tag = device_->name()->utf8String();
  }
  for (char& ch : device_tag) {
    if (!std::isalnum(static_cast<unsigned char>(ch))) {
      ch = '_';
    }
  }

  std::filesystem::path shader_storage_title_root =
      cache_root / "shaders" / "metal" / "local" / device_tag /
      fmt::format("{:08X}", title_id);
  std::error_code ec;
  std::filesystem::create_directories(shader_storage_title_root, ec);
  if (ec) {
    XELOGW("Metal shader storage: Failed to create {}: {}",
           shader_storage_title_root.string(), ec.message());
  } else if (::cvars::metal_shader_disk_cache && g_metal_shader_cache) {
    g_metal_shader_cache->Initialize(shader_storage_title_root / "metallib");
    SetMslShaderSourceCacheDirectory(shader_storage_title_root / "msl_source");
  } else {
    SetMslShaderSourceCacheDirectory(shader_storage_title_root / "msl_source");
  }

  if (completion_callback) {
    completion_callback();
  }
}

void MetalCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  ProcessCompletedSubmissions();
  saw_swap_ = true;
  last_swap_ptr_ = frontbuffer_ptr;
  last_swap_width_ = frontbuffer_width;
  last_swap_height_ = frontbuffer_height;

  // End any active render encoder
  EndRenderEncoder();

  // Submit and wait for command buffer
  if (current_command_buffer_) {
    if (UseSpirvCrossPath()) {
      ScheduleSpirvUniformBufferRelease(current_command_buffer_);
    }
    ScheduleSpirvArgumentBufferRelease(current_command_buffer_);
    current_command_buffer_->commit();
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    current_draw_index_ = 0;
    copy_resolve_writes_pending_ = false;
  }

  if (primitive_processor_ && frame_open_) {
    primitive_processor_->EndFrame();
    frame_open_ = false;
  }
  // Frame boundary reached - resolved memory tracking is only needed within a
  // frame when trace playback writes memory.
  ClearResolvedMemory();
  if (shared_memory_ && ::cvars::clear_memory_page_state) {
    shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
  }

  // Push the rendered frame to the presenter's guest output mailbox
  // This is required for trace dumps to capture the output. Use the
  // MetalRenderTargetCache color target (like D3D12) rather than the
  // legacy standalone render_target_texture_.
  auto* presenter =
      static_cast<ui::metal::MetalPresenter*>(graphics_system_->presenter());
  if (presenter && render_target_cache_) {
    uint32_t output_width =
        frontbuffer_width ? frontbuffer_width : render_target_width_;
    uint32_t output_height =
        frontbuffer_height ? frontbuffer_height : render_target_height_;

    MTL::Texture* source_texture = nullptr;
    bool use_pwl_gamma_ramp = false;
    if (texture_cache_) {
      uint32_t swap_width = 0;
      uint32_t swap_height = 0;
      xenos::TextureFormat swap_format = xenos::TextureFormat::k_8_8_8_8;
      source_texture = texture_cache_->RequestSwapTexture(
          swap_width, swap_height, swap_format);
      if (source_texture) {
        output_width = swap_width;
        output_height = swap_height;
        use_pwl_gamma_ramp =
            swap_format == xenos::TextureFormat::k_2_10_10_10 ||
            swap_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        static MTL::PixelFormat last_format = MTL::PixelFormatInvalid;
        static uint32_t last_samples = 0;
        static uint32_t last_width = 0;
        static uint32_t last_height = 0;
        static int last_swap_format = -1;
        MTL::PixelFormat src_format = source_texture->pixelFormat();
        uint32_t src_samples = source_texture->sampleCount();
        uint32_t src_width = uint32_t(source_texture->width());
        uint32_t src_height = uint32_t(source_texture->height());
        int swap_format_int = static_cast<int>(swap_format);
        if (src_format != last_format || src_samples != last_samples ||
            src_width != last_width || src_height != last_height ||
            swap_format_int != last_swap_format) {
          last_format = src_format;
          last_samples = src_samples;
          last_width = src_width;
          last_height = src_height;
          last_swap_format = swap_format_int;
        }
        if (presenter) {
          if (!gamma_ramp_256_entry_table_up_to_date_ ||
              !gamma_ramp_pwl_up_to_date_) {
            constexpr size_t kGammaRampTableBytes =
                sizeof(reg::DC_LUT_30_COLOR) * 256;
            constexpr size_t kGammaRampPwlBytes =
                sizeof(reg::DC_LUT_PWL_DATA) * 128 * 3;
            if (presenter->UpdateGammaRamp(
                    gamma_ramp_256_entry_table(), kGammaRampTableBytes,
                    gamma_ramp_pwl_rgb(), kGammaRampPwlBytes)) {
              gamma_ramp_256_entry_table_up_to_date_ = true;
              gamma_ramp_pwl_up_to_date_ = true;
            } else {
              XELOGW("Metal IssueSwap: gamma ramp upload failed");
            }
          }
        }
      }
    }

    bool swap_dest_swap = false;
    const bool has_swap_dest_swap =
        ConsumeSwapDestSwap(frontbuffer_ptr, &swap_dest_swap);
    if (!has_swap_dest_swap && frontbuffer_ptr) {
      static uint32_t swap_dest_miss_count = 0;
      if (swap_dest_miss_count < 8) {
        ++swap_dest_miss_count;
      }
    }
    bool force_swap_rb = has_swap_dest_swap && swap_dest_swap;

    if (!source_texture) {
      static bool missing_swap_logged = false;
      if (!missing_swap_logged) {
        missing_swap_logged = true;
        XELOGW(
            "MetalCommandProcessor::IssueSwap: swap texture unavailable; "
            "presenting inactive (black) output");
      }
      presenter->RefreshGuestOutput(
          0, 0, 0, 0, [](ui::Presenter::GuestOutputRefreshContext&) -> bool {
            return false;
          });
      return;
    }

    if (source_texture) {
      ui::metal::MetalPresenter* metal_presenter = presenter;
      uint32_t source_width = output_width;
      uint32_t source_height = output_height;
      bool force_swap_rb_copy = force_swap_rb;
      bool use_pwl_gamma_ramp_copy = use_pwl_gamma_ramp;
      auto aspect = graphics_system_->GetScaledAspectRatio();
      presenter->RefreshGuestOutput(
          output_width, output_height, aspect.first, aspect.second,
          [source_texture, metal_presenter, source_width, source_height,
           force_swap_rb_copy, use_pwl_gamma_ramp_copy](
              ui::Presenter::GuestOutputRefreshContext& context) -> bool {
            auto& metal_context =
                static_cast<ui::metal::MetalGuestOutputRefreshContext&>(
                    context);
            context.SetIs8bpc(!use_pwl_gamma_ramp_copy);
            uint64_t submission_id = 0;
            bool copy_success = metal_presenter->CopyTextureToGuestOutput(
                source_texture, metal_context.resource_uav_capable(),
                source_width, source_height, force_swap_rb_copy,
                use_pwl_gamma_ramp_copy, &submission_id);
            if (copy_success && submission_id) {
              metal_context.SetSubmissionId(submission_id);
            }
            return copy_success;
          });
    }
  }
}

void MetalCommandProcessor::OnPrimaryBufferEnd() {
  if (!current_command_buffer_) {
    return;
  }

  // In the SPIRV-Cross path, keep command buffers open across primary-buffer
  // boundaries unless a copy->draw visibility boundary is pending.
  if (UseSpirvCrossPath() && !copy_resolve_writes_pending_) {
    return;
  }

  if (!cvars::submit_on_primary_buffer_end) {
    return;
  }

  if (!copy_resolve_writes_pending_ && !CanEndSubmissionImmediately()) {
    return;
  }
  EndCommandBuffer();
}

bool MetalCommandProcessor::CanEndSubmissionImmediately() {
  if (!current_command_buffer_) {
    return true;
  }
  if (!cvars::async_shader_compilation || msl_shader_compile_threads_.empty()) {
    return true;
  }
  std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
  return msl_shader_compile_busy_ == 0 && msl_shader_compile_queue_.empty() &&
         msl_pipeline_compile_queue_.empty() &&
         msl_shader_compile_pending_.empty() &&
         msl_pipeline_compile_pending_.empty();
}

Shader* MetalCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                          const uint32_t* host_address,
                                          uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));

  auto it = msl_shader_cache_.find(hash);
  if (it != msl_shader_cache_.end()) {
    return it->second.get();
  }
  auto shader =
      std::make_unique<MslShader>(shader_type, hash, host_address, dword_count);
  MslShader* result = shader.get();
  msl_shader_cache_[hash] = std::move(shader);
  return result;
}

bool MetalCommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  const RegisterFile& regs = *register_file_;
  uint32_t normalized_color_mask = 0;

  // Check for copy mode
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kCopy && copy_resolve_writes_pending_) {
    // Preserve resolve write visibility when transitioning from copy-only
    // bursts to regular draw work.
    // MSC keeps the conservative split behavior; SPIRV-Cross decides in
    // IssueDrawMsl based on actual texture overlap with pending resolve writes.
    if (!UseSpirvCrossPath()) {
      EndCommandBuffer();
    }
  }
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }

  // Vertex shader analysis — use Shader* base type so both MSC and
  // SPIRV-Cross paths share this common code.
  Shader* vertex_shader = active_vertex_shader();
  if (!vertex_shader) {
    XELOGW("IssueDraw: No vertex shader");
    return false;
  }
  if (!vertex_shader->is_ucode_analyzed()) {
    vertex_shader->AnalyzeUcode(ucode_disasm_buffer_);
  }
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = active_pixel_shader();
      if (pixel_shader) {
        if (!pixel_shader->is_ucode_analyzed()) {
          pixel_shader->AnalyzeUcode(ucode_disasm_buffer_);
        }
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    if (!memexport_used_vertex) {
      return true;
    }
  }
  bool memexport_used_pixel =
      pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  // Primitive/index processing (like D3D12/Vulkan).
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_) {
    XELOGE("IssueDraw: primitive processor is not initialized");
    return false;
  }
  if (!primitive_processor_->Process(primitive_processing_result)) {
    XELOGE("IssueDraw: primitive processing failed");
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kMemExportCompute) {
    primitive_processing_result.host_vertex_shader_type =
        Shader::HostVertexShaderType::kVertex;
  }

  bool use_tessellation_emulation = false;
  if (primitive_processing_result.IsTessellated()) {
    // The MSC path uses mesh shader emulation for tessellation, so it requires
    // mesh shader support. The SPIRV-Cross path uses native Metal tessellation
    // (drawPatches), so mesh shaders are not needed.
    if (!UseSpirvCrossPath() && !mesh_shader_supported_) {
      static bool tess_mesh_logged = false;
      if (!tess_mesh_logged) {
        tess_mesh_logged = true;
        XELOGW(
            "Metal: Tessellation emulation requested but mesh shaders are not "
            "supported on this device");
      }
      return true;
    }
    if (!pixel_shader) {
      static bool tess_no_ps_logged = false;
      if (!tess_no_ps_logged) {
        tess_no_ps_logged = true;
        XELOGW(
            "Metal: Tessellation emulation requested without a pixel shader; "
            "using depth-only PS fallback");
      }
    }
    use_tessellation_emulation = true;
  }

  // Configure render targets via MetalRenderTargetCache, similar to D3D12.
  if (render_target_cache_) {
    auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
    uint32_t ps_writes_color_targets =
        pixel_shader ? pixel_shader->writes_color_targets() : 0;
    normalized_color_mask = pixel_shader ? draw_util::GetNormalizedColorMask(
                                               regs, ps_writes_color_targets)
                                         : 0;
    if (!render_target_cache_->Update(is_rasterization_done,
                                      normalized_depth_control,
                                      normalized_color_mask, *vertex_shader)) {
      XELOGE(
          "MetalCommandProcessor::IssueDraw - RenderTargetCache::Update "
          "failed");
      return false;
    }
  }

  // Begin command buffer if needed (will use cache-provided render targets).
  BeginCommandBuffer();
  if (!current_command_buffer_ || !current_render_encoder_) {
    static bool spirv_no_command_buffer_logged = false;
    if (!spirv_no_command_buffer_logged) {
      spirv_no_command_buffer_logged = true;
      XELOGE(
          "IssueDraw: failed to begin Metal command buffer/render encoder; "
          "skipping draws until uniforms buffer allocation recovers");
    }
    return UseSpirvCrossPath();
  }
  if (UseSpirvCrossPath() && !EnsureSpirvUniformBufferCapacity()) {
    XELOGE(
        "IssueDraw: failed to prepare SPIRV-Cross uniforms ring; skipping "
        "draw");
    return true;
  }

  // =========================================================================
  // SPIRV-Cross (MSL) draw path — bypasses the entire MSC / IRRuntime flow.
  // =========================================================================
  if (UseSpirvCrossPath()) {
    return IssueDrawMsl(vertex_shader, pixel_shader,
                        primitive_processing_result, primitive_polygonal,
                        is_rasterization_done, memexport_used,
                        normalized_color_mask, regs);
  }

  XELOGE("MSC draw path not available");
  return false;
}

// ==========================================================================
// SPIRV-Cross (MSL) draw path
// ==========================================================================
bool MetalCommandProcessor::IssueDrawMsl(
    Shader* vertex_shader, Shader* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool primitive_polygonal, bool is_rasterization_done, bool memexport_used,
    uint32_t normalized_color_mask, const RegisterFile& regs) {
  assert_not_null(vertex_shader);
  // Cast to MslShader for the SPIRV-Cross path.
  auto* msl_vertex_shader = static_cast<MslShader*>(vertex_shader);
  auto* msl_pixel_shader = static_cast<MslShader*>(pixel_shader);

  // Tessellation draws use Metal's native tessellation: tessellation factors
  // are computed on the CPU and the domain shader (TES) runs as the
  // post-tessellation vertex function.
  const bool is_tessellated = primitive_processing_result.IsTessellated();

  // Determine the host vertex shader type for geometry expansion.
  // The primitive processor has already set kPointListAsTriangleStrip or
  // kRectangleListAsTriangleStrip when VS expansion is needed (enabled by
  // setting point_sprites_supported_without_vs_expansion = false in the
  // primitive processor init when spirvcross is active).
  Shader::HostVertexShaderType host_vertex_shader_type =
      primitive_processing_result.host_vertex_shader_type;

  // Compute interpolator mask for shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask = 0;
  if (msl_pixel_shader) {
    interpolator_mask = msl_vertex_shader->writes_interpolators() &
                        msl_pixel_shader->GetInterpolatorInputMask(
                            regs.Get<reg::SQ_PROGRAM_CNTL>(),
                            regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos);
  }

  auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  // Compute SPIRV shader modifications.
  SpirvShaderTranslator::Modification vertex_shader_modification =
      GetCurrentSpirvVertexShaderModification(
          *msl_vertex_shader, host_vertex_shader_type, interpolator_mask);
  SpirvShaderTranslator::Modification pixel_shader_modification =
      msl_pixel_shader
          ? GetCurrentSpirvPixelShaderModification(
                *msl_pixel_shader, interpolator_mask, ps_param_gen_pos,
                normalized_depth_control, normalized_color_mask)
          : SpirvShaderTranslator::Modification(0);

  // Sanity: if a pixel shader writes color targets and any RT is enabled,
  // color_targets_used must be non-zero or fragments will produce no output.
  if (msl_pixel_shader && msl_pixel_shader->writes_color_targets() &&
      normalized_color_mask) {
    assert_not_zero(pixel_shader_modification.pixel.color_targets_used);
  }

  // Get or create shader translations.
  constexpr bool kIsIos =
#if XE_PLATFORM_IOS
      true;
#else
      false;
#endif
  auto* vertex_translation = static_cast<MslShader::MslTranslation*>(
      msl_vertex_shader->GetOrCreateTranslation(
          vertex_shader_modification.value));
  if (!vertex_translation->is_translated()) {
    if (!spirv_shader_translator_->TranslateAnalyzedShader(
            *vertex_translation)) {
      XELOGE("SPIRV-Cross: Failed to translate vertex shader to SPIR-V");
      return false;
    }
  }
  auto ensure_msl_translation_ready =
      [&](MslShader::MslTranslation* translation,
          uint8_t priority) -> MslShaderCompileStatus {
    if (!translation) {
      return MslShaderCompileStatus::kFailed;
    }
    MslShaderCompileStatus status = GetMslShaderCompileStatus(translation);
    if (status == MslShaderCompileStatus::kReady ||
        status == MslShaderCompileStatus::kPending ||
        status == MslShaderCompileStatus::kFailed) {
      return status;
    }
    if (EnqueueMslShaderCompilation(translation, kIsIos, priority)) {
      return MslShaderCompileStatus::kPending;
    }
    if (!translation->CompileToMsl(device_, kIsIos)) {
      std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
      msl_shader_compile_failed_.insert(translation);
      return MslShaderCompileStatus::kFailed;
    }
    return MslShaderCompileStatus::kReady;
  };
  auto log_pending_compile = [&](MslShader::MslTranslation* translation,
                                 const char* stage_tag) {
    static auto last_pending_log = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_pending_log < std::chrono::seconds(1)) {
      return;
    }
    last_pending_log = now;
    XELOGI(
        "SPIRV-Cross: Skipping draw - {} shader compile pending "
        "(shader={:016X}, mod={:016X})",
        stage_tag, translation->shader().ucode_data_hash(),
        translation->modification());
  };
  MslShaderCompileStatus vertex_compile_status =
      ensure_msl_translation_ready(vertex_translation, 1);
  if (vertex_compile_status == MslShaderCompileStatus::kPending) {
    log_pending_compile(vertex_translation, "vertex");
    return true;
  }
  if (vertex_compile_status == MslShaderCompileStatus::kFailed) {
    XELOGE("SPIRV-Cross: Failed to prepare vertex shader MSL/library/function");
    return false;
  }

  MslShader::MslTranslation* pixel_translation = nullptr;
  if (msl_pixel_shader) {
    pixel_translation = static_cast<MslShader::MslTranslation*>(
        msl_pixel_shader->GetOrCreateTranslation(
            pixel_shader_modification.value));
    if (!pixel_translation->is_translated()) {
      if (!spirv_shader_translator_->TranslateAnalyzedShader(
              *pixel_translation)) {
        XELOGE("SPIRV-Cross: Failed to translate pixel shader to SPIR-V");
        return false;
      }
    }
    MslShaderCompileStatus pixel_compile_status =
        ensure_msl_translation_ready(pixel_translation, 2);
    if (pixel_compile_status == MslShaderCompileStatus::kPending) {
      log_pending_compile(pixel_translation, "pixel");
      return true;
    }
    if (pixel_compile_status == MslShaderCompileStatus::kFailed) {
      XELOGE(
          "SPIRV-Cross: Failed to prepare pixel shader MSL/library/function");
      return false;
    }
  }

  // Create or retrieve pipeline state.
  MTL::RenderPipelineState* pipeline = nullptr;
  MslPipelineCompileStatus pipeline_compile_status =
      MslPipelineCompileStatus::kReady;
  if (is_tessellated) {
    pipeline = GetOrCreateMslTessPipelineState(
        vertex_translation, pixel_translation, host_vertex_shader_type, regs);
  } else {
    pipeline = GetOrCreateMslPipelineState(
        vertex_translation, pixel_translation, regs, &pipeline_compile_status);
  }
  if (!pipeline) {
    if (!is_tessellated &&
        pipeline_compile_status == MslPipelineCompileStatus::kPending) {
      if (ShouldLogRateLimited(msl_pipeline_pending_last_log_ns_,
                               kMslAsyncLogIntervalNs)) {
        XELOGI(
            "SPIRV-Cross: Skipping draw - render pipeline compile pending "
            "(VS {:016X} mod {:016X}, PS {:016X} mod {:016X})",
            vertex_translation->shader().ucode_data_hash(),
            vertex_translation->modification(),
            pixel_translation ? pixel_translation->shader().ucode_data_hash()
                              : 0,
            pixel_translation ? pixel_translation->modification() : 0);
      }
      return true;
    }
    XELOGE("SPIRV-Cross: Failed to create pipeline state");
    return false;
  }

  // Request textures used by the shaders.
  uint32_t used_texture_mask =
      msl_vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (msl_pixel_shader) {
    used_texture_mask |= msl_pixel_shader->GetUsedTextureMaskAfterTranslation();
  }

  if (copy_resolve_writes_pending_ && used_texture_mask) {
    auto overlaps_resolved_texture_ranges = [&](uint32_t texture_fetch_mask) {
      uint32_t remaining_fetch_bits = texture_fetch_mask;
      uint32_t fetch_index = 0;
      while (xe::bit_scan_forward(remaining_fetch_bits, &fetch_index)) {
        remaining_fetch_bits &= ~(uint32_t(1) << fetch_index);
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(fetch_index);
        uint32_t width_minus_1 = 0;
        uint32_t height_minus_1 = 0;
        uint32_t depth_or_array_size_minus_1 = 0;
        uint32_t base_page = 0;
        uint32_t mip_page = 0;
        uint32_t mip_max_level = 0;
        texture_util::GetSubresourcesFromFetchConstant(
            fetch, &width_minus_1, &height_minus_1,
            &depth_or_array_size_minus_1, &base_page, &mip_page, nullptr,
            &mip_max_level);
        if (!base_page && !mip_page) {
          continue;
        }
        auto layout = texture_util::GetGuestTextureLayout(
            fetch.dimension, fetch.pitch, width_minus_1 + 1, height_minus_1 + 1,
            depth_or_array_size_minus_1 + 1, fetch.tiled, fetch.format,
            fetch.packed_mips, true, mip_max_level);
        const uint32_t base_size = layout.base.level_data_extent_bytes;
        const uint32_t mip_size = layout.mips_total_extent_bytes;
        if (base_page && base_size &&
            IsResolvedMemory(base_page << 12, base_size)) {
          return true;
        }
        if (mip_page && mip_size &&
            IsResolvedMemory(mip_page << 12, mip_size)) {
          return true;
        }
      }
      return false;
    };
    if (overlaps_resolved_texture_ranges(used_texture_mask)) {
      EndCommandBuffer();
      BeginCommandBuffer();
      if (!current_command_buffer_ || !current_render_encoder_) {
        XELOGE(
            "SPIRV-Cross: failed to re-begin command buffer for copy->draw "
            "sync split");
        return true;
      }
    }
  }

  if (texture_cache_ && used_texture_mask &&
      texture_cache_->AnyUsedTextureRequestWorkPending(used_texture_mask)) {
    texture_cache_->RequestTextures(used_texture_mask);
  }

  // Ensure shared-memory ranges used by translated shaders are synchronized.
  // The SPIRV-Cross path bypasses the MSC setup path, so it must request
  // vertex fetch and memexport ranges explicitly.
  if (shared_memory_) {
    const Shader::ConstantRegisterMap& constant_map_vertex =
        msl_vertex_shader->constant_register_map();
    for (uint32_t i = 0;
         i < xe::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining =
          constant_map_vertex.vertex_fetch_bitmap[i];
      uint32_t j;
      while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining &= ~(uint32_t(1) << j);
        uint32_t vfetch_index = i * 32 + j;
        xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(vfetch_index);
        switch (vfetch.type) {
          case xenos::FetchConstantType::kVertex:
            break;
          case xenos::FetchConstantType::kInvalidVertex:
            if (::cvars::gpu_allow_invalid_fetch_constants) {
              break;
            }
            XELOGW(
                "SPIRV-Cross: Vertex fetch constant {} ({:08X} {:08X}) has "
                "\"invalid\" type. Use --gpu_allow_invalid_fetch_constants to "
                "bypass.",
                vfetch_index, vfetch.dword_0, vfetch.dword_1);
            return false;
          default:
            XELOGW(
                "SPIRV-Cross: Vertex fetch constant {} ({:08X} {:08X}) is "
                "invalid.",
                vfetch_index, vfetch.dword_0, vfetch.dword_1);
            return false;
        }
        uint32_t buffer_offset = vfetch.address << 2;
        uint32_t buffer_length = vfetch.size << 2;
        if (buffer_offset > SharedMemory::kBufferSize ||
            SharedMemory::kBufferSize - buffer_offset < buffer_length) {
          XELOGW(
              "SPIRV-Cross: Vertex fetch constant {} out of range "
              "(offset=0x{:08X} size={})",
              vfetch_index, buffer_offset, buffer_length);
          return false;
        }
        if (!shared_memory_->RequestRange(buffer_offset, buffer_length)) {
          XELOGE(
              "SPIRV-Cross: Failed to request vertex buffer at 0x{:08X} "
              "(size {}) in shared memory",
              buffer_offset, buffer_length);
          return false;
        }
      }
    }

    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      uint32_t base_bytes = memexport_range.base_address_dwords << 2;
      if (!shared_memory_->RequestRange(base_bytes,
                                        memexport_range.size_bytes)) {
        XELOGE(
            "SPIRV-Cross: Failed to request memexport stream at 0x{:08X} "
            "(size {}) in shared memory",
            base_bytes, memexport_range.size_bytes);
        return false;
      }
    }
  }

  // Viewport info for system constants (same as the MSC path).
  draw_util::ViewportInfo viewport_info;
  constexpr uint32_t kViewportBoundsMax = 32767;
  bool convert_z_to_float24 = ::cvars::depth_float24_convert_in_pixel_shader;
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(
      draw_resolution_scale_x, draw_resolution_scale_y,
      texture_cache_ ? texture_cache_->draw_resolution_scale_x_divisor()
                     : divisors::MagicDiv(1),
      texture_cache_ ? texture_cache_->draw_resolution_scale_y_divisor()
                     : divisors::MagicDiv(1),
      true, kViewportBoundsMax, kViewportBoundsMax, false,
      normalized_depth_control, convert_z_to_float24, true,
      msl_pixel_shader && msl_pixel_shader->writes_depth());
  gviargs.SetupRegisterValues(regs);
  draw_util::GetHostViewportInfo(&gviargs, viewport_info);

  // Apply viewport and scissor (matching the MSC path).
  {
    draw_util::Scissor scissor;
    draw_util::GetScissor(regs, scissor);
    scissor.offset[0] *= draw_resolution_scale_x;
    scissor.offset[1] *= draw_resolution_scale_y;
    scissor.extent[0] *= draw_resolution_scale_x;
    scissor.extent[1] *= draw_resolution_scale_y;

    // Clamp scissor to actual render target bounds (Metal requires this).
    uint32_t rt_width = 1;
    uint32_t rt_height = 1;
    GetBoundRenderTargetSize(render_target_cache_.get(), render_target_width_,
                             render_target_height_, rt_width, rt_height);
    ClampScissorToBounds(scissor, rt_width, rt_height);

    MTL::Viewport mtl_viewport;
    mtl_viewport.originX = static_cast<double>(viewport_info.xy_offset[0]);
    mtl_viewport.originY = static_cast<double>(viewport_info.xy_offset[1]);
    mtl_viewport.width = static_cast<double>(viewport_info.xy_extent[0]);
    mtl_viewport.height = static_cast<double>(viewport_info.xy_extent[1]);
    mtl_viewport.znear = viewport_info.z_min;
    mtl_viewport.zfar = viewport_info.z_max;
    current_render_encoder_->setViewport(mtl_viewport);

    MTL::ScissorRect mtl_scissor;
    mtl_scissor.x = scissor.offset[0];
    mtl_scissor.y = scissor.offset[1];
    mtl_scissor.width = scissor.extent[0];
    mtl_scissor.height = scissor.extent[1];
    if (!msl_scissor_valid_ || msl_scissor_.x != mtl_scissor.x ||
        msl_scissor_.y != mtl_scissor.y ||
        msl_scissor_.width != mtl_scissor.width ||
        msl_scissor_.height != mtl_scissor.height) {
      current_render_encoder_->setScissorRect(mtl_scissor);
      msl_scissor_ = mtl_scissor;
      msl_scissor_valid_ = true;
    }
  }

  // Apply fixed-function state.
  if (msl_bound_pipeline_state_ != pipeline) {
    current_render_encoder_->setRenderPipelineState(pipeline);
    msl_bound_pipeline_state_ = pipeline;
  }
  ApplyRasterizerState(primitive_polygonal);
  ApplyDepthStencilState(primitive_polygonal, normalized_depth_control);

  // Update SPIRV system constants.
  UpdateSpirvSystemConstantValues(
      primitive_processing_result, primitive_polygonal,
      primitive_processing_result.line_loop_closing_index,
      primitive_processing_result.host_shader_index_endian, viewport_info,
      used_texture_mask, normalized_depth_control, normalized_color_mask);

  // Blend constants (fixed-function, same as MSC path).
  float blend_constants[] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  if (!ff_blend_factor_valid_ ||
      std::memcmp(ff_blend_factor_, blend_constants, sizeof(float) * 4) != 0) {
    std::memcpy(ff_blend_factor_, blend_constants, sizeof(float) * 4);
    ff_blend_factor_valid_ = true;
    current_render_encoder_->setBlendColor(
        blend_constants[0], blend_constants[1], blend_constants[2],
        blend_constants[3]);
  }

  // =====================================================================
  // Resource binding — direct Metal encoder calls, no IRDescriptorTable.
  // =====================================================================
  constexpr uint32_t kCBVSize = MslBufferIndex::kCbvSizeBytes;
  uint32_t ring_index = current_draw_index_ % uint32_t(draw_ring_count_);
  constexpr size_t kStageVertex = 0;
  constexpr size_t kStagePixel = 1;
  size_t table_index_vertex = size_t(ring_index) * kStageCount + kStageVertex;
  size_t table_index_pixel = size_t(ring_index) * kStageCount + kStagePixel;

  uint8_t* uniforms_base = static_cast<uint8_t*>(uniforms_buffer_->contents());
  uint8_t* uniforms_vertex =
      uniforms_base + table_index_vertex * kUniformsBytesPerTable;
  uint8_t* uniforms_pixel =
      uniforms_base + table_index_pixel * kUniformsBytesPerTable;
  if (msl_constants_versioned_uniform_buffer_ != uniforms_buffer_) {
    msl_constants_versioned_uniform_buffer_ = uniforms_buffer_;
    std::fill(msl_system_constants_written_vertex_versions_.begin(),
              msl_system_constants_written_vertex_versions_.end(), uint64_t(0));
    std::fill(msl_system_constants_written_pixel_versions_.begin(),
              msl_system_constants_written_pixel_versions_.end(), uint64_t(0));
    std::fill(msl_clip_plane_constants_written_vertex_versions_.begin(),
              msl_clip_plane_constants_written_vertex_versions_.end(),
              uint64_t(0));
    std::fill(msl_tessellation_constants_written_vertex_versions_.begin(),
              msl_tessellation_constants_written_vertex_versions_.end(),
              uint64_t(0));
    std::fill(msl_tessellation_constants_written_pixel_versions_.begin(),
              msl_tessellation_constants_written_pixel_versions_.end(),
              uint64_t(0));
  }
  auto ensure_uniform_versions_size = [&](std::vector<uint64_t>& versions) {
    if (versions.size() != draw_ring_count_) {
      versions.assign(draw_ring_count_, 0);
    }
  };
  ensure_uniform_versions_size(msl_system_constants_written_vertex_versions_);
  ensure_uniform_versions_size(msl_system_constants_written_pixel_versions_);
  ensure_uniform_versions_size(
      msl_clip_plane_constants_written_vertex_versions_);
  ensure_uniform_versions_size(
      msl_tessellation_constants_written_vertex_versions_);
  ensure_uniform_versions_size(
      msl_tessellation_constants_written_pixel_versions_);
  const size_t ring_index_size = size_t(ring_index);
  auto copy_uniform_block_if_stale =
      [&](uint8_t* dst, const void* src, size_t size,
          std::vector<uint64_t>& written_versions, uint64_t source_version) {
        if (ring_index_size >= written_versions.size()) {
          return;
        }
        if (written_versions[ring_index_size] != source_version) {
          std::memcpy(dst, src, size);
          written_versions[ring_index_size] = source_version;
        }
      };

  // b0 (msl_buffer 1): System constants.
  copy_uniform_block_if_stale(uniforms_vertex, &spirv_system_constants_,
                              sizeof(SpirvShaderTranslator::SystemConstants),
                              msl_system_constants_written_vertex_versions_,
                              msl_system_constants_version_);
  copy_uniform_block_if_stale(uniforms_pixel, &spirv_system_constants_,
                              sizeof(SpirvShaderTranslator::SystemConstants),
                              msl_system_constants_written_pixel_versions_,
                              msl_system_constants_version_);

  // b1 (msl_buffer 2/3): Float constants.
  // SpirvShaderTranslator uses packed float constants like Vulkan.
  const size_t kFloatConstantOffset = 1 * kCBVSize;
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      msl_vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < 4; ++i) {
    if (msl_current_float_constant_map_vertex_[i] !=
        float_constant_map_vertex.float_bitmap[i]) {
      msl_current_float_constant_map_vertex_[i] =
          float_constant_map_vertex.float_bitmap[i];
      msl_float_constants_dirty_vertex_ = true;
    }
  }
  if (msl_pixel_shader) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        msl_pixel_shader->constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      if (msl_current_float_constant_map_pixel_[i] !=
          float_constant_map_pixel.float_bitmap[i]) {
        msl_current_float_constant_map_pixel_[i] =
            float_constant_map_pixel.float_bitmap[i];
        msl_float_constants_dirty_pixel_ = true;
      }
    }
  } else {
    for (uint32_t i = 0; i < 4; ++i) {
      if (msl_current_float_constant_map_pixel_[i] != 0) {
        msl_current_float_constant_map_pixel_[i] = 0;
        msl_float_constants_dirty_pixel_ = true;
      }
    }
  }

  auto rebuild_packed_float_constants =
      [&](std::array<uint8_t, kCbvSizeBytes>& dst, const Shader* shader,
          uint32_t regs_base) {
        std::memset(dst.data(), 0, kCBVSize);
        if (!shader) {
          return;
        }
        const Shader::ConstantRegisterMap& map =
            shader->constant_register_map();
        if (!map.float_count) {
          return;
        }
        uint8_t* out = dst.data();
        for (uint32_t i = 0; i < 4; ++i) {
          uint64_t bits = map.float_bitmap[i];
          uint32_t constant_index;
          while (xe::bit_scan_forward(bits, &constant_index)) {
            bits &= ~(uint64_t(1) << constant_index);
            if (out + 4 * sizeof(uint32_t) > dst.data() + kCBVSize) {
              return;
            }
            std::memcpy(
                out, &regs.values[regs_base + (i << 8) + (constant_index << 2)],
                4 * sizeof(uint32_t));
            out += 4 * sizeof(uint32_t);
          }
        }
      };
  if (msl_float_constants_dirty_vertex_) {
    rebuild_packed_float_constants(msl_cached_float_constants_vertex_,
                                   msl_vertex_shader,
                                   XE_GPU_REG_SHADER_CONSTANT_000_X);
    msl_float_constants_dirty_vertex_ = false;
  }
  if (msl_float_constants_dirty_pixel_) {
    rebuild_packed_float_constants(msl_cached_float_constants_pixel_,
                                   msl_pixel_shader,
                                   XE_GPU_REG_SHADER_CONSTANT_256_X);
    msl_float_constants_dirty_pixel_ = false;
  }
  std::memcpy(uniforms_vertex + kFloatConstantOffset,
              msl_cached_float_constants_vertex_.data(), kCBVSize);
  std::memcpy(uniforms_pixel + kFloatConstantOffset,
              msl_cached_float_constants_pixel_.data(), kCBVSize);

  // b2 (msl_buffer 4): Bool/loop constants.
  const size_t kBoolLoopConstantOffset = 2 * kCBVSize;
  constexpr size_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
  if (msl_bool_loop_constants_dirty_) {
    std::memcpy(msl_cached_bool_loop_constants_.data(),
                &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    msl_bool_loop_constants_dirty_ = false;
  }
  std::memcpy(uniforms_vertex + kBoolLoopConstantOffset,
              msl_cached_bool_loop_constants_.data(), kBoolLoopConstantsSize);
  std::memcpy(uniforms_pixel + kBoolLoopConstantOffset,
              msl_cached_bool_loop_constants_.data(), kBoolLoopConstantsSize);

  // b3 (msl_buffer 5): Fetch constants.
  const size_t kFetchConstantOffset = 3 * kCBVSize;
  const size_t kFetchConstantCount = xenos::kTextureFetchConstantCount * 6;
  const size_t kFetchConstantsSize = kFetchConstantCount * sizeof(uint32_t);
  if (msl_fetch_constants_dirty_) {
    std::memcpy(msl_cached_fetch_constants_.data(),
                &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                kFetchConstantsSize);
    msl_fetch_constants_dirty_ = false;
  }
  std::memcpy(uniforms_vertex + kFetchConstantOffset,
              msl_cached_fetch_constants_.data(), kFetchConstantsSize);
  std::memcpy(uniforms_pixel + kFetchConstantOffset,
              msl_cached_fetch_constants_.data(), kFetchConstantsSize);

  // b5 (msl_buffer 6): Clip plane constants (separate buffer for SPIR-V path).
  const size_t kClipPlaneConstantOffset = 4 * kCBVSize;
  copy_uniform_block_if_stale(uniforms_vertex + kClipPlaneConstantOffset,
                              &spirv_clip_plane_constants_,
                              sizeof(SpirvShaderTranslator::ClipPlaneConstants),
                              msl_clip_plane_constants_written_vertex_versions_,
                              msl_clip_plane_constants_version_);

  // b6 (msl_buffer 7): Tessellation constants.
  const size_t kTessellationConstantOffset = 5 * kCBVSize;
  copy_uniform_block_if_stale(
      uniforms_vertex + kTessellationConstantOffset,
      &spirv_tessellation_constants_,
      sizeof(SpirvShaderTranslator::TessellationConstants),
      msl_tessellation_constants_written_vertex_versions_,
      msl_tessellation_constants_version_);
  copy_uniform_block_if_stale(
      uniforms_pixel + kTessellationConstantOffset,
      &spirv_tessellation_constants_,
      sizeof(SpirvShaderTranslator::TessellationConstants),
      msl_tessellation_constants_written_pixel_versions_,
      msl_tessellation_constants_version_);

  // Keep binding behavior conservative while using per-encoder dedupe caches.
  const bool msl_bind_dedupe = true;

  // Bind shared memory buffer at msl_buffer 0.
  MTL::Buffer* shared_mem_buffer =
      shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
  MTL::ResourceUsage shared_memory_usage = MTL::ResourceUsageRead;
  if (memexport_used) {
    shared_memory_usage |= MTL::ResourceUsageWrite;
  }
  if (!msl_bind_dedupe ||
      msl_bound_shared_memory_buffer_ != shared_mem_buffer) {
    current_render_encoder_->setVertexBuffer(shared_mem_buffer, 0,
                                             MslBufferIndex::kSharedMemory);
    current_render_encoder_->setFragmentBuffer(shared_mem_buffer, 0,
                                               MslBufferIndex::kSharedMemory);
    msl_bound_shared_memory_buffer_ = shared_mem_buffer;
  }
  if (shared_mem_buffer) {
    UseRenderEncoderResource(shared_mem_buffer, shared_memory_usage);
  }

  // Bind a null buffer at the EDRAM slot (msl_buffer 30) as a safety measure.
  // FSI/EDRAM is disabled on this path (fragment_shader_sample_interlock =
  // false), so no shader should reference it, but binding a dummy prevents
  // GPU faults if any code path unexpectedly accesses buffer(30).
  if (!msl_bind_dedupe || msl_bound_null_buffer_ != null_buffer_) {
    current_render_encoder_->setFragmentBuffer(null_buffer_, 0, 30);
    msl_bound_null_buffer_ = null_buffer_;
  }

  // Bind uniforms buffer at the appropriate indices.
  NS::UInteger vs_base_offset = table_index_vertex * kUniformsBytesPerTable;
  NS::UInteger ps_base_offset = table_index_pixel * kUniformsBytesPerTable;
  if (!msl_bind_dedupe || !msl_bound_uniforms_offsets_valid_ ||
      msl_bound_uniforms_buffer_ != uniforms_buffer_ ||
      msl_bound_uniforms_vs_base_offset_ != vs_base_offset ||
      msl_bound_uniforms_ps_base_offset_ != ps_base_offset) {
    // System constants (msl_buffer 1).
    current_render_encoder_->setVertexBuffer(uniforms_buffer_,
                                             vs_base_offset + 0 * kCBVSize,
                                             MslBufferIndex::kSystemConstants);
    current_render_encoder_->setFragmentBuffer(
        uniforms_buffer_, ps_base_offset + 0 * kCBVSize,
        MslBufferIndex::kSystemConstants);

    // Float constants vertex (msl_buffer 2).
    current_render_encoder_->setVertexBuffer(
        uniforms_buffer_, vs_base_offset + 1 * kCBVSize,
        MslBufferIndex::kFloatConstantsVertex);
    // Float constants pixel (msl_buffer 3).
    current_render_encoder_->setFragmentBuffer(
        uniforms_buffer_, ps_base_offset + 1 * kCBVSize,
        MslBufferIndex::kFloatConstantsPixel);

    // Bool/loop constants (msl_buffer 4).
    current_render_encoder_->setVertexBuffer(
        uniforms_buffer_, vs_base_offset + 2 * kCBVSize,
        MslBufferIndex::kBoolLoopConstants);
    current_render_encoder_->setFragmentBuffer(
        uniforms_buffer_, ps_base_offset + 2 * kCBVSize,
        MslBufferIndex::kBoolLoopConstants);

    // Fetch constants (msl_buffer 5).
    current_render_encoder_->setVertexBuffer(uniforms_buffer_,
                                             vs_base_offset + 3 * kCBVSize,
                                             MslBufferIndex::kFetchConstants);
    current_render_encoder_->setFragmentBuffer(uniforms_buffer_,
                                               ps_base_offset + 3 * kCBVSize,
                                               MslBufferIndex::kFetchConstants);

    // Clip plane constants (msl_buffer 6) — vertex shader only.
    // The SPIR-V translator uses a separate constant buffer for clip planes.
    current_render_encoder_->setVertexBuffer(
        uniforms_buffer_, vs_base_offset + 4 * kCBVSize,
        MslBufferIndex::kClipPlaneConstants);

    // Tessellation constants (msl_buffer 7).
    current_render_encoder_->setVertexBuffer(
        uniforms_buffer_, vs_base_offset + 5 * kCBVSize,
        MslBufferIndex::kTessellationConstants);
    current_render_encoder_->setFragmentBuffer(
        uniforms_buffer_, ps_base_offset + 5 * kCBVSize,
        MslBufferIndex::kTessellationConstants);

    msl_bound_uniforms_buffer_ = uniforms_buffer_;
    msl_bound_uniforms_vs_base_offset_ = vs_base_offset;
    msl_bound_uniforms_ps_base_offset_ = ps_base_offset;
    msl_bound_uniforms_offsets_valid_ = true;
  }

  UseRenderEncoderResource(uniforms_buffer_, MTL::ResourceUsageRead);

  const bool vertex_uses_argbuf =
      vertex_translation && vertex_translation->uses_argument_buffers();
  const bool pixel_uses_argbuf =
      pixel_translation && pixel_translation->uses_argument_buffers();
  auto get_msl_binding_layout_uid =
      [](const MslShader::MslTranslation* translation) -> uint64_t {
    if (!translation) {
      return 0;
    }
    const auto& texture_binding_indices =
        translation->texture_binding_indices_for_msl_slots();
    const auto& sampler_binding_indices =
        translation->sampler_binding_indices_for_msl_slots();
    uint64_t uid = XXH3_64bits(
        texture_binding_indices.data(),
        texture_binding_indices.size() * sizeof(texture_binding_indices[0]));
    uid = XXH3_64bits_withSeed(
        sampler_binding_indices.data(),
        sampler_binding_indices.size() * sizeof(sampler_binding_indices[0]),
        uid);
    return uid;
  };

  auto bind_msl_argument_buffer = [&](MslShader* shader,
                                      MslShader::MslTranslation* translation,
                                      bool is_pixel_stage) -> bool {
    if (!shader || !translation || !translation->uses_argument_buffers() ||
        !texture_cache_) {
      return true;
    }

    MTL::ArgumentEncoder* arg_encoder = translation->argument_encoder();
    uint32_t encoded_length = translation->argument_encoder_encoded_length();
    if (!arg_encoder || encoded_length == 0) {
      return true;
    }

    const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
    const auto& texture_binding_indices =
        translation->texture_binding_indices_for_msl_slots();
    uint32_t texture_count = std::min(uint32_t(texture_binding_indices.size()),
                                      MslTextureIndex::kMaxPerStage);
    std::array<const MTL::Texture*, MslTextureIndex::kMaxPerStage> textures =
        {};

    MetalTextureCache* metal_texture_cache = texture_cache_.get();
    for (uint32_t slot = 0; slot < texture_count; ++slot) {
      MTL::Texture* texture = nullptr;
      int32_t texture_binding_index = texture_binding_indices[slot];
      if (texture_binding_index >= 0 &&
          size_t(texture_binding_index) < texture_bindings.size()) {
        const auto& binding = texture_bindings[size_t(texture_binding_index)];
        texture = texture_cache_->GetTextureForBinding(
            binding.fetch_constant, binding.dimension, binding.is_signed);
        if (!texture) {
          switch (binding.dimension) {
            case xenos::FetchOpDimension::k3DOrStacked:
              texture = metal_texture_cache->GetNullTexture3D();
              break;
            case xenos::FetchOpDimension::kCube:
              texture = metal_texture_cache->GetNullTextureCube();
              break;
            default:
              texture = metal_texture_cache->GetNullTexture2D();
              break;
          }
        }
      } else {
        texture = metal_texture_cache->GetNullTexture2D();
      }
      textures[slot] = texture;
      // UseRenderEncoderResource must always be called for hazard tracking,
      // even when we skip re-encoding. The dedup map handles per-encoder
      // deduplication.
      if (texture) {
        UseRenderEncoderResource(texture, MTL::ResourceUsageRead);
      }
    }

    const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
    const auto& sampler_binding_indices =
        translation->sampler_binding_indices_for_msl_slots();
    uint32_t sampler_count = std::min(uint32_t(sampler_binding_indices.size()),
                                      MslSamplerIndex::kMaxPerStage);
    std::array<const MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
        samplers = {};
    for (uint32_t smp_index = 0; smp_index < sampler_count; ++smp_index) {
      MTL::SamplerState* sampler_state = null_sampler_;
      uint32_t sampler_binding_index = sampler_binding_indices[smp_index];
      if (sampler_binding_index < sampler_bindings.size()) {
        auto parameters = texture_cache_->GetSamplerParameters(
            sampler_bindings[sampler_binding_index]);
        sampler_state = texture_cache_->GetOrCreateSampler(parameters);
        if (!sampler_state) {
          sampler_state = null_sampler_;
        }
      }
      samplers[smp_index] = sampler_state;
    }

    // Check if textures and samplers match the cached content from the last
    // encoding. If so, skip the expensive acquire+encode and reuse the previous
    // argument buffer slice.
    auto& cached_textures = is_pixel_stage ? msl_last_argbuf_pixel_textures_
                                           : msl_last_argbuf_vertex_textures_;
    auto& cached_texture_count = is_pixel_stage
                                     ? msl_last_argbuf_pixel_texture_count_
                                     : msl_last_argbuf_vertex_texture_count_;
    auto& cached_samplers = is_pixel_stage ? msl_last_argbuf_pixel_samplers_
                                           : msl_last_argbuf_vertex_samplers_;
    auto& cached_sampler_count = is_pixel_stage
                                     ? msl_last_argbuf_pixel_sampler_count_
                                     : msl_last_argbuf_vertex_sampler_count_;
    auto& cached_argbuf_buffer = is_pixel_stage
                                     ? msl_last_argbuf_pixel_buffer_
                                     : msl_last_argbuf_vertex_buffer_;
    auto& cached_argbuf_offset = is_pixel_stage
                                     ? msl_last_argbuf_pixel_offset_
                                     : msl_last_argbuf_vertex_offset_;
    auto& cached_translation = is_pixel_stage
                                   ? msl_last_argbuf_pixel_translation_
                                   : msl_last_argbuf_vertex_translation_;
    auto& cached_encoded_length = is_pixel_stage
                                      ? msl_last_argbuf_pixel_encoded_length_
                                      : msl_last_argbuf_vertex_encoded_length_;
    auto& cached_layout_uid = is_pixel_stage
                                  ? msl_last_argbuf_pixel_layout_uid_
                                  : msl_last_argbuf_vertex_layout_uid_;
    const uint64_t layout_uid = get_msl_binding_layout_uid(translation);

    bool content_changed = cached_translation != translation ||
                           cached_encoded_length != encoded_length ||
                           layout_uid != cached_layout_uid ||
                           texture_count != cached_texture_count ||
                           sampler_count != cached_sampler_count ||
                           !cached_argbuf_buffer;
    if (!content_changed && texture_count > 0) {
      content_changed = std::memcmp(textures.data(), cached_textures.data(),
                                    texture_count * sizeof(textures[0])) != 0;
    }
    if (!content_changed && sampler_count > 0) {
      content_changed = std::memcmp(samplers.data(), cached_samplers.data(),
                                    sampler_count * sizeof(samplers[0])) != 0;
    }

    MTL::Buffer* argbuf_buffer;
    NS::UInteger argbuf_offset;

    if (content_changed) {
      // Content changed — acquire a new slice and re-encode.
      argbuf_buffer = nullptr;
      argbuf_offset = 0;
      if (!AcquireSpirvArgumentBufferSlice(
              encoded_length, translation->argument_encoder_alignment(),
              &argbuf_buffer, &argbuf_offset)) {
        XELOGE(
            "SPIRV-Cross: Failed to allocate argument buffer slice ({} bytes)",
            encoded_length);
        return false;
      }

      arg_encoder->setArgumentBuffer(argbuf_buffer, argbuf_offset);
      if (texture_count) {
        arg_encoder->setTextures(
            textures.data(),
            NS::Range::Make(MslTextureIndex::kBase, texture_count));
      }
      if (sampler_count) {
        arg_encoder->setSamplerStates(
            samplers.data(),
            NS::Range::Make(MslArgumentBufferId::kSamplerBase, sampler_count));
      }

      // Update the cache.
      std::memcpy(cached_textures.data(), textures.data(),
                  texture_count * sizeof(textures[0]));
      if (texture_count < cached_texture_count) {
        std::memset(&cached_textures[texture_count], 0,
                    (cached_texture_count - texture_count) *
                        sizeof(cached_textures[0]));
      }
      cached_texture_count = texture_count;
      std::memcpy(cached_samplers.data(), samplers.data(),
                  sampler_count * sizeof(samplers[0]));
      if (sampler_count < cached_sampler_count) {
        std::memset(&cached_samplers[sampler_count], 0,
                    (cached_sampler_count - sampler_count) *
                        sizeof(cached_samplers[0]));
      }
      cached_sampler_count = sampler_count;
      cached_argbuf_buffer = argbuf_buffer;
      cached_argbuf_offset = argbuf_offset;
      cached_translation = translation;
      cached_encoded_length = encoded_length;
      cached_layout_uid = layout_uid;
    } else {
      // Content unchanged — reuse the previous argument buffer slice.
      argbuf_buffer = cached_argbuf_buffer;
      argbuf_offset = cached_argbuf_offset;
    }

    if (is_pixel_stage) {
      if (argbuf_buffer != msl_bound_pixel_argument_buffer_) {
        current_render_encoder_->setFragmentBuffer(
            argbuf_buffer, 0, MslBufferIndex::kArgumentBufferTexturesSamplers);
        msl_bound_pixel_argument_buffer_ = argbuf_buffer;
        msl_bound_pixel_argument_buffer_offset_valid_ = false;
      }
      if (!msl_bound_pixel_argument_buffer_offset_valid_ ||
          msl_bound_pixel_argument_buffer_offset_ != argbuf_offset) {
        current_render_encoder_->setFragmentBufferOffset(
            argbuf_offset, MslBufferIndex::kArgumentBufferTexturesSamplers);
        msl_bound_pixel_argument_buffer_offset_ = argbuf_offset;
        msl_bound_pixel_argument_buffer_offset_valid_ = true;
      }
    } else {
      if (argbuf_buffer != msl_bound_vertex_argument_buffer_) {
        current_render_encoder_->setVertexBuffer(
            argbuf_buffer, 0, MslBufferIndex::kArgumentBufferTexturesSamplers);
        msl_bound_vertex_argument_buffer_ = argbuf_buffer;
        msl_bound_vertex_argument_buffer_offset_valid_ = false;
      }
      if (!msl_bound_vertex_argument_buffer_offset_valid_ ||
          msl_bound_vertex_argument_buffer_offset_ != argbuf_offset) {
        current_render_encoder_->setVertexBufferOffset(
            argbuf_offset, MslBufferIndex::kArgumentBufferTexturesSamplers);
        msl_bound_vertex_argument_buffer_offset_ = argbuf_offset;
        msl_bound_vertex_argument_buffer_offset_valid_ = true;
      }
    }

    UseRenderEncoderResource(argbuf_buffer, MTL::ResourceUsageRead);
    return true;
  };
  if (vertex_uses_argbuf &&
      !bind_msl_argument_buffer(msl_vertex_shader, vertex_translation, false)) {
    return false;
  }
  if (pixel_uses_argbuf &&
      !bind_msl_argument_buffer(msl_pixel_shader, pixel_translation, true)) {
    return false;
  }

  // Bind textures and samplers directly.
  auto bind_msl_textures = [&](MslShader* shader,
                               MslShader::MslTranslation* translation,
                               bool is_pixel_stage) {
    auto bind_texture_slot = [&](uint32_t slot, MTL::Texture* texture) {
      if (is_pixel_stage) {
        current_render_encoder_->setFragmentTexture(texture, slot);
      } else {
        current_render_encoder_->setVertexTexture(texture, slot);
      }
    };
    uint32_t* previous_bound_count = is_pixel_stage
                                         ? &msl_bound_pixel_texture_count_
                                         : &msl_bound_vertex_texture_count_;
    uint64_t* cached_binding_uid = is_pixel_stage
                                       ? &msl_bound_pixel_texture_binding_uid_
                                       : &msl_bound_vertex_texture_binding_uid_;
    auto* bound_textures = is_pixel_stage ? &msl_bound_pixel_textures_
                                          : &msl_bound_vertex_textures_;
    const uint64_t layout_uid = get_msl_binding_layout_uid(translation);
    const uint64_t binding_uid =
        (shader && translation && texture_cache_)
            ? (uint64_t(reinterpret_cast<uintptr_t>(pipeline)) *
                   UINT64_C(11400714819323198485) ^
               layout_uid)
            : 0;
    const bool force_rebind = *cached_binding_uid != binding_uid;
    auto clear_slots_from = [&](uint32_t start, uint32_t end_exclusive) {
      for (uint32_t slot = start; slot < end_exclusive; ++slot) {
        if (force_rebind || (*bound_textures)[slot] != nullptr) {
          bind_texture_slot(slot, nullptr);
          (*bound_textures)[slot] = nullptr;
        }
      }
    };

    if (!shader || !translation || !texture_cache_) {
      clear_slots_from(0, *previous_bound_count);
      *previous_bound_count = 0;
      *cached_binding_uid = binding_uid;
      return;
    }

    const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
    const auto& texture_binding_indices =
        translation->texture_binding_indices_for_msl_slots();
    uint32_t bound_count = std::min(uint32_t(texture_binding_indices.size()),
                                    MslTextureIndex::kMaxPerStage);
    if (*previous_bound_count > bound_count) {
      clear_slots_from(bound_count, *previous_bound_count);
    }

    MetalTextureCache* metal_texture_cache = texture_cache_.get();
    for (uint32_t slot = 0; slot < bound_count; ++slot) {
      uint32_t tex_index = MslTextureIndex::kBase + slot;
      MTL::Texture* texture = nullptr;
      int32_t texture_binding_index = texture_binding_indices[slot];
      if (texture_binding_index >= 0 &&
          size_t(texture_binding_index) < texture_bindings.size()) {
        const auto& binding = texture_bindings[size_t(texture_binding_index)];
        texture = texture_cache_->GetTextureForBinding(
            binding.fetch_constant, binding.dimension, binding.is_signed);
        if (!texture) {
          switch (binding.dimension) {
            case xenos::FetchOpDimension::k3DOrStacked:
              texture = metal_texture_cache->GetNullTexture3D();
              break;
            case xenos::FetchOpDimension::kCube:
              texture = metal_texture_cache->GetNullTextureCube();
              break;
            default:
              texture = metal_texture_cache->GetNullTexture2D();
              break;
          }
        }
      } else {
        texture = metal_texture_cache->GetNullTexture2D();
      }
      if (force_rebind || (*bound_textures)[tex_index] != texture) {
        bind_texture_slot(tex_index, texture);
        (*bound_textures)[tex_index] = texture;
      }
      if (texture) {
        UseRenderEncoderResource(texture, MTL::ResourceUsageRead);
      }
    }
    *previous_bound_count = bound_count;
    *cached_binding_uid = binding_uid;
  };

  auto bind_msl_samplers = [&](MslShader* shader,
                               MslShader::MslTranslation* translation,
                               bool is_pixel_stage) {
    auto bind_sampler_slot = [&](uint32_t slot, MTL::SamplerState* sampler) {
      if (is_pixel_stage) {
        current_render_encoder_->setFragmentSamplerState(sampler, slot);
      } else {
        current_render_encoder_->setVertexSamplerState(sampler, slot);
      }
    };

    uint32_t* previous_bound_count = is_pixel_stage
                                         ? &msl_bound_pixel_sampler_count_
                                         : &msl_bound_vertex_sampler_count_;
    uint64_t* cached_binding_uid = is_pixel_stage
                                       ? &msl_bound_pixel_sampler_binding_uid_
                                       : &msl_bound_vertex_sampler_binding_uid_;
    auto* bound_samplers = is_pixel_stage ? &msl_bound_pixel_samplers_
                                          : &msl_bound_vertex_samplers_;
    const uint64_t layout_uid = get_msl_binding_layout_uid(translation);
    const uint64_t binding_uid =
        (shader && translation && texture_cache_)
            ? (uint64_t(reinterpret_cast<uintptr_t>(pipeline)) *
                   UINT64_C(11400714819323198485) ^
               layout_uid)
            : 0;
    const bool force_rebind = *cached_binding_uid != binding_uid;

    if (!shader || !translation || !texture_cache_) {
      for (uint32_t slot = 0; slot < *previous_bound_count; ++slot) {
        if (force_rebind || (*bound_samplers)[slot] != null_sampler_) {
          bind_sampler_slot(slot, null_sampler_);
          (*bound_samplers)[slot] = null_sampler_;
        }
      }
      *previous_bound_count = 0;
      *cached_binding_uid = binding_uid;
      return;
    }
    // Samplers are remapped to compact Metal indices 0..M-1 in
    // MslShader::AddResourceBindings. Use the reflected SPIR-V remap order
    // captured in the translation, not the raw translator array order, because
    // SPIRV-Cross may drop/reorder separate samplers.
    const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
    const auto& sampler_binding_indices =
        translation->sampler_binding_indices_for_msl_slots();
    uint32_t bound_count = std::min(uint32_t(sampler_binding_indices.size()),
                                    MslSamplerIndex::kMaxPerStage);
    if (*previous_bound_count > bound_count) {
      for (uint32_t slot = bound_count; slot < *previous_bound_count; ++slot) {
        if (force_rebind || (*bound_samplers)[slot] != null_sampler_) {
          bind_sampler_slot(slot, null_sampler_);
          (*bound_samplers)[slot] = null_sampler_;
        }
      }
    }

    for (uint32_t smp_index = 0; smp_index < bound_count; ++smp_index) {
      MTL::SamplerState* sampler_state = null_sampler_;
      uint32_t sampler_binding_index = sampler_binding_indices[smp_index];
      if (sampler_binding_index < sampler_bindings.size()) {
        auto parameters = texture_cache_->GetSamplerParameters(
            sampler_bindings[sampler_binding_index]);
        sampler_state = texture_cache_->GetOrCreateSampler(parameters);
        if (!sampler_state) {
          sampler_state = null_sampler_;
        }
      }
      if (force_rebind || (*bound_samplers)[smp_index] != sampler_state) {
        bind_sampler_slot(smp_index, sampler_state);
        (*bound_samplers)[smp_index] = sampler_state;
      }
    }
    *previous_bound_count = bound_count;
    *cached_binding_uid = binding_uid;
  };

  if (!vertex_uses_argbuf) {
    bind_msl_textures(msl_vertex_shader, vertex_translation, false);
    bind_msl_samplers(msl_vertex_shader, vertex_translation, false);
  }
  if (!pixel_uses_argbuf) {
    bind_msl_textures(msl_pixel_shader, pixel_translation, true);
    bind_msl_samplers(msl_pixel_shader, pixel_translation, true);
  }

  // =====================================================================
  // Draw dispatch — native Metal encoder calls (no IRRuntime).
  // =====================================================================
  if (is_tessellated) {
    // ---------------------------------------------------------------
    // Tessellated draw: fill tessellation factor buffer, then drawPatches.
    // ---------------------------------------------------------------
    // Xenos tess levels are 0-based; add 1 to match other backends.
    float max_tess = std::max(
        1.0f, regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f);

    // Determine control points per patch and patch count from the draw.
    // The primitive processor passes patch count in host_draw_vertex_count
    // for tessellated draws (vertex count = cp_per_patch * patch_count).
    uint32_t cp_per_patch = 1;
    bool is_quad_domain = false;
    switch (host_vertex_shader_type) {
      case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
        cp_per_patch = 3;
        break;
      case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
        cp_per_patch = 4;
        is_quad_domain = true;
        break;
      case Shader::HostVertexShaderType::kLineDomainCPIndexed:
      case Shader::HostVertexShaderType::kLineDomainPatchIndexed:
        cp_per_patch = 2;
        break;
      default:
        break;
    }
    uint32_t vertex_count = primitive_processing_result.host_draw_vertex_count;
    uint32_t patch_count = cp_per_patch > 0 ? vertex_count / cp_per_patch : 0;
    if (patch_count == 0) {
      return true;  // Nothing to draw.
    }

    // Ensure tessellation factor buffer is large enough.
    if (!EnsureTessFactorBuffer(patch_count)) {
      XELOGE(
          "SPIRV-Cross: Failed to allocate tess factor buffer for {} "
          "patches",
          patch_count);
      return false;
    }

    // IEEE 754 float32 → float16 conversion for tessellation factors.
    // Uses round-to-nearest-even and handles subnormals for accuracy
    // near tessellation factor boundaries.
    auto f32_to_f16 = [](float v) -> uint16_t {
      uint32_t b;
      std::memcpy(&b, &v, 4);
      uint16_t s = (b >> 16) & 0x8000u;
      int e = int((b >> 23) & 0xFFu) - 127 + 15;
      uint32_t m = b & 0x7FFFFFu;
      if (e <= 0) {
        // Subnormal or zero in half precision.
        if (e < -10) {
          return s;  // Too small, flush to signed zero.
        }
        // Subnormal half: shift mantissa (with implicit leading 1) right.
        m = (m | 0x800000u) >> (1 - e);
        // Round to nearest even.
        if ((m & 0x1FFFu) > 0x1000u ||
            ((m & 0x1FFFu) == 0x1000u && (m & 0x2000u))) {
          m += 0x2000u;
        }
        return uint16_t(s | (m >> 13));
      }
      if (e >= 31) {
        // Overflow → infinity (or NaN passthrough).
        if (e == 31 && m != 0) {
          // NaN: preserve at least one mantissa bit.
          return uint16_t(s | 0x7C00u | std::max(m >> 13, uint32_t(1)));
        }
        return uint16_t(s | 0x7C00u);
      }
      // Round to nearest even: check the 13 bits being truncated.
      if ((m & 0x1FFFu) > 0x1000u ||
          ((m & 0x1FFFu) == 0x1000u && (m & 0x2000u))) {
        m += 0x2000u;
        if (m & 0x800000u) {
          m = 0;
          e++;
          if (e >= 31) {
            return uint16_t(s | 0x7C00u);
          }
        }
      }
      return uint16_t(s | (e << 10) | (m >> 13));
    };

    // Determine tessellation mode from the register file.
    auto tess_mode = regs.Get<reg::VGT_HOS_CNTL>().tess_mode;

    uint8_t* factor_data =
        static_cast<uint8_t*>(tess_factor_buffer_->contents());

    if (tess_mode == xenos::TessellationMode::kAdaptive && shared_memory_) {
      // ------------------------------------------------------------------
      // Adaptive tessellation: per-edge factors from shared memory.
      // The guest "index buffer" is repurposed as a factor buffer
      // containing big-endian float32 edge factors.
      // ------------------------------------------------------------------
      xenos::Endian index_endian =
          primitive_processing_result.host_shader_index_endian;
      uint32_t factor_base = primitive_processing_result.guest_index_base;
      const uint8_t* xbox_ram = shared_memory_->GetXboxRamBase();
      // Minimum factor from VGT_HOS_MIN_TESS_LEVEL + 1.0 (Xbox 360
      // convention). For fractional_even partitioning, must be >= 2.0.
      float factor_min = std::max(
          2.0f, regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f);
      float factor_max = max_tess;

      if (is_quad_domain) {
        // Quad: 4 edge factors per patch.
        struct QuadFactors {
          uint16_t edge[4];
          uint16_t inside[2];
        };
        static_assert(sizeof(QuadFactors) == 12);
        auto* factors = reinterpret_cast<QuadFactors*>(factor_data);
        for (uint32_t i = 0; i < patch_count; ++i) {
          // Read 4 edge factors from guest memory.
          float ef[4];
          for (uint32_t j = 0; j < 4; ++j) {
            uint32_t addr = factor_base + (i * 4 + j) * sizeof(float);
            float raw;
            std::memcpy(&raw, xbox_ram + addr, sizeof(float));
            // Endian-swap per the index endian mode.
            raw = xenos::GpuSwap(raw, index_endian);
            // Add 1.0 per Xbox 360 convention, clamp.
            ef[j] = std::clamp(raw + 1.0f, factor_min, factor_max);
          }
          // Map Xbox 360 edge order to Metal:
          //   edge[i] = input[(i+3) & 3]
          //   (from adaptive_quad.hs.glsl)
          factors[i].edge[0] = f32_to_f16(ef[3]);
          factors[i].edge[1] = f32_to_f16(ef[0]);
          factors[i].edge[2] = f32_to_f16(ef[1]);
          factors[i].edge[3] = f32_to_f16(ef[2]);
          // Inside factors: minimum of opposing edges.
          // inside[0] along U = min(mapped_edge[1], mapped_edge[3])
          // inside[1] along V = min(mapped_edge[0], mapped_edge[2])
          factors[i].inside[0] = f32_to_f16(std::min(ef[0], ef[2]));
          factors[i].inside[1] = f32_to_f16(std::min(ef[3], ef[1]));
        }
      } else {
        // Triangle: 3 edge factors per patch.
        struct TriFactors {
          uint16_t edge[3];
          uint16_t inside;
        };
        static_assert(sizeof(TriFactors) == 8);
        auto* factors = reinterpret_cast<TriFactors*>(factor_data);
        for (uint32_t i = 0; i < patch_count; ++i) {
          // Read 3 edge factors from guest memory.
          float ef[3];
          for (uint32_t j = 0; j < 3; ++j) {
            uint32_t addr = factor_base + (i * 3 + j) * sizeof(float);
            float raw;
            std::memcpy(&raw, xbox_ram + addr, sizeof(float));
            raw = xenos::GpuSwap(raw, index_endian);
            ef[j] = std::clamp(raw + 1.0f, factor_min, factor_max);
          }
          // Map Xbox 360 edge order to Metal:
          //   Metal edge[0] = U0 (v1->v2) = ef[1]
          //   Metal edge[1] = V0 (v2->v0) = ef[2]
          //   Metal edge[2] = W0 (v0->v1) = ef[0]
          //   (from adaptive_triangle.hs.glsl)
          factors[i].edge[0] = f32_to_f16(ef[1]);
          factors[i].edge[1] = f32_to_f16(ef[2]);
          factors[i].edge[2] = f32_to_f16(ef[0]);
          // Inside factor = minimum of all edge factors.
          factors[i].inside =
              f32_to_f16(std::min(std::min(ef[0], ef[1]), ef[2]));
        }
      }
    } else {
      // ------------------------------------------------------------------
      // Uniform tessellation (discrete / continuous modes).
      // All patches get the same factor from VGT_HOS_MAX_TESS_LEVEL.
      // ------------------------------------------------------------------
      uint16_t ef = f32_to_f16(max_tess);
      if (is_quad_domain) {
        struct QuadFactors {
          uint16_t edge[4];
          uint16_t inside[2];
        };
        static_assert(sizeof(QuadFactors) == 12);
        auto* factors = reinterpret_cast<QuadFactors*>(factor_data);
        for (uint32_t i = 0; i < patch_count; ++i) {
          factors[i].edge[0] = ef;
          factors[i].edge[1] = ef;
          factors[i].edge[2] = ef;
          factors[i].edge[3] = ef;
          factors[i].inside[0] = ef;
          factors[i].inside[1] = ef;
        }
      } else {
        struct TriFactors {
          uint16_t edge[3];
          uint16_t inside;
        };
        static_assert(sizeof(TriFactors) == 8);
        auto* factors = reinterpret_cast<TriFactors*>(factor_data);
        for (uint32_t i = 0; i < patch_count; ++i) {
          factors[i].edge[0] = ef;
          factors[i].edge[1] = ef;
          factors[i].edge[2] = ef;
          factors[i].inside = ef;
        }
      }
    }

    // Draw with tessellation.
    assert_not_null(tess_factor_buffer_);
    UseRenderEncoderResource(tess_factor_buffer_, MTL::ResourceUsageRead);
    current_render_encoder_->setTessellationFactorBuffer(tess_factor_buffer_, 0,
                                                         0);
    // drawPatches signature:
    //   numberOfPatchControlPoints, patchStart, patchCount,
    //   patchIndexBuffer, patchIndexBufferOffset,
    //   instanceCount, baseInstance
    // patchIndexBuffer = nullptr since patches are not indexed (the domain
    // shader reads control points from shared memory via vertex ID).
    current_render_encoder_->drawPatches(
        NS::UInteger(cp_per_patch), NS::UInteger(0), NS::UInteger(patch_count),
        nullptr,           // patchIndexBuffer (non-indexed patches)
        0,                 // patchIndexBufferOffset
        NS::UInteger(1),   // instanceCount
        NS::UInteger(0));  // baseInstance
  } else {
    // ---------------------------------------------------------------
    // Non-tessellated draw: standard primitives.
    // ---------------------------------------------------------------
    MTL::PrimitiveType mtl_primitive = MTL::PrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        mtl_primitive = MTL::PrimitiveTypePoint;
        break;
      case xenos::PrimitiveType::kLineList:
        mtl_primitive = MTL::PrimitiveTypeLine;
        break;
      case xenos::PrimitiveType::kLineStrip:
        mtl_primitive = MTL::PrimitiveTypeLineStrip;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        mtl_primitive = MTL::PrimitiveTypeTriangle;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        mtl_primitive = MTL::PrimitiveTypeTriangleStrip;
        break;
      default:
        XELOGE("SPIRV-Cross: Unsupported host primitive type {}",
               uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    auto request_guest_index_range = [&](uint64_t index_base,
                                         uint32_t index_count,
                                         MTL::IndexType index_type) -> bool {
      if (!shared_memory_) {
        return false;
      }
      uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t);
      uint64_t index_length = uint64_t(index_count) * index_stride;
      if (index_base > SharedMemory::kBufferSize ||
          SharedMemory::kBufferSize - index_base < index_length) {
        return false;
      }
      return shared_memory_->RequestRange(static_cast<uint32_t>(index_base),
                                          static_cast<uint32_t>(index_length));
    };

    bool use_expansion_triangle_list_fallback = false;
    uint32_t draw_index_count =
        primitive_processing_result.host_draw_vertex_count;
    if ((host_vertex_shader_type ==
             Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
         host_vertex_shader_type ==
             Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) &&
        (primitive_processing_result.index_buffer_type ==
             PrimitiveProcessor::ProcessedIndexBufferType::
                 kHostBuiltinForAuto ||
         primitive_processing_result.index_buffer_type ==
             PrimitiveProcessor::ProcessedIndexBufferType::
                 kHostBuiltinForDMA)) {
      // Expansion strips normally rely on primitive restart separators.
      // Keep a Metal-local triangle-list fallback to avoid dependence on strip
      // restart behavior in this SPIRV-Cross path.
      uint32_t strip_index_count = draw_index_count;
      uint32_t expanded_primitive_count =
          strip_index_count ? (strip_index_count + 1u) / 5u : 0u;
      draw_index_count = expanded_primitive_count * 6u;
      mtl_primitive = MTL::PrimitiveTypeTriangle;
      use_expansion_triangle_list_fallback = true;
      static bool logged_expansion_triangle_list_fallback = false;
      if (!logged_expansion_triangle_list_fallback) {
        logged_expansion_triangle_list_fallback = true;
        XELOGW(
            "SPIRV-Cross: Using triangle-list fallback for VS primitive "
            "expansion draws to avoid strip-restart dependency");
      }
    }

    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      // Non-indexed draw.
      current_render_encoder_->drawPrimitives(
          mtl_primitive, NS::UInteger(0),
          NS::UInteger(primitive_processing_result.host_draw_vertex_count));
    } else {
      // Indexed draw.
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      switch (primitive_processing_result.index_buffer_type) {
        case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
          index_buffer = shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
          index_offset = primitive_processing_result.guest_index_base;
          if (!request_guest_index_range(index_offset, draw_index_count,
                                         index_type)) {
            XELOGE("SPIRV-Cross: Failed to validate guest index buffer range");
            return false;
          }
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
          if (primitive_processor_) {
            index_buffer = primitive_processor_->GetConvertedIndexBuffer(
                primitive_processing_result.host_index_buffer_handle,
                index_offset);
          }
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
          if (primitive_processor_) {
            if (use_expansion_triangle_list_fallback) {
              index_buffer =
                  primitive_processor_->GetExpansionTriangleListIndexBuffer();
              index_offset = 0;
              index_type = MTL::IndexTypeUInt32;
            } else {
              index_buffer = primitive_processor_->GetBuiltinIndexBuffer();
              index_offset =
                  primitive_processing_result.host_index_buffer_handle;
            }
          }
          break;
        default:
          XELOGE("SPIRV-Cross: Unsupported index buffer type {}",
                 uint32_t(primitive_processing_result.index_buffer_type));
          return false;
      }
      if (!index_buffer) {
        XELOGE("SPIRV-Cross: Index buffer is null");
        return false;
      }
      UseRenderEncoderResource(index_buffer, MTL::ResourceUsageRead);
      current_render_encoder_->drawIndexedPrimitives(
          mtl_primitive, NS::UInteger(draw_index_count), index_type,
          index_buffer, NS::UInteger(index_offset));
    }
  }

  // Handle memexport.
  if (memexport_used && shared_memory_) {
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      shared_memory_->RangeWrittenByGpu(
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    }
  }

  ++current_draw_index_;
  return true;
}

bool MetalCommandProcessor::IssueCopy() {
  // Finish any in-flight rendering so render target contents are visible to
  // resolve logic.
  EndRenderEncoder();
  MTL::CommandBuffer* copy_command_buffer = EnsureCommandBuffer();
  if (!copy_command_buffer) {
    XELOGE("MetalCommandProcessor::IssueCopy: failed to get command buffer");
    return false;
  }

  if (!render_target_cache_) {
    XELOGW("MetalCommandProcessor::IssueCopy - No render target cache");
    return true;
  }

  uint32_t written_address = 0;
  uint32_t written_length = 0;

  if (!render_target_cache_->Resolve(*memory_, written_address, written_length,
                                     copy_command_buffer)) {
    XELOGE("MetalCommandProcessor::IssueCopy - Resolve failed");
    return false;
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode();
  bool do_readback = (readback_mode != ReadbackResolveMode::kDisabled);
  bool readback_scaled = false;
  bool readback_scaled_gpu = false;
  bool use_gpu_downscale = false;
  bool readback_scheduled = false;
  uint32_t write_index = 0;
  uint32_t read_index = 0;
  bool use_delayed_sync = false;
  bool wait_for_completion = false;
  bool should_copy = false;
  bool is_cache_miss = false;
  uint32_t source_length = 0;
  uint32_t readback_length = 0;
  uint32_t tile_count = 0;
  uint32_t pixel_size_log2 = 0;
  uint32_t scale_x = 1;
  uint32_t scale_y = 1;
  bool half_pixel_offset = false;
  uint32_t source_offset_bytes = 0;
  uint64_t scaled_range_offset_bytes = 0;
  uint64_t readback_base_offset_bytes = 0;
  uint64_t scaled_copy_length = 0;
  size_t source_buffer_binding_offset = 0;
  uint64_t source_offset_bytes_log = 0;

  if (do_readback) {
    // Early check: if destination memory is not accessible, skip readback.
    VirtualHeap* physical_heap = memory_->GetPhysicalHeap();
    bool memory_accessible = false;
    if (physical_heap) {
      HeapAllocationInfo alloc_info;
      if (physical_heap->QueryRegionInfo(written_address, &alloc_info) &&
          (alloc_info.state & kMemoryAllocationCommit) &&
          IsWritableProtect(alloc_info.protect)) {
        uint32_t end_address = written_address + written_length;
        uint32_t region_end = alloc_info.base_address + alloc_info.region_size;
        if (end_address <= region_end) {
          memory_accessible = true;
        }
      }
    }
    if (!memory_accessible) {
      do_readback = false;
    }
  }
  if (!written_length) {
    // Keep the submission open for no-op copies and let primary-buffer end,
    // swap, or explicit sync points choose the commit boundary.
    return true;
  }

  // Track this resolved region so the trace player can avoid overwriting it
  // with stale MemoryRead commands from the trace file.
  MarkResolvedMemory(written_address, written_length);

  // Keep copy-only resolve bursts open so multiple resolves can be coalesced,
  // but commit draw-containing submissions so subsequent work observes the
  // resolved guest memory immediately.
  if (current_draw_index_ == 0) {
    copy_resolve_writes_pending_ = true;
    return true;
  }

  // Resolve touched guest memory in a draw-containing submission; commit now
  // so following packets don't observe stale resolve results.
  if (UseSpirvCrossPath()) {
    ScheduleSpirvUniformBufferRelease(copy_command_buffer);
  }
  copy_command_buffer->commit();
  copy_command_buffer->release();
  current_command_buffer_ = nullptr;
  current_draw_index_ = 0;
  copy_resolve_writes_pending_ = false;

  return true;
}

void MetalCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void MetalCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

void MetalCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    const uint32_t float_constant_index =
        (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    const uint32_t stage_constant_index = float_constant_index & 0xFF;
    const uint32_t map_index = stage_constant_index >> 6;
    const uint64_t map_bit = uint64_t(1) << (stage_constant_index & 63);
    if (float_constant_index >= 256) {
      if (msl_current_float_constant_map_pixel_[map_index] & map_bit) {
        msl_float_constants_dirty_pixel_ = true;
      }
    } else {
      if (msl_current_float_constant_map_vertex_[map_index] & map_bit) {
        msl_float_constants_dirty_vertex_ = true;
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    msl_bool_loop_constants_dirty_ = true;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    msl_fetch_constants_dirty_ = true;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten(
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
    }
  }
}

MTL::CommandBuffer* MetalCommandProcessor::EnsureCommandBuffer() {
  ProcessCompletedSubmissions();
  if (current_command_buffer_) {
    return current_command_buffer_;
  }
  if (!command_queue_) {
    XELOGE("EnsureCommandBuffer: no command queue");
    return nullptr;
  }

  EnsureCommandBufferAutoreleasePool();

  // Note: commandBuffer() returns an autoreleased object, we must retain it.
  current_command_buffer_ = command_queue_->commandBuffer();
  if (!current_command_buffer_) {
    XELOGE("EnsureCommandBuffer: failed to create command buffer");
    DrainCommandBufferAutoreleasePool();
    return nullptr;
  }
  current_command_buffer_->retain();
  current_command_buffer_->setLabel(
      NS::String::string("XeniaCommandBuffer", NS::UTF8StringEncoding));

  if (UseSpirvCrossPath() && !EnsureSpirvUniformBuffer()) {
    static auto last_ensure_uniforms_fail_log =
        std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_ensure_uniforms_fail_log >= std::chrono::seconds(1)) {
      last_ensure_uniforms_fail_log = now;
      XELOGE(
          "EnsureCommandBuffer: failed to prepare SPIRV-Cross uniforms "
          "buffer");
    }
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    DrainCommandBufferAutoreleasePool();
    return nullptr;
  }

  ++submission_current_;
  pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
  current_command_buffer_->addCompletedHandler([this](MTL::CommandBuffer*) {
    completed_command_buffers_.fetch_add(1, std::memory_order_relaxed);
    pending_completion_handlers_.fetch_sub(1, std::memory_order_relaxed);
  });

  if (primitive_processor_) {
    primitive_processor_->BeginSubmission();
  }
  if (texture_cache_) {
    texture_cache_->BeginSubmission(submission_current_);
  }
  if (primitive_processor_ && !frame_open_) {
    primitive_processor_->BeginFrame();
    if (render_target_cache_) {
      render_target_cache_->BeginFrame();
    }
    if (texture_cache_) {
      texture_cache_->BeginFrame();
    }
    frame_open_ = true;
  }

  return current_command_buffer_;
}

void MetalCommandProcessor::ProcessCompletedSubmissions() {
  const uint64_t completed =
      completed_command_buffers_.load(std::memory_order_relaxed);
  if (completed <= submission_completed_processed_) {
    return;
  }
  submission_completed_processed_ = completed;
  if (primitive_processor_) {
    primitive_processor_->CompletedSubmissionUpdated();
  }
  if (texture_cache_) {
    texture_cache_->CompletedSubmissionUpdated(completed);
  }
}

void MetalCommandProcessor::EnsureCommandBufferAutoreleasePool() {
  if (command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_ = NS::AutoreleasePool::alloc()->init();
}

void MetalCommandProcessor::DrainCommandBufferAutoreleasePool() {
  if (!command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_->release();
  command_buffer_autorelease_pool_ = nullptr;
}

void MetalCommandProcessor::ResetMslRenderEncoderStateCache() {
  msl_bound_vertex_texture_count_ = 0;
  msl_bound_pixel_texture_count_ = 0;
  msl_bound_vertex_sampler_count_ = 0;
  msl_bound_pixel_sampler_count_ = 0;
  msl_bound_vertex_texture_binding_uid_ = 0;
  msl_bound_pixel_texture_binding_uid_ = 0;
  msl_bound_vertex_sampler_binding_uid_ = 0;
  msl_bound_pixel_sampler_binding_uid_ = 0;
  msl_bound_vertex_textures_.fill(nullptr);
  msl_bound_pixel_textures_.fill(nullptr);
  msl_bound_vertex_samplers_.fill(nullptr);
  msl_bound_pixel_samplers_.fill(nullptr);
  msl_bound_shared_memory_buffer_ = nullptr;
  msl_bound_vertex_argument_buffer_ = nullptr;
  msl_bound_pixel_argument_buffer_ = nullptr;
  msl_bound_vertex_argument_buffer_offset_ = 0;
  msl_bound_pixel_argument_buffer_offset_ = 0;
  msl_bound_vertex_argument_buffer_offset_valid_ = false;
  msl_bound_pixel_argument_buffer_offset_valid_ = false;
  msl_bound_null_buffer_ = nullptr;
  msl_bound_uniforms_buffer_ = nullptr;
  msl_bound_uniforms_vs_base_offset_ = 0;
  msl_bound_uniforms_ps_base_offset_ = 0;
  msl_bound_uniforms_offsets_valid_ = false;
  msl_bound_pipeline_state_ = nullptr;
  msl_viewport_valid_ = false;
  msl_scissor_valid_ = false;
  msl_rasterizer_state_valid_ = false;
  msl_depth_stencil_state_ = nullptr;
  msl_stencil_reference_valid_ = false;
  msl_stencil_reference_ = 0;
  ff_blend_factor_valid_ = false;
  ResetRenderEncoderResourceUsage();
}

void MetalCommandProcessor::ResetMslCrossEncoderReuseCaches() {
  msl_last_argbuf_vertex_textures_.fill(nullptr);
  msl_last_argbuf_vertex_texture_count_ = 0;
  msl_last_argbuf_vertex_samplers_.fill(nullptr);
  msl_last_argbuf_vertex_sampler_count_ = 0;
  msl_last_argbuf_vertex_buffer_ = nullptr;
  msl_last_argbuf_vertex_offset_ = 0;
  msl_last_argbuf_vertex_translation_ = nullptr;
  msl_last_argbuf_vertex_encoded_length_ = 0;
  msl_last_argbuf_vertex_layout_uid_ = 0;
  msl_last_argbuf_pixel_textures_.fill(nullptr);
  msl_last_argbuf_pixel_texture_count_ = 0;
  msl_last_argbuf_pixel_samplers_.fill(nullptr);
  msl_last_argbuf_pixel_sampler_count_ = 0;
  msl_last_argbuf_pixel_buffer_ = nullptr;
  msl_last_argbuf_pixel_offset_ = 0;
  msl_last_argbuf_pixel_translation_ = nullptr;
  msl_last_argbuf_pixel_encoded_length_ = 0;
  msl_last_argbuf_pixel_layout_uid_ = 0;
}

void MetalCommandProcessor::EndRenderEncoder() {
  if (!current_render_encoder_) {
    return;
  }
  current_render_encoder_->endEncoding();
  current_render_encoder_->release();
  current_render_encoder_ = nullptr;
  current_render_pass_descriptor_ = nullptr;
  ResetMslRenderEncoderStateCache();
}

void MetalCommandProcessor::ResetRenderEncoderResourceUsage() {
  render_encoder_resource_usage_.clear();
  render_encoder_heap_usage_.clear();
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Resource* resource,
                                                     MTL::ResourceUsage usage) {
  if (!current_render_encoder_ || !resource) {
    return;
  }
  UseRenderEncoderHeap(resource->heap());
  uint32_t usage_bits = static_cast<uint32_t>(usage);
  auto it = render_encoder_resource_usage_.find(resource);
  if (it != render_encoder_resource_usage_.end()) {
    if ((it->second & usage_bits) == usage_bits) {
      return;
    }
    it->second |= usage_bits;
  } else {
    render_encoder_resource_usage_.emplace(resource, usage_bits);
  }
  current_render_encoder_->useResource(resource, usage);
}

void MetalCommandProcessor::UseRenderEncoderHeap(MTL::Heap* heap) {
  if (!current_render_encoder_ || !heap) {
    return;
  }
  if (!render_encoder_heap_usage_.insert(heap).second) {
    return;
  }
  current_render_encoder_->useHeap(heap);
}

void MetalCommandProcessor::UseRenderEncoderAttachmentHeaps(
    MTL::RenderPassDescriptor* descriptor) {
  if (!current_render_encoder_ || !descriptor) {
    return;
  }
  auto* color_attachments = descriptor->colorAttachments();
  for (uint32_t i = 0; i < 8; ++i) {
    auto* attachment = color_attachments->object(i);
    if (!attachment) {
      continue;
    }
    MTL::Texture* texture = attachment->texture();
    if (texture) {
      UseRenderEncoderHeap(texture->heap());
    }
  }
  auto* depth_attachment = descriptor->depthAttachment();
  if (depth_attachment && depth_attachment->texture()) {
    UseRenderEncoderHeap(depth_attachment->texture()->heap());
  }
  auto* stencil_attachment = descriptor->stencilAttachment();
  if (stencil_attachment && stencil_attachment->texture()) {
    UseRenderEncoderHeap(stencil_attachment->texture()->heap());
  }
}

void MetalCommandProcessor::BeginCommandBuffer() {
  if (!EnsureCommandBuffer()) {
    return;
  }

  if (!current_render_encoder_ && (!render_encoder_resource_usage_.empty() ||
                                   !render_encoder_heap_usage_.empty())) {
    ResetRenderEncoderResourceUsage();
  }

  // Obtain the render pass descriptor. Prefer the one provided by
  // MetalRenderTargetCache (host render-target path), falling back to the
  // legacy descriptor if needed.
  MTL::RenderPassDescriptor* pass_descriptor = render_pass_descriptor_;
  if (render_target_cache_) {
    if (MTL::RenderPassDescriptor* cache_desc =
            render_target_cache_->GetRenderPassDescriptor(1)) {
      pass_descriptor = cache_desc;
    }
  }
  if (!pass_descriptor) {
    XELOGE("BeginCommandBuffer: No render pass descriptor available");
    return;
  }

  // Detect Reverse-Z usage and update clear depth.
  if (register_file_) {
    auto depth_control = register_file_->Get<reg::RB_DEPTHCONTROL>();
    bool reverse_z =
        depth_control.z_enable &&
        (depth_control.zfunc == xenos::CompareFunction::kGreater ||
         depth_control.zfunc == xenos::CompareFunction::kGreaterEqual);
    if (auto* da = pass_descriptor->depthAttachment()) {
      if (reverse_z) {
        da->setClearDepth(0.0);
      } else {
        da->setClearDepth(1.0);
      }
    }
  }

  // If the render pass configuration has changed since the current render
  // encoder was created (e.g. dummy RT0 -> real RTs, depth/stencil binding),
  // restart the render encoder with the updated descriptor.
  if (current_render_encoder_ &&
      current_render_pass_descriptor_ != pass_descriptor) {
    EndRenderEncoder();
  }

  if (!current_render_encoder_) {
    // If some path cleared the encoder without going through EndRenderEncoder,
    // avoid leaking cached binding state into the new encoder.
    ResetMslRenderEncoderStateCache();
    // Note: renderCommandEncoder() returns an autoreleased object, we must
    // retain it.
    current_render_encoder_ =
        current_command_buffer_->renderCommandEncoder(pass_descriptor);
    if (!current_render_encoder_) {
      XELOGE("Failed to create render command encoder");
      return;
    }
    current_render_encoder_->retain();
    current_render_encoder_->setLabel(
        NS::String::string("XeniaRenderEncoder", NS::UTF8StringEncoding));
    ff_blend_factor_valid_ = false;
    current_render_pass_descriptor_ = pass_descriptor;
    UseRenderEncoderAttachmentHeaps(pass_descriptor);
  }

  // Derive viewport/scissor from the actual bound render target rather than
  // a hard-coded 1280x720. Prefer color RT 0 from the MetalRenderTargetCache,
  // falling back to depth (depth-only passes) and then legacy
  // render_target_width_/height_ when needed.
  uint32_t rt_width = 1;
  uint32_t rt_height = 1;
  GetBoundRenderTargetSize(render_target_cache_.get(), render_target_width_,
                           render_target_height_, rt_width, rt_height);

  // Set viewport
  MTL::Viewport viewport = {
      0.0, 0.0, static_cast<double>(rt_width), static_cast<double>(rt_height),
      0.0, 1.0};
  current_render_encoder_->setViewport(viewport);

  // Set scissor (must not exceed render pass dimensions)
  MTL::ScissorRect scissor = {0, 0, rt_width, rt_height};
  current_render_encoder_->setScissorRect(scissor);
}

bool MetalCommandProcessor::EnsureSpirvUniformBuffer() {
  if (uniforms_buffer_) {
    return true;
  }
  if (!device_) {
    XELOGE("EnsureSpirvUniformBuffer: Metal device is null");
    return false;
  }

  // Keep this aligned with the SPIRV-Cross descriptor table layout used by
  // IssueDrawMsl (6 x 4KB CBVs + texture/sampler descriptor blocks).
  constexpr size_t kUniformsBytesPerTable = 24576;
  constexpr size_t kStageCount = 2;

  if (!draw_ring_count_) {
    XELOGW("SPIRV-Cross: draw ring count was zero, forcing to 1");
    draw_ring_count_ = 1;
  }

  // Keep a slightly larger initial pool on iOS to reduce early-frame pressure.
#if XE_PLATFORM_IOS
  constexpr size_t kUniformsBuffersInFlightInitial = 6;
  // iOS commonly needs extra headroom to avoid command-buffer churn when
  // submissions retire later than the CPU draw cadence.
  constexpr size_t kUniformsBuffersInFlightMax = 24;
#else
  constexpr size_t kUniformsBuffersInFlightInitial = 4;
  constexpr size_t kUniformsBuffersInFlightMax = 12;
#endif

  if (!spirv_uniforms_pool_initialized_) {
    size_t requested_ring_count = std::max<size_t>(1, draw_ring_count_);
    while (requested_ring_count >= 1) {
      const size_t descriptor_table_count = kStageCount * requested_ring_count;
      const size_t uniforms_buffer_size =
          kUniformsBytesPerTable * descriptor_table_count;

      std::vector<MTL::Buffer*> new_pool;
      new_pool.reserve(kUniformsBuffersInFlightInitial);
      bool allocation_failed = false;
      for (size_t i = 0; i < kUniformsBuffersInFlightInitial; ++i) {
        MTL::Buffer* buffer = device_->newBuffer(
            uniforms_buffer_size, MTL::ResourceStorageModeShared);
        if (!buffer) {
          allocation_failed = true;
          break;
        }
        buffer->setLabel(
            NS::String::string("MslUniformsBuffer", NS::UTF8StringEncoding));
        std::memset(buffer->contents(), 0, uniforms_buffer_size);
        new_pool.push_back(buffer);
      }

      if (!allocation_failed &&
          new_pool.size() == kUniformsBuffersInFlightInitial) {
        {
          std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
          for (MTL::Buffer* old_buffer : spirv_uniforms_pool_) {
            if (old_buffer) {
              old_buffer->release();
            }
          }
          spirv_uniforms_pool_ = std::move(new_pool);
          spirv_uniforms_available_.clear();
          spirv_uniforms_available_.insert(spirv_uniforms_available_.end(),
                                           spirv_uniforms_pool_.begin(),
                                           spirv_uniforms_pool_.end());
        }

        if (spirv_uniforms_available_semaphore_) {
#if !OS_OBJECT_USE_OBJC
          dispatch_release(spirv_uniforms_available_semaphore_);
#endif
          spirv_uniforms_available_semaphore_ = nullptr;
        }
        spirv_uniforms_available_semaphore_ = dispatch_semaphore_create(
            static_cast<long>(kUniformsBuffersInFlightInitial));
        if (!spirv_uniforms_available_semaphore_) {
          XELOGE(
              "SPIRV-Cross: failed to create uniforms availability semaphore");
          return false;
        }

        if (requested_ring_count != draw_ring_count_) {
          XELOGW(
              "SPIRV-Cross: reduced uniforms ring from {} to {} pages after "
              "allocation pressure",
              draw_ring_count_, requested_ring_count);
          draw_ring_count_ = requested_ring_count;
        }
        spirv_uniforms_pool_initialized_ = true;
        break;
      }

      for (MTL::Buffer* buffer : new_pool) {
        if (buffer) {
          buffer->release();
        }
      }
      if (requested_ring_count == 1) {
        break;
      }
      const size_t fallback_ring_count =
          std::max<size_t>(1, requested_ring_count / 2);
      XELOGW(
          "SPIRV-Cross: failed to allocate uniforms pool with {} ring pages, "
          "retrying with {}",
          requested_ring_count, fallback_ring_count);
      requested_ring_count = fallback_ring_count;
    }

    if (!spirv_uniforms_pool_initialized_) {
      XELOGE(
          "Failed to create uniforms buffer pool for SPIRV-Cross path (ring "
          "pages={}, bytes per table={})",
          draw_ring_count_, kUniformsBytesPerTable);
      return false;
    }
  }

  if (!spirv_uniforms_available_semaphore_) {
    XELOGE("SPIRV-Cross: uniforms pool semaphore is not initialized");
    return false;
  }

  const auto try_grow_uniforms_pool = [&]() -> bool {
    size_t pool_size = 0;
    {
      std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
      pool_size = spirv_uniforms_pool_.size();
      if (pool_size >= kUniformsBuffersInFlightMax) {
        return false;
      }
    }
    const size_t descriptor_table_count =
        kStageCount * std::max<size_t>(size_t(1), draw_ring_count_);
    const size_t uniforms_buffer_size =
        kUniformsBytesPerTable * descriptor_table_count;
    MTL::Buffer* buffer = device_->newBuffer(uniforms_buffer_size,
                                             MTL::ResourceStorageModeShared);
    if (!buffer) {
      return false;
    }
    buffer->setLabel(
        NS::String::string("MslUniformsBufferGrow", NS::UTF8StringEncoding));
    std::memset(buffer->contents(), 0, uniforms_buffer_size);
    size_t new_pool_size = 0;
    {
      std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
      spirv_uniforms_pool_.push_back(buffer);
      spirv_uniforms_available_.push_back(buffer);
      new_pool_size = spirv_uniforms_pool_.size();
    }
    dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
    XELOGW("SPIRV-Cross: grew uniforms pool to {} buffers under load",
           new_pool_size);
    return true;
  };

  if (dispatch_semaphore_wait(spirv_uniforms_available_semaphore_,
                              DISPATCH_TIME_NOW) != 0) {
    bool acquired_after_grow = false;
    if (try_grow_uniforms_pool()) {
      acquired_after_grow =
          dispatch_semaphore_wait(spirv_uniforms_available_semaphore_,
                                  DISPATCH_TIME_NOW) == 0;
    }
    if (!acquired_after_grow) {
      // Last resort: block until one in-flight command buffer retires.
      // D3D12-style behavior is to avoid this in common paths by growing first.
      static auto last_wait_log = std::chrono::steady_clock::time_point{};
      static uint32_t suppressed_wait_logs = 0;
      const auto now = std::chrono::steady_clock::now();
      if (now - last_wait_log >= std::chrono::seconds(10)) {
        last_wait_log = now;
        size_t pool_size = 0;
        size_t available_size = 0;
        {
          std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
          pool_size = spirv_uniforms_pool_.size();
          available_size = spirv_uniforms_available_.size();
        }
        XELOGW(
            "SPIRV-Cross: uniforms pool busy; waiting for an in-flight command "
            "buffer to retire (in-use={}, total={}, available={}, ring "
            "pages={}, suppressed_wait_logs={})",
            pool_size - available_size, pool_size, available_size,
            draw_ring_count_, suppressed_wait_logs);
        suppressed_wait_logs = 0;
      } else {
        ++suppressed_wait_logs;
      }
      dispatch_semaphore_wait(spirv_uniforms_available_semaphore_,
                              DISPATCH_TIME_FOREVER);
    }
  }

  {
    std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
    if (spirv_uniforms_available_.empty()) {
      XELOGE(
          "SPIRV-Cross: uniforms semaphore signaled but no reusable buffer is "
          "available");
      return false;
    }
    uniforms_buffer_ = spirv_uniforms_available_.back();
    spirv_uniforms_available_.pop_back();
  }
  if (uniforms_buffer_) {
    command_buffer_spirv_uniform_buffers_.push_back(uniforms_buffer_);
  }

  return uniforms_buffer_ != nullptr;
}

bool MetalCommandProcessor::EnsureSpirvUniformBufferCapacity() {
  if (!draw_ring_count_) {
    draw_ring_count_ = 1;
  }
  if (!uniforms_buffer_) {
    return EnsureSpirvUniformBuffer();
  }
  if (current_draw_index_ == 0) {
    return true;
  }
  const uint32_t ring_index =
      current_draw_index_ % uint32_t(std::max<size_t>(1, draw_ring_count_));
  if (ring_index != 0) {
    return true;
  }

  // Try to rotate to another uniforms buffer in the current command buffer to
  // avoid forcing a split at every ring wrap.
  uniforms_buffer_ = nullptr;
  const auto try_acquire_uniforms_buffer = [&]() -> bool {
    if (!spirv_uniforms_available_semaphore_ ||
        dispatch_semaphore_wait(spirv_uniforms_available_semaphore_,
                                DISPATCH_TIME_NOW) != 0) {
      return false;
    }
    std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
    if (!spirv_uniforms_available_.empty()) {
      uniforms_buffer_ = spirv_uniforms_available_.back();
      spirv_uniforms_available_.pop_back();
      command_buffer_spirv_uniform_buffers_.push_back(uniforms_buffer_);
      return true;
    }
    // Keep semaphore state consistent if availability changed concurrently.
    dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
    return false;
  };

#if XE_PLATFORM_IOS
  constexpr size_t kUniformsBuffersInFlightMax = 24;
#else
  constexpr size_t kUniformsBuffersInFlightMax = 12;
#endif
  const auto try_grow_uniforms_pool = [&]() -> bool {
    if (!device_) {
      return false;
    }
    size_t pool_size = 0;
    {
      std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
      pool_size = spirv_uniforms_pool_.size();
      if (pool_size >= kUniformsBuffersInFlightMax) {
        return false;
      }
    }
    const size_t descriptor_table_count =
        kStageCount * std::max<size_t>(size_t(1), draw_ring_count_);
    const size_t uniforms_buffer_size =
        kUniformsBytesPerTable * descriptor_table_count;
    MTL::Buffer* buffer = device_->newBuffer(uniforms_buffer_size,
                                             MTL::ResourceStorageModeShared);
    if (!buffer) {
      return false;
    }
    buffer->setLabel(
        NS::String::string("MslUniformsBufferGrow", NS::UTF8StringEncoding));
    std::memset(buffer->contents(), 0, uniforms_buffer_size);
    size_t new_pool_size = 0;
    {
      std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
      spirv_uniforms_pool_.push_back(buffer);
      spirv_uniforms_available_.push_back(buffer);
      new_pool_size = spirv_uniforms_pool_.size();
    }
    dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
    XELOGW(
        "SPIRV-Cross: grew uniforms pool to {} buffers at ring-wrap pressure",
        new_pool_size);
    return true;
  };

  if (try_acquire_uniforms_buffer()) {
    return true;
  }
  if (try_grow_uniforms_pool() && try_acquire_uniforms_buffer()) {
    return true;
  }

  static bool rollover_logged = false;
  if (!rollover_logged) {
    rollover_logged = true;
    XELOGW(
        "SPIRV-Cross: uniforms ring exhausted; rotating Metal command buffer");
  }

  EndCommandBuffer();
  BeginCommandBuffer();
  if (!current_command_buffer_ || !current_render_encoder_ ||
      !uniforms_buffer_) {
    XELOGE(
        "SPIRV-Cross: failed to restart command buffer after uniforms ring "
        "rollover");
    return false;
  }
  return true;
}

void MetalCommandProcessor::ScheduleSpirvUniformBufferRelease(
    MTL::CommandBuffer* command_buffer) {
  if (!command_buffer) {
    return;
  }

  std::vector<MTL::Buffer*> submitted_uniforms;
  if (!command_buffer_spirv_uniform_buffers_.empty()) {
    submitted_uniforms.swap(command_buffer_spirv_uniform_buffers_);
  } else if (uniforms_buffer_) {
    submitted_uniforms.reserve(1);
    submitted_uniforms.push_back(uniforms_buffer_);
  }
  uniforms_buffer_ = nullptr;

  if (submitted_uniforms.empty()) {
    return;
  }

  pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
  command_buffer->addCompletedHandler(
      [this, submitted_uniforms =
                 std::move(submitted_uniforms)](MTL::CommandBuffer*) mutable {
        size_t returned_count = 0;
        {
          std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
          for (MTL::Buffer* uniforms : submitted_uniforms) {
            if (!uniforms) {
              continue;
            }
            spirv_uniforms_available_.push_back(uniforms);
            ++returned_count;
          }
        }
        if (spirv_uniforms_available_semaphore_) {
          for (size_t i = 0; i < returned_count; ++i) {
            dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
          }
        }
        pending_completion_handlers_.fetch_sub(1, std::memory_order_relaxed);
      });
}

bool MetalCommandProcessor::AcquireSpirvArgumentBufferSlice(
    uint32_t bytes, uint32_t alignment, MTL::Buffer** buffer_out,
    NS::UInteger* offset_out) {
  if (!buffer_out || !offset_out) {
    return false;
  }
  *buffer_out = nullptr;
  *offset_out = 0;
  if (!device_ || !current_command_buffer_ || bytes == 0) {
    return false;
  }

  const size_t align = std::max<size_t>(1, size_t(alignment));
  auto align_up = [](size_t value, size_t alignment) -> size_t {
    return ((value + alignment - 1) / alignment) * alignment;
  };

  if (!command_buffer_spirv_argbuf_pages_.empty()) {
    auto& page = command_buffer_spirv_argbuf_pages_.back();
    const size_t aligned_offset = align_up(page->offset, align);
    if (aligned_offset + bytes <= page->bytes) {
      page->offset = aligned_offset + bytes;
      *buffer_out = page->buffer;
      *offset_out = NS::UInteger(aligned_offset);
      return *buffer_out != nullptr;
    }
  }

  constexpr size_t kDefaultSpirvArgumentBufferPageBytes = 1024 * 1024;
  const size_t required_page_bytes = align_up(bytes, align);
  const size_t page_bytes =
      std::max(kDefaultSpirvArgumentBufferPageBytes, required_page_bytes);

  std::shared_ptr<SpirvArgumentBufferPage> page;
  {
    std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
    for (auto it = spirv_argbuf_pool_.begin(); it != spirv_argbuf_pool_.end();
         ++it) {
      if ((*it) && (*it)->bytes >= page_bytes) {
        page = *it;
        spirv_argbuf_pool_.erase(it);
        break;
      }
    }
  }
  if (!page) {
    page = std::make_shared<SpirvArgumentBufferPage>();
    page->bytes = page_bytes;
    page->buffer =
        device_->newBuffer(page_bytes, MTL::ResourceStorageModeShared);
    if (!page->buffer) {
      return false;
    }
  }
  page->offset = 0;
  command_buffer_spirv_argbuf_pages_.push_back(page);

  const size_t aligned_offset = align_up(page->offset, align);
  if (aligned_offset + bytes > page->bytes) {
    return false;
  }
  page->offset = aligned_offset + bytes;
  *buffer_out = page->buffer;
  *offset_out = NS::UInteger(aligned_offset);
  return *buffer_out != nullptr;
}

void MetalCommandProcessor::ScheduleSpirvArgumentBufferRelease(
    MTL::CommandBuffer* command_buffer) {
  if (!command_buffer || command_buffer_spirv_argbuf_pages_.empty()) {
    return;
  }

  std::vector<std::shared_ptr<SpirvArgumentBufferPage>> pages;
  pages.swap(command_buffer_spirv_argbuf_pages_);

  bool add_handler = false;
  {
    std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
    auto& pending = pending_spirv_argbuf_releases_[command_buffer];
    add_handler = pending.empty();
    pending.reserve(pending.size() + pages.size());
    for (auto& page : pages) {
      pending.push_back(std::move(page));
    }
  }

  if (add_handler) {
    pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
    command_buffer->addCompletedHandler(
        [this](MTL::CommandBuffer* completed_cmd) {
          {
            std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
            auto it = pending_spirv_argbuf_releases_.find(completed_cmd);
            if (it != pending_spirv_argbuf_releases_.end()) {
              for (auto& page : it->second) {
                if (page) {
                  page->offset = 0;
                  spirv_argbuf_pool_.push_back(std::move(page));
                }
              }
              pending_spirv_argbuf_releases_.erase(it);
            }
          }
          pending_completion_handlers_.fetch_sub(1, std::memory_order_relaxed);
        });
  }
}

void MetalCommandProcessor::EndCommandBuffer() {
  EndRenderEncoder();
  ResetMslCrossEncoderReuseCaches();

  if (current_command_buffer_) {
    if (UseSpirvCrossPath()) {
      ScheduleSpirvUniformBufferRelease(current_command_buffer_);
    }
    ScheduleSpirvArgumentBufferRelease(current_command_buffer_);
    current_command_buffer_->commit();
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    current_draw_index_ = 0;
  }
  copy_resolve_writes_pending_ = false;
  DrainCommandBufferAutoreleasePool();
}

void MetalCommandProcessor::ApplyDepthStencilState(
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
  if (!current_render_encoder_ || !device_) {
    return;
  }

  const RegisterFile& regs = *register_file_;
  auto stencil_ref_mask_front = regs.Get<reg::RB_STENCILREFMASK>();
  auto stencil_ref_mask_back =
      regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto depth_control = normalized_depth_control;

  bool has_stencil_attachment = false;
  if (current_render_pass_descriptor_) {
    if (auto* stencil_attachment =
            current_render_pass_descriptor_->stencilAttachment()) {
      has_stencil_attachment = stencil_attachment->texture() != nullptr;
    }
  }

  if (!has_stencil_attachment && depth_control.stencil_enable) {
    static bool no_stencil_logged = false;
    if (!no_stencil_logged) {
      no_stencil_logged = true;
      XELOGW(
          "Metal: stencil enabled but no stencil attachment bound; disabling "
          "stencil for this pass");
    }
    depth_control.stencil_enable = 0;
    depth_control.backface_enable = 0;
    depth_control.stencilfunc = xenos::CompareFunction::kAlways;
    depth_control.stencilfail = xenos::StencilOp::kKeep;
    depth_control.stencilzpass = xenos::StencilOp::kKeep;
    depth_control.stencilzfail = xenos::StencilOp::kKeep;
    depth_control.stencilfunc_bf = xenos::CompareFunction::kAlways;
    depth_control.stencilfail_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzpass_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzfail_bf = xenos::StencilOp::kKeep;
    stencil_ref_mask_front.value = 0;
    stencil_ref_mask_back.value = 0;
  }

  DepthStencilStateKey key;
  key.depth_control = depth_control.value;
  key.stencil_ref_mask_front = stencil_ref_mask_front.value;
  key.stencil_ref_mask_back = stencil_ref_mask_back.value;
  key.polygonal_and_backface = (primitive_polygonal ? 1u : 0u) |
                               (depth_control.backface_enable ? 2u : 0u);

  MTL::DepthStencilState* state = nullptr;
  auto it = depth_stencil_state_cache_.find(key);
  if (it != depth_stencil_state_cache_.end()) {
    state = it->second;
  } else {
    MTL::DepthStencilDescriptor* ds_desc =
        MTL::DepthStencilDescriptor::alloc()->init();
    if (depth_control.z_enable) {
      ds_desc->setDepthCompareFunction(
          ToMetalCompareFunction(depth_control.zfunc));
      ds_desc->setDepthWriteEnabled(depth_control.z_write_enable != 0);
    } else {
      ds_desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
      ds_desc->setDepthWriteEnabled(false);
    }

    if (depth_control.stencil_enable) {
      auto* front = MTL::StencilDescriptor::alloc()->init();
      front->setStencilCompareFunction(
          ToMetalCompareFunction(depth_control.stencilfunc));
      front->setStencilFailureOperation(
          ToMetalStencilOperation(depth_control.stencilfail));
      front->setDepthFailureOperation(
          ToMetalStencilOperation(depth_control.stencilzfail));
      front->setDepthStencilPassOperation(
          ToMetalStencilOperation(depth_control.stencilzpass));
      front->setReadMask(stencil_ref_mask_front.stencilmask);
      front->setWriteMask(stencil_ref_mask_front.stencilwritemask);

      ds_desc->setFrontFaceStencil(front);

      if (primitive_polygonal && depth_control.backface_enable) {
        auto* back = MTL::StencilDescriptor::alloc()->init();
        back->setStencilCompareFunction(
            ToMetalCompareFunction(depth_control.stencilfunc_bf));
        back->setStencilFailureOperation(
            ToMetalStencilOperation(depth_control.stencilfail_bf));
        back->setDepthFailureOperation(
            ToMetalStencilOperation(depth_control.stencilzfail_bf));
        back->setDepthStencilPassOperation(
            ToMetalStencilOperation(depth_control.stencilzpass_bf));
        back->setReadMask(stencil_ref_mask_back.stencilmask);
        back->setWriteMask(stencil_ref_mask_back.stencilwritemask);
        ds_desc->setBackFaceStencil(back);
        back->release();
      } else {
        ds_desc->setBackFaceStencil(front);
      }

      front->release();
    }

    state = device_->newDepthStencilState(ds_desc);
    ds_desc->release();

    if (!state) {
      XELOGE("Failed to create Metal depth/stencil state");
      return;
    }
    depth_stencil_state_cache_.emplace(key, state);
  }

  current_render_encoder_->setDepthStencilState(state);

  if (depth_control.stencil_enable) {
    uint32_t ref_front = stencil_ref_mask_front.stencilref;
    uint32_t ref_back = stencil_ref_mask_back.stencilref;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    uint32_t ref = ref_front;
    if (primitive_polygonal && depth_control.backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      ref = ref_back;
    } else if (primitive_polygonal && depth_control.backface_enable &&
               ref_front != ref_back) {
      static bool mismatch_logged = false;
      if (!mismatch_logged) {
        mismatch_logged = true;
        XELOGW(
            "Metal: front/back stencil ref differ (front={}, back={}); using "
            "front for both",
            ref_front, ref_back);
      }
    }
    current_render_encoder_->setStencilReferenceValue(ref);
  }
}

void MetalCommandProcessor::ApplyRasterizerState(bool primitive_polygonal) {
  if (!current_render_encoder_ || !render_target_cache_) {
    return;
  }

  const RegisterFile& regs = *register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();

  MTL::CullMode cull_mode = MTL::CullModeNone;
  if (primitive_polygonal) {
    bool cull_front = pa_su_sc_mode_cntl.cull_front;
    bool cull_back = pa_su_sc_mode_cntl.cull_back;
    if (cull_front && !cull_back) {
      cull_mode = MTL::CullModeFront;
    } else if (cull_back && !cull_front) {
      cull_mode = MTL::CullModeBack;
    }
  }
  current_render_encoder_->setCullMode(cull_mode);

  current_render_encoder_->setFrontFacingWinding(
      pa_su_sc_mode_cntl.face ? MTL::WindingClockwise
                              : MTL::WindingCounterClockwise);

  MTL::TriangleFillMode fill_mode = MTL::TriangleFillModeFill;
  if (primitive_polygonal &&
      pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
    xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
    if (!pa_su_sc_mode_cntl.cull_front) {
      polygon_type =
          std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
    }
    if (!pa_su_sc_mode_cntl.cull_back) {
      polygon_type =
          std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
    }
    if (polygon_type != xenos::PolygonType::kTriangles) {
      fill_mode = MTL::TriangleFillModeLines;
    }
  }
  current_render_encoder_->setTriangleFillMode(fill_mode);

  float polygon_offset_scale = 0.0f;
  float polygon_offset = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(
      regs, primitive_polygonal, polygon_offset_scale, polygon_offset);
  float depth_bias_factor = regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                                    xenos::DepthRenderTargetFormat::kD24S8
                                ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                                : draw_util::kD3D10PolygonOffsetFactorFloat24;
  float depth_bias_constant = polygon_offset * depth_bias_factor;
  float depth_bias_slope =
      polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit *
      float(std::max(render_target_cache_->draw_resolution_scale_x(),
                     render_target_cache_->draw_resolution_scale_y()));
  current_render_encoder_->setDepthBias(depth_bias_constant, depth_bias_slope,
                                        0.0f);

  current_render_encoder_->setDepthClipMode(pa_cl_clip_cntl.clip_disable
                                                ? MTL::DepthClipModeClamp
                                                : MTL::DepthClipModeClip);
}

MTL::RenderPassDescriptor*
MetalCommandProcessor::GetCurrentRenderPassDescriptor() {
  return render_pass_descriptor_;
}

// ==========================================================================
// SPIRV-Cross tessellation support.
// ==========================================================================

// Tessellation factor compute kernels are defined in msl_tess_factor_kernels.h.
#include "xenia/gpu/metal/msl_tess_factor_kernels.h"

bool MetalCommandProcessor::InitializeMslTessellation() {
  if (!device_) {
    return false;
  }

  auto compile_kernel =
      [&](const char* source,
          const char* function_name) -> MTL::ComputePipelineState* {
    NS::Error* error = nullptr;
    auto* src = NS::String::string(source, NS::UTF8StringEncoding);
    auto* opts = MTL::CompileOptions::alloc()->init();
    opts->setFastMathEnabled(true);
    MTL::Library* lib = device_->newLibrary(src, opts, &error);
    opts->release();
    if (!lib) {
      if (error) {
        XELOGE("Tessellation kernel compile error: {}",
               error->localizedDescription()->utf8String());
      }
      return nullptr;
    }
    auto* fn_name = NS::String::string(function_name, NS::UTF8StringEncoding);
    MTL::Function* fn = lib->newFunction(fn_name);
    lib->release();
    if (!fn) {
      XELOGE("Tessellation kernel: function '{}' not found", function_name);
      return nullptr;
    }
    MTL::ComputePipelineState* pso =
        device_->newComputePipelineState(fn, &error);
    fn->release();
    if (!pso && error) {
      XELOGE("Tessellation kernel PSO error: {}",
             error->localizedDescription()->utf8String());
    }
    return pso;
  };

  // Uniform factor kernels (discrete / continuous modes).
  tess_factor_pipeline_tri_ =
      compile_kernel(kMslTessFactorUniformTriangle, "tess_factor_triangle");
  tess_factor_pipeline_quad_ =
      compile_kernel(kMslTessFactorUniformQuad, "tess_factor_quad");

  if (!tess_factor_pipeline_tri_ || !tess_factor_pipeline_quad_) {
    XELOGW(
        "SPIRV-Cross: Failed to create uniform tessellation factor "
        "pipelines");
    return false;
  }

  // Adaptive factor kernels (per-edge factors from shared memory).
  tess_factor_pipeline_adaptive_tri_ = compile_kernel(
      kMslTessFactorAdaptiveTriangle, "tess_factor_adaptive_triangle");
  tess_factor_pipeline_adaptive_quad_ =
      compile_kernel(kMslTessFactorAdaptiveQuad, "tess_factor_adaptive_quad");

  if (!tess_factor_pipeline_adaptive_tri_ ||
      !tess_factor_pipeline_adaptive_quad_) {
    XELOGW(
        "SPIRV-Cross: Failed to create adaptive tessellation factor "
        "pipelines (adaptive tessellation will fall back to uniform)");
    // Non-fatal — adaptive tessellation will degrade to uniform factors.
  }

  XELOGI("SPIRV-Cross: Tessellation factor pipelines initialized");
  return true;
}

void MetalCommandProcessor::ShutdownMslTessellation() {
  for (auto& [key, pso] : msl_tess_pipeline_cache_) {
    if (pso) {
      pso->release();
    }
  }
  msl_tess_pipeline_cache_.clear();
  if (tess_factor_buffer_) {
    tess_factor_buffer_->release();
    tess_factor_buffer_ = nullptr;
    tess_factor_buffer_patch_capacity_ = 0;
  }
  if (tess_factor_pipeline_tri_) {
    tess_factor_pipeline_tri_->release();
    tess_factor_pipeline_tri_ = nullptr;
  }
  if (tess_factor_pipeline_quad_) {
    tess_factor_pipeline_quad_->release();
    tess_factor_pipeline_quad_ = nullptr;
  }
  if (tess_factor_pipeline_adaptive_tri_) {
    tess_factor_pipeline_adaptive_tri_->release();
    tess_factor_pipeline_adaptive_tri_ = nullptr;
  }
  if (tess_factor_pipeline_adaptive_quad_) {
    tess_factor_pipeline_adaptive_quad_->release();
    tess_factor_pipeline_adaptive_quad_ = nullptr;
  }
}

bool MetalCommandProcessor::EnsureTessFactorBuffer(uint32_t patch_count) {
  // MTLQuadTessellationFactorsHalf is the larger of the two (12 bytes vs 8).
  constexpr size_t kMaxFactorSize = 12;
  size_t needed = size_t(patch_count) * kMaxFactorSize;
  if (tess_factor_buffer_ && tess_factor_buffer_->length() >= needed) {
    // Buffer already large enough; don't reduce the tracked capacity.
    return true;
  }
  if (tess_factor_buffer_) {
    tess_factor_buffer_->release();
  }
  // Round up and over-allocate for future growth.
  size_t alloc_size = std::max(needed, size_t(4096));
  alloc_size = (alloc_size + 4095) & ~size_t(4095);
  // Use Shared storage so the CPU can fill tessellation factors directly
  // (avoids needing a separate compute encoder for uniform factors).
  tess_factor_buffer_ =
      device_->newBuffer(alloc_size, MTL::ResourceStorageModeShared);
  if (!tess_factor_buffer_) {
    XELOGE("Failed to allocate tessellation factor buffer ({} bytes)",
           alloc_size);
    return false;
  }
  tess_factor_buffer_->setLabel(
      NS::String::string("Xenia Tess Factor Buffer", NS::UTF8StringEncoding));
  tess_factor_buffer_patch_capacity_ = uint32_t(alloc_size / kMaxFactorSize);
  return true;
}

MTL::RenderPipelineState*
MetalCommandProcessor::GetOrCreateMslTessPipelineState(
    MslShader::MslTranslation* domain_translation,
    MslShader::MslTranslation* pixel_translation,
    Shader::HostVertexShaderType host_vertex_shader_type,
    const RegisterFile& regs) {
  if (!domain_translation || !domain_translation->metal_function()) {
    XELOGE("SPIRV-Cross tess: No domain shader function");
    return nullptr;
  }

  // Determine attachment formats from render target cache (same pattern as
  // GetOrCreateMslPipelineState).
  uint32_t sample_count = 1;
  MTL::PixelFormat color_formats[4] = {
      MTL::PixelFormatInvalid, MTL::PixelFormatInvalid, MTL::PixelFormatInvalid,
      MTL::PixelFormatInvalid};
  MTL::PixelFormat depth_format = MTL::PixelFormatInvalid;
  MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
  if (render_target_cache_) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (MTL::Texture* rt = render_target_cache_->GetColorTargetForDraw(i)) {
        color_formats[i] = rt->pixelFormat();
        if (rt->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(rt->sampleCount()));
        }
      }
    }
    if (color_formats[0] == MTL::PixelFormatInvalid) {
      if (MTL::Texture* dummy =
              render_target_cache_->GetDummyColorTargetForDraw()) {
        color_formats[0] = dummy->pixelFormat();
        if (dummy->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(dummy->sampleCount()));
        }
      }
    }
    if (MTL::Texture* depth_tex =
            render_target_cache_->GetDepthTargetForDraw()) {
      depth_format = depth_tex->pixelFormat();
      switch (depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          stencil_format = depth_format;
          break;
        default:
          stencil_format = MTL::PixelFormatInvalid;
          break;
      }
      if (depth_tex->sampleCount() > 0) {
        sample_count = std::max<uint32_t>(
            sample_count, static_cast<uint32_t>(depth_tex->sampleCount()));
      }
    }
  }

  // Keep tessellation PSO attachment formats/sample count in sync with the
  // active render pass descriptor to satisfy Metal validation.
  MTL::RenderPassDescriptor* pass_descriptor = current_render_pass_descriptor_;
  if (!pass_descriptor) {
    pass_descriptor = render_pass_descriptor_;
  }
  if (pass_descriptor) {
    sample_count = 1;
    for (uint32_t i = 0; i < 4; ++i) {
      color_formats[i] = MTL::PixelFormatInvalid;
    }
    depth_format = MTL::PixelFormatInvalid;
    stencil_format = MTL::PixelFormatInvalid;
    PopulatePipelineFormatsFromRenderPassDescriptor(
        pass_descriptor, color_formats, 4, &depth_format, &stencil_format,
        &sample_count);
  }
  bool fragment_writes_depth =
      (pixel_translation && pixel_translation->shader().writes_depth()) ||
      (!pixel_translation && depth_only_pixel_library_ &&
       !depth_only_pixel_function_name_.empty());
  EnsureDepthFormatForDepthWritingFragment(
      "SPIRV-Cross tess pipeline", fragment_writes_depth, &depth_format);

  // Build cache key incorporating RT formats, tessellation mode, and blend
  // state (same blend fields as GetOrCreateMslPipelineState).
  auto tess_mode = regs.Get<reg::VGT_HOS_CNTL>().tess_mode;
  uint32_t pixel_shader_writes_color_targets =
      pixel_translation ? pixel_translation->shader().writes_color_targets()
                        : 0;
  uint32_t normalized_color_mask = 0;
  if (pixel_shader_writes_color_targets) {
    normalized_color_mask = draw_util::GetNormalizedColorMask(
        regs, pixel_shader_writes_color_targets);
  }
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  struct TessPipelineKey {
    const void* ds;
    const void* ps;
    uint32_t host_vertex_shader_type;
    uint32_t tess_mode;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};
  key_data.ds = domain_translation;
  key_data.ps = pixel_translation;
  key_data.host_vertex_shader_type = uint32_t(host_vertex_shader_type);
  key_data.tess_mode = uint32_t(tess_mode);
  key_data.sample_count = sample_count;
  key_data.depth_format = uint32_t(depth_format);
  key_data.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(color_formats[i]);
  }
  key_data.normalized_color_mask = normalized_color_mask;
  key_data.alpha_to_mask_enable = rb_colorcontrol.alpha_to_mask_enable ? 1 : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.blendcontrol[i] =
        regs.Get<reg::RB_BLENDCONTROL>(
                reg::RB_BLENDCONTROL::rt_register_indices[i])
            .value;
  }
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));
  auto it = msl_tess_pipeline_cache_.find(key);
  if (it != msl_tess_pipeline_cache_.end()) {
    return it->second;
  }

  auto* desc = MTL::RenderPipelineDescriptor::alloc()->init();
  // The post-tessellation vertex function IS the domain shader.
  desc->setVertexFunction(domain_translation->metal_function());

  if (pixel_translation && pixel_translation->metal_function()) {
    desc->setFragmentFunction(pixel_translation->metal_function());
  } else if (depth_only_pixel_function_name_.size() &&
             depth_only_pixel_library_) {
    auto* fn_name = NS::String::string(depth_only_pixel_function_name_.c_str(),
                                       NS::UTF8StringEncoding);
    MTL::Function* depth_fn = depth_only_pixel_library_->newFunction(fn_name);
    if (depth_fn) {
      desc->setFragmentFunction(depth_fn);
      depth_fn->release();
    }
  }

  // Tessellation configuration.
  desc->setMaxTessellationFactor(64);
  desc->setTessellationFactorStepFunction(
      MTL::TessellationFactorStepFunctionPerPatch);

  switch (tess_mode) {
    case xenos::TessellationMode::kDiscrete:
      desc->setTessellationPartitionMode(MTL::TessellationPartitionModeInteger);
      break;
    case xenos::TessellationMode::kContinuous:
      desc->setTessellationPartitionMode(
          MTL::TessellationPartitionModeFractionalEven);
      break;
    case xenos::TessellationMode::kAdaptive:
      desc->setTessellationPartitionMode(
          MTL::TessellationPartitionModeFractionalEven);
      break;
  }

  // Control point index type (not needed for our use case since the
  // domain shader reads control points from shared memory).
  desc->setTessellationControlPointIndexType(
      MTL::TessellationControlPointIndexTypeNone);

  // Render target attachments with blend state (same as non-tess pipeline).
  desc->setAlphaToCoverageEnabled(key_data.alpha_to_mask_enable != 0);
  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    color_attachment->setPixelFormat(color_formats[i]);
    if (color_formats[i] == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingEnabled(false);
      continue;
    }
    uint32_t rt_write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingEnabled(false);
      continue;
    }
    auto blendcontrol = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[i]);
    MTL::BlendFactor src_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_srcblend);
    MTL::BlendFactor dst_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_destblend);
    MTL::BlendOperation op_rgb =
        ToMetalBlendOperation(blendcontrol.color_comb_fcn);
    MTL::BlendFactor src_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_srcblend);
    MTL::BlendFactor dst_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_destblend);
    MTL::BlendOperation op_alpha =
        ToMetalBlendOperation(blendcontrol.alpha_comb_fcn);
    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    color_attachment->setBlendingEnabled(blending_enabled);
    if (blending_enabled) {
      color_attachment->setSourceRGBBlendFactor(src_rgb);
      color_attachment->setDestinationRGBBlendFactor(dst_rgb);
      color_attachment->setRgbBlendOperation(op_rgb);
      color_attachment->setSourceAlphaBlendFactor(src_alpha);
      color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
      color_attachment->setAlphaBlendOperation(op_alpha);
    }
  }
  desc->setDepthAttachmentPixelFormat(depth_format);
  desc->setStencilAttachmentPixelFormat(stencil_format);
  desc->setSampleCount(sample_count);

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pso = device_->newRenderPipelineState(desc, &error);
  desc->release();

  if (!pso) {
    if (error) {
      XELOGE("SPIRV-Cross tess pipeline error: {}",
             error->localizedDescription()->utf8String());
    }
    return nullptr;
  }

  msl_tess_pipeline_cache_[key] = pso;
  return pso;
}

// ==========================================================================
// SPIRV-Cross (MSL) path — shader modification + system constants helpers.
// ==========================================================================

SpirvShaderTranslator::Modification
MetalCommandProcessor::GetCurrentSpirvVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask) const {
  const auto& regs = *register_file_;

  SpirvShaderTranslator::Modification modification(
      spirv_shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);

  modification.vertex.output_point_parameters =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                   xenos::PrimitiveType::kPointList);

  return modification;
}

SpirvShaderTranslator::Modification
MetalCommandProcessor::GetCurrentSpirvPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) const {
  const auto& regs = *register_file_;

  SpirvShaderTranslator::Modification modification(
      spirv_shader_translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask &
      ~xenos::GetInterpolatorSamplingPattern(
          regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
          regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
          regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                 xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  using DepthStencilMode =
      SpirvShaderTranslator::Modification::DepthStencilMode;
  if (shader.implicit_early_z_write_allowed() &&
      (!shader.writes_color_target(0) ||
       !draw_util::DoesCoverageDependOnAlpha(
           regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }

  // Initialize MIN/MAX blend pre-multiply factors to kOne (no pre-multiply).
  modification.pixel.rt0_blend_rgb_factor_for_premult =
      xenos::BlendFactor::kOne;
  modification.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;

  bool rt0_minmax_premult_rgb_expected = false;
  bool rt0_minmax_premult_a_expected = false;
  if (shader.writes_color_target(0)) {
    auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[0]);
    rt0_minmax_premult_rgb_expected =
        (blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.color_destblend == xenos::BlendFactor::kOne;
    rt0_minmax_premult_a_expected =
        (blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.alpha_destblend == xenos::BlendFactor::kOne;
    if (rt0_minmax_premult_rgb_expected) {
      modification.pixel.rt0_blend_rgb_factor_for_premult =
          xenos::BlendFactor::kSrcAlpha;
    }
    if (rt0_minmax_premult_a_expected) {
      modification.pixel.rt0_blend_a_factor_for_premult =
          xenos::BlendFactor::kSrcAlpha;
    }
  }
  if (rt0_minmax_premult_rgb_expected || rt0_minmax_premult_a_expected) {
    XELOGD(
        "SPIRV-Cross PS mod diagnostic: shader={:016X} expected_premult(rgb={},"
        "a={}) selected(rgb={},a={})",
        shader.ucode_data_hash(), rt0_minmax_premult_rgb_expected ? 1 : 0,
        rt0_minmax_premult_a_expected ? 1 : 0,
        uint32_t(modification.pixel.rt0_blend_rgb_factor_for_premult),
        uint32_t(modification.pixel.rt0_blend_a_factor_for_premult));
  }

  // Extract 1 bit per RT from the 4-bits-per-RT normalized_color_mask.
  // Without this, color_targets_used defaults to 0 and the SPIR-V translator
  // declares NO fragment color outputs, producing black/transparent rendering.
  modification.pixel.color_targets_used =
      (((normalized_color_mask >> 0) & 0xF) ? 1 : 0) |
      (((normalized_color_mask >> 4) & 0xF) ? 2 : 0) |
      (((normalized_color_mask >> 8) & 0xF) ? 4 : 0) |
      (((normalized_color_mask >> 12) & 0xF) ? 8 : 0);

  return modification;
}

MTL::RenderPipelineState* MetalCommandProcessor::GetOrCreateMslPipelineState(
    MslShader::MslTranslation* vertex_translation,
    MslShader::MslTranslation* pixel_translation, const RegisterFile& regs,
    MslPipelineCompileStatus* compile_status_out) {
  if (compile_status_out) {
    *compile_status_out = MslPipelineCompileStatus::kFailed;
  }
  if (!vertex_translation || !vertex_translation->metal_function()) {
    XELOGE("SPIRV-Cross: No valid vertex shader function");
    return nullptr;
  }

  // Determine attachment formats from render target cache.
  uint32_t sample_count = 1;
  MTL::PixelFormat color_formats[4] = {
      MTL::PixelFormatInvalid, MTL::PixelFormatInvalid, MTL::PixelFormatInvalid,
      MTL::PixelFormatInvalid};
  MTL::PixelFormat depth_format = MTL::PixelFormatInvalid;
  MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
  if (render_target_cache_) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (MTL::Texture* rt = render_target_cache_->GetColorTargetForDraw(i)) {
        color_formats[i] = rt->pixelFormat();
        if (rt->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(rt->sampleCount()));
        }
      }
    }
    if (color_formats[0] == MTL::PixelFormatInvalid) {
      if (MTL::Texture* dummy =
              render_target_cache_->GetDummyColorTargetForDraw()) {
        color_formats[0] = dummy->pixelFormat();
        if (dummy->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(dummy->sampleCount()));
        }
      }
    }
    if (MTL::Texture* depth_tex =
            render_target_cache_->GetDepthTargetForDraw()) {
      depth_format = depth_tex->pixelFormat();
      switch (depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          stencil_format = depth_format;
          break;
        default:
          stencil_format = MTL::PixelFormatInvalid;
          break;
      }
      if (depth_tex->sampleCount() > 0) {
        sample_count = std::max<uint32_t>(
            sample_count, static_cast<uint32_t>(depth_tex->sampleCount()));
      }
    }
  }

  // Match the active render pass attachments exactly to avoid
  // setRenderPipelineState validation failures when the pass descriptor differs
  // from the cache snapshot (for instance, after attachment reconfiguration).
  MTL::RenderPassDescriptor* pass_descriptor = current_render_pass_descriptor_;
  if (!pass_descriptor) {
    pass_descriptor = render_pass_descriptor_;
  }
  if (pass_descriptor) {
    sample_count = 1;
    for (uint32_t i = 0; i < 4; ++i) {
      color_formats[i] = MTL::PixelFormatInvalid;
    }
    depth_format = MTL::PixelFormatInvalid;
    stencil_format = MTL::PixelFormatInvalid;
    PopulatePipelineFormatsFromRenderPassDescriptor(
        pass_descriptor, color_formats, 4, &depth_format, &stencil_format,
        &sample_count);
  }
  bool pixel_shader_writes_depth =
      pixel_translation && pixel_translation->shader().writes_depth();
  EnsureDepthFormatForDepthWritingFragment(
      "SPIRV-Cross pipeline", pixel_shader_writes_depth, &depth_format);

  // Build pipeline cache key.
  struct MslPipelineKey {
    const void* vs;
    const void* ps;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};
  key_data.vs = vertex_translation;
  key_data.ps = pixel_translation;
  key_data.sample_count = sample_count;
  key_data.depth_format = uint32_t(depth_format);
  key_data.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(color_formats[i]);
  }
  uint32_t pixel_shader_writes_color_targets =
      pixel_translation ? pixel_translation->shader().writes_color_targets()
                        : 0;
  key_data.normalized_color_mask = 0;
  if (pixel_shader_writes_color_targets) {
    key_data.normalized_color_mask = draw_util::GetNormalizedColorMask(
        regs, pixel_shader_writes_color_targets);
  }
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  key_data.alpha_to_mask_enable = rb_colorcontrol.alpha_to_mask_enable ? 1 : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.blendcontrol[i] =
        regs.Get<reg::RB_BLENDCONTROL>(
                reg::RB_BLENDCONTROL::rt_register_indices[i])
            .value;
  }
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));

  bool use_async = cvars::async_shader_compilation &&
                   !msl_shader_compile_threads_.empty() &&
                   pixel_translation != nullptr;

  {
    std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
    auto it = msl_pipeline_cache_.find(key);
    if (it != msl_pipeline_cache_.end()) {
      if (compile_status_out) {
        *compile_status_out = MslPipelineCompileStatus::kReady;
      }
      return it->second;
    }
    if (msl_pipeline_compile_pending_.find(key) !=
        msl_pipeline_compile_pending_.end()) {
      if (compile_status_out) {
        *compile_status_out = MslPipelineCompileStatus::kPending;
      }
      return nullptr;
    }
    if (msl_pipeline_compile_failed_.find(key) !=
        msl_pipeline_compile_failed_.end()) {
      return nullptr;
    }
  }

  MslPipelineCompileRequest request = {};
  request.pipeline_key = key;
  request.vertex_shader_hash = vertex_translation->shader().ucode_data_hash();
  request.vertex_modification = vertex_translation->modification();
  if (pixel_translation) {
    request.pixel_shader_hash = pixel_translation->shader().ucode_data_hash();
    request.pixel_modification = pixel_translation->modification();
  }
  request.vertex_function = vertex_translation->metal_function();
  request.fragment_function =
      pixel_translation ? pixel_translation->metal_function() : nullptr;
  request.sample_count = sample_count;
  request.depth_format = depth_format;
  request.stencil_format = stencil_format;
  request.normalized_color_mask = key_data.normalized_color_mask;
  request.alpha_to_mask_enable = key_data.alpha_to_mask_enable;
  request.priority = pixel_translation ? 2 : 1;
  for (uint32_t i = 0; i < 4; ++i) {
    request.color_formats[i] = color_formats[i];
    request.blendcontrol[i] = key_data.blendcontrol[i];
  }

  if (use_async) {
    if (EnqueueMslPipelineCompilation(request)) {
      std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
      auto it = msl_pipeline_cache_.find(key);
      if (it != msl_pipeline_cache_.end()) {
        if (compile_status_out) {
          *compile_status_out = MslPipelineCompileStatus::kReady;
        }
        return it->second;
      }
      if (msl_pipeline_compile_failed_.find(key) !=
          msl_pipeline_compile_failed_.end()) {
        return nullptr;
      }
      if (compile_status_out) {
        *compile_status_out = MslPipelineCompileStatus::kPending;
      }
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
    if (msl_pipeline_compile_failed_.find(key) !=
        msl_pipeline_compile_failed_.end()) {
      return nullptr;
    }
  }

  std::string error_message;
  MTL::RenderPipelineState* pipeline =
      CreateMslPipelineState(request, &error_message);
  if (!pipeline) {
    if (!error_message.empty()) {
      XELOGE("SPIRV-Cross: Failed to create pipeline: {}", error_message);
    } else {
      XELOGE("SPIRV-Cross: Failed to create pipeline (unknown error)");
    }
    std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
    msl_pipeline_compile_failed_.insert(key);
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(msl_shader_compile_mutex_);
    auto [it, inserted] = msl_pipeline_cache_.emplace(key, pipeline);
    if (!inserted) {
      pipeline->release();
      pipeline = it->second;
    }
    msl_pipeline_compile_failed_.erase(key);
  }
  if (compile_status_out) {
    *compile_status_out = MslPipelineCompileStatus::kReady;
  }
  return pipeline;
}

void MetalCommandProcessor::UpdateSpirvSystemConstantValues(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  const SpirvShaderTranslator::SystemConstants previous_system_constants =
      spirv_system_constants_;
  const SpirvShaderTranslator::ClipPlaneConstants
      previous_clip_plane_constants = spirv_clip_plane_constants_;
  const SpirvShaderTranslator::TessellationConstants
      previous_tessellation_constants = spirv_tessellation_constants_;

  const RegisterFile& regs = *register_file_;
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  auto& consts = spirv_system_constants_;
  std::memset(&consts, 0, sizeof(consts));

  // Build flags (matching Vulkan backend's SpirvShaderTranslator kSysFlag_*).
  uint32_t flags = 0;

  // Coordinate format.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }

  // Primitive type.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineList ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineStrip ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineLoop ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::k2DLineStrip) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }

  // MSAA sample count.
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;

  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }

  // Alpha test — pack the CompareFunction value directly into the flag bits
  // (matching Vulkan backend behavior).
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;

  // Gamma correction for render targets.
  reg::RB_COLOR_INFO color_infos[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (color_infos[i].color_format ==
        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
  }

  // Vertex index loading for VS-based primitive expansion (point sprites,
  // rectangle lists).  When the primitive processor builds a host-side
  // index buffer for DMA-based VS expansion the shader must load the
  // original guest vertex index from shared memory.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator::
          kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
  }

  consts.flags = flags;

  // Vertex index.
  consts.vertex_index_endian = index_endian;
  consts.vertex_base_index = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  const bool is_vs_expansion_draw =
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
  consts.vertex_index_count =
      is_vs_expansion_draw ? primitive_processing_result.guest_draw_vertex_count
                           : primitive_processing_result.host_draw_vertex_count;

  // Vertex index load address (for VS-based primitive expansion).
  if (flags &
      (SpirvShaderTranslator::kSysFlag_VertexIndexLoad |
       SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    consts.vertex_index_load_address =
        primitive_processing_result.guest_index_base;
  }

  // NDC scale/offset.
  for (uint32_t i = 0; i < 3; ++i) {
    consts.ndc_scale[i] = viewport_info.ndc_scale[i];
    consts.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point rendering (matching Vulkan backend).
  auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
  auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
  consts.point_vertex_diameter_min =
      float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
  consts.point_vertex_diameter_max =
      float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
  consts.point_constant_diameter[0] =
      float(pa_su_point_size.width) * (2.0f / 16.0f);
  consts.point_constant_diameter[1] =
      float(pa_su_point_size.height) * (2.0f / 16.0f);
  // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
  // to radius conversion — matching the Vulkan backend formula.
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  consts.point_screen_diameter_to_ndc_radius[0] =
      float(draw_resolution_scale_x) /
      float(std::max(viewport_info.xy_extent[0], uint32_t(1)));
  consts.point_screen_diameter_to_ndc_radius[1] =
      float(draw_resolution_scale_y) /
      float(std::max(viewport_info.xy_extent[1], uint32_t(1)));

  // Texture swizzled signs and swizzles — retrieved from the texture cache
  // (matching Vulkan backend behavior).
  if (texture_cache_) {
    for (uint32_t i = 0; i < 32; ++i) {
      if (!(used_texture_mask & (uint32_t(1) << i))) {
        continue;
      }
      // Swizzled signs: 8 bits per texture, 4 textures per uint32.
      uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(i);
      uint32_t signs_shift = 8 * (i & 3);
      consts.texture_swizzled_signs[i >> 2] |= uint32_t(texture_signs)
                                               << signs_shift;

      // Host swizzles: 12 bits per texture, 2 textures per uint32.
      uint32_t texture_swizzle = texture_cache_->GetActiveTextureHostSwizzle(i);
      uint32_t swizzle_shift = 12 * (i & 1);
      consts.texture_swizzles[i >> 1] |= (texture_swizzle & 0xFFF)
                                         << swizzle_shift;
    }
  }

  // Textures resolved — which textures are from scaled resolve operations
  // (matching Vulkan backend).
  if (texture_cache_) {
    uint32_t textures_resolved = 0;
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      textures_resolved |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
    consts.textures_resolved = textures_resolved;
  }

  // Alpha test reference.
  consts.alpha_test_reference = rb_alpha_ref;

  // Alpha to mask — if enabled, bits 0:7 are sample offsets, bit 8 = 1.
  // (matching Vulkan backend / MSC path).
  if (rb_colorcontrol.alpha_to_mask_enable) {
    consts.alpha_to_mask = (rb_colorcontrol.value >> 24) | (1 << 8);
  }

  // Color exponent bias (matching Vulkan backend).
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t color_exp_bias = color_infos[i].color_exp_bias;
    if (render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        ((color_infos[i].color_format ==
              xenos::ColorRenderTargetFormat::k_16_16 &&
          !render_target_cache_->IsFixedRG16TruncatedToMinus1To1()) ||
         (color_infos[i].color_format ==
              xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
          !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1()))) {
      color_exp_bias -= 5;
    }
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    consts.color_exp_bias[i] = color_exp_bias_scale;
  }

  // Clip plane constants (separate buffer for the SPIR-V translator).
  // The SPIR-V translator reads clip planes from a dedicated uniform buffer
  // (kConstantBufferClipPlanes), not from system constants.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  std::memset(&spirv_clip_plane_constants_, 0,
              sizeof(spirv_clip_plane_constants_));
  if (!pa_cl_clip_cntl.clip_disable && pa_cl_clip_cntl.ucp_ena) {
    float* clip_plane_write_ptr =
        spirv_clip_plane_constants_.user_clip_planes[0];
    uint32_t clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t clip_plane_index;
    while (xe::bit_scan_forward(clip_planes_remaining, &clip_plane_index)) {
      clip_planes_remaining &= ~(UINT32_C(1) << clip_plane_index);
      const float* clip_plane_regs = reinterpret_cast<const float*>(
          &regs.values[XE_GPU_REG_PA_CL_UCP_0_X + clip_plane_index * 4]);
      std::memcpy(clip_plane_write_ptr, clip_plane_regs, 4 * sizeof(float));
      clip_plane_write_ptr += 4;
    }
  }

  // Tessellation constants (separate buffer for the SPIR-V translator).
  // Mirror Vulkan constant buffer population to keep shader inputs identical.
  std::memset(&spirv_tessellation_constants_, 0,
              sizeof(spirv_tessellation_constants_));
  float tess_factor_min =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tess_factor_max =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  spirv_tessellation_constants_.tessellation_factor_range[0] = tess_factor_min;
  spirv_tessellation_constants_.tessellation_factor_range[1] = tess_factor_max;
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  spirv_tessellation_constants_.vertex_index_endian =
      static_cast<uint32_t>(vgt_dma_size.swap_mode);
  spirv_tessellation_constants_.vertex_index_offset =
      regs[XE_GPU_REG_VGT_INDX_OFFSET];
  spirv_tessellation_constants_.vertex_index_min_max[0] =
      regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
  spirv_tessellation_constants_.vertex_index_min_max[1] =
      regs[XE_GPU_REG_VGT_MAX_VTX_INDX];

  if (std::memcmp(&previous_system_constants, &spirv_system_constants_,
                  sizeof(spirv_system_constants_)) != 0) {
    ++msl_system_constants_version_;
    if (msl_system_constants_version_ == 0) {
      msl_system_constants_version_ = 1;
    }
  }
  if (std::memcmp(&previous_clip_plane_constants, &spirv_clip_plane_constants_,
                  sizeof(spirv_clip_plane_constants_)) != 0) {
    ++msl_clip_plane_constants_version_;
    if (msl_clip_plane_constants_version_ == 0) {
      msl_clip_plane_constants_version_ = 1;
    }
  }
  if (std::memcmp(&previous_tessellation_constants,
                  &spirv_tessellation_constants_,
                  sizeof(spirv_tessellation_constants_)) != 0) {
    ++msl_tessellation_constants_version_;
    if (msl_tessellation_constants_version_ == 0) {
      msl_tessellation_constants_version_ = 1;
    }
  }
}

#define COMMAND_PROCESSOR MetalCommandProcessor
#include "../pm4_command_processor_implement.h"
#undef COMMAND_PROCESSOR

}  // namespace metal
}  // namespace gpu
}  // namespace xe
