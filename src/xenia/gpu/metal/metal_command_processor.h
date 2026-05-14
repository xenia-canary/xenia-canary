/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_
#define XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_

#include <dispatch/dispatch.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/base/platform.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/metal/metal_primitive_processor.h"
#include "xenia/gpu/metal/metal_render_target_cache.h"
#include "xenia/gpu/metal/metal_shared_memory.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/metal/msl_bindings.h"
#include "xenia/gpu/metal/msl_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/ui/metal/metal_api.h"
#include "xenia/ui/metal/metal_provider.h"

namespace MTL {
class Heap;
class SharedEvent;
}  // namespace MTL

namespace xe {
namespace gpu {
namespace metal {

class MetalGraphicsSystem;

class MetalCommandProcessor : public CommandProcessor {
 protected:
#define OVERRIDING_BASE_CMDPROCESSOR
#include "../pm4_command_processor_declare.h"
#undef OVERRIDING_BASE_CMDPROCESSOR

 public:
  explicit MetalCommandProcessor(MetalGraphicsSystem* graphics_system,
                                 kernel::KernelState* kernel_state);
  ~MetalCommandProcessor();

  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  void RestoreEdramSnapshot(const void* snapshot) override;
  void ClearCaches() override;
  void InvalidateGpuMemory() override;
  void ClearReadbackBuffers() override;

  std::string GetTitleStateSuffix() const override;

  // Track memory regions written by IssueCopy (resolve) so trace playback
  // can skip overwriting them with stale data from the trace file.
  void MarkResolvedMemory(uint32_t base_ptr, uint32_t length);
  bool IsResolvedMemory(uint32_t base_ptr, uint32_t length) const;
  void ClearResolvedMemory();

  ui::metal::MetalProvider& GetMetalProvider() const;

  // Get the Metal device and command queue
  MTL::Device* GetMetalDevice() const { return device_; }
  MTL::CommandQueue* GetMetalCommandQueue() const { return command_queue_; }
  MTL::CommandBuffer* GetCurrentCommandBuffer() const {
    return current_command_buffer_;
  }
  bool HasActiveRenderEncoder() const {
    return current_render_encoder_ != nullptr;
  }
  uint32_t current_draw_index() const { return current_draw_index_; }
  uint64_t GetCurrentSubmission() const;
  uint64_t GetCompletedSubmission() const override;
  MTL::CommandBuffer* EnsureCommandBuffer();
  void EndRenderEncoder();
  void ResetRenderEncoderResourceUsage();
  void UseRenderEncoderResource(MTL::Resource* resource,
                                MTL::ResourceUsage usage);
  void EnsureCommandBufferAutoreleasePool();
  void DrainCommandBufferAutoreleasePool();

  // Get current render pass descriptor (for render target binding)
  MTL::RenderPassDescriptor* GetCurrentRenderPassDescriptor();

  // Force issue a swap to push render target to presenter (for trace dumps)
  void ForceIssueSwap();
  bool HasSeenSwap() const { return saw_swap_; }
  void SetSwapDestSwap(uint32_t dest_base, bool swap);
  bool ConsumeSwapDestSwap(uint32_t dest_base, bool* swap_out);

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr) override;

  // Flush pending GPU work before entering wait state.
  // This ensures Metal command buffers are submitted and completed before
  // the autorelease pool is drained, preventing hangs from deferred
  // deallocation.
  void PrepareForWait() override;

  // Use base class WriteRegister - don't override with empty implementation!
  // The base class stores values in register_file_->values[] which we need.
  void OnPrimaryBufferEnd() override;
  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address,
                     uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;
  // SPIRV-Cross draw path — called from IssueDraw. Handles shader translation,
  // pipeline creation, resource binding, and draw dispatch using native Metal
  // encoder calls.
  bool IssueDrawMsl(
      Shader* vertex_shader, Shader* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool primitive_polygonal, bool is_rasterization_done, bool memexport_used,
      uint32_t normalized_color_mask, const RegisterFile& regs);
  bool IssueCopy() override;
  void WriteRegister(uint32_t index, uint32_t value) override;

 private:
  // Initialize shader translation pipeline
  bool InitializeShaderTranslation();

  // Command buffer management
  void BeginCommandBuffer();
  void EndCommandBuffer();
  // Reset per-render-encoder cached bindings/state for the SPIRV-Cross (MSL)
  // path. Safe to call when no render encoder is active.
  void ResetMslRenderEncoderStateCache();
  // Reset cross-encoder SPIRV-Cross reuse caches at command-buffer boundaries.
  void ResetMslCrossEncoderReuseCaches();
  bool CanEndSubmissionImmediately();
  void WaitForPendingCompletionHandlers();
  void ProcessCompletedSubmissions();
  bool EnsureDrawRingCapacity();
  void UseRenderEncoderAttachmentHeaps(MTL::RenderPassDescriptor* descriptor);
  void UseRenderEncoderHeap(MTL::Heap* heap);
  // SPIRV-Cross path: uniforms buffer is command-buffer scoped to avoid
  // CPU writes racing ahead of in-flight GPU reads.
  bool EnsureSpirvUniformBuffer();
  bool EnsureSpirvUniformBufferCapacity();
  void ScheduleSpirvUniformBufferRelease(MTL::CommandBuffer* command_buffer);
  bool AcquireSpirvArgumentBufferSlice(uint32_t bytes, uint32_t alignment,
                                       MTL::Buffer** buffer_out,
                                       NS::UInteger* offset_out);
  void ScheduleSpirvArgumentBufferRelease(MTL::CommandBuffer* command_buffer);

  // Fixed-function depth/stencil state (mirrors Vulkan/D3D12 dynamic state).
  void ApplyDepthStencilState(bool primitive_polygonal,
                              reg::RB_DEPTHCONTROL normalized_depth_control);
  void ApplyRasterizerState(bool primitive_polygonal);

  // Constants shared between MSC and SPIRV-Cross paths.
  static constexpr size_t kStageCount = 2;  // Vertex + pixel.
  static constexpr size_t kNullBufferSize = 4096;
  static constexpr size_t kCbvSizeBytes = 4096;
  static constexpr size_t kUniformsBytesPerTable = 6 * kCbvSizeBytes;

  struct SpirvArgumentBufferPage {
    MTL::Buffer* buffer = nullptr;
    size_t bytes = 0;
    size_t offset = 0;

    ~SpirvArgumentBufferPage();
  };

  // System constants population (mirrors D3D12 implementation)
  void UpdateSystemConstantValues(bool shared_memory_is_uav,
                                  bool primitive_polygonal,
                                  uint32_t line_loop_closing_index,
                                  xenos::Endian index_endian,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);

  // SPIRV-Cross (MSL) path - shader modification and pipeline helpers.
  SpirvShaderTranslator::Modification GetCurrentSpirvVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  SpirvShaderTranslator::Modification GetCurrentSpirvPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask) const;
  void UpdateSpirvSystemConstantValues(
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool primitive_polygonal, uint32_t line_loop_closing_index,
      xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
      uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask);
  enum class MslShaderCompileStatus {
    kReady,
    kPending,
    kFailed,
    kNotQueued,
  };
  enum class MslPipelineCompileStatus {
    kReady,
    kPending,
    kFailed,
  };
  struct MslPipelineCompileRequest;
  void InitializeMslAsyncCompilation();
  void ShutdownMslAsyncCompilation();
  MslShaderCompileStatus GetMslShaderCompileStatus(
      MslShader::MslTranslation* translation);
  bool EnqueueMslShaderCompilation(MslShader::MslTranslation* translation,
                                   bool is_ios, uint8_t priority);
  bool EnqueueMslPipelineCompilation(const MslPipelineCompileRequest& request);
  MTL::RenderPipelineState* CreateMslPipelineState(
      const MslPipelineCompileRequest& request, std::string* error_out);
  void MslShaderCompileThread(size_t thread_index);
  MTL::RenderPipelineState* GetOrCreateMslPipelineState(
      MslShader::MslTranslation* vertex_translation,
      MslShader::MslTranslation* pixel_translation, const RegisterFile& regs,
      MslPipelineCompileStatus* compile_status_out = nullptr);

  // Metal device and command queue (from provider)
  MTL::Device* device_ = nullptr;
  MTL::CommandQueue* command_queue_ = nullptr;
  MTL::SharedEvent* wait_shared_event_ = nullptr;
  uint64_t wait_shared_event_value_ = 0;

  // Render targets
  MTL::Texture* render_target_texture_ = nullptr;
  MTL::Texture* depth_stencil_texture_ = nullptr;
  MTL::RenderPassDescriptor* render_pass_descriptor_ = nullptr;
  uint32_t render_target_width_ = 1280;
  uint32_t render_target_height_ = 720;

  // Current command buffer and encoder
  MTL::CommandBuffer* current_command_buffer_ = nullptr;
  MTL::RenderCommandEncoder* current_render_encoder_ = nullptr;
  MTL::RenderPassDescriptor* current_render_pass_descriptor_ = nullptr;
  NS::AutoreleasePool* command_buffer_autorelease_pool_ = nullptr;

  // Tracks resources marked via useResource for the current render encoder
  // to avoid redundant driver calls across draws within the same encoder.
  std::unordered_map<MTL::Resource*, uint32_t> render_encoder_resource_usage_;
  std::unordered_set<MTL::Heap*> render_encoder_heap_usage_;

  // Shared memory for Xbox 360 memory access
  std::unique_ptr<MetalSharedMemory> shared_memory_;
  std::unique_ptr<MetalPrimitiveProcessor> primitive_processor_;
  bool frame_open_ = false;

  bool saw_swap_ = false;
  uint32_t last_swap_ptr_ = 0;
  uint32_t last_swap_width_ = 0;
  uint32_t last_swap_height_ = 0;
  std::unordered_map<uint32_t, bool> swap_dest_swaps_by_base_;

 public:
  MetalSharedMemory* shared_memory() const { return shared_memory_.get(); }
  MetalRenderTargetCache* render_target_cache() const {
    return render_target_cache_.get();
  }
  MetalTextureCache* texture_cache() const { return texture_cache_.get(); }

 private:
  // Shader ucode disassembly buffer (used by AnalyzeUcode).
  StringBuffer ucode_disasm_buffer_;

  // SPIRV-Cross (MSL) path - shader translator and cache.
  std::unique_ptr<SpirvShaderTranslator> spirv_shader_translator_;
  std::unordered_map<uint64_t, std::unique_ptr<MslShader>> msl_shader_cache_;
  SpirvShaderTranslator::SystemConstants spirv_system_constants_ = {};
  SpirvShaderTranslator::ClipPlaneConstants spirv_clip_plane_constants_ = {};
  SpirvShaderTranslator::TessellationConstants spirv_tessellation_constants_ =
      {};
  struct MslShaderCompileRequest {
    MslShader::MslTranslation* translation = nullptr;
    uint64_t shader_hash = 0;
    uint64_t modification = 0;
    bool is_ios = false;
    uint8_t priority = 0;
  };
  struct MslPipelineCompileRequest {
    uint64_t pipeline_key = 0;
    uint64_t vertex_shader_hash = 0;
    uint64_t vertex_modification = 0;
    uint64_t pixel_shader_hash = 0;
    uint64_t pixel_modification = 0;
    MTL::Function* vertex_function = nullptr;
    MTL::Function* fragment_function = nullptr;
    uint32_t sample_count = 1;
    MTL::PixelFormat color_formats[4] = {
        MTL::PixelFormatInvalid, MTL::PixelFormatInvalid,
        MTL::PixelFormatInvalid, MTL::PixelFormatInvalid};
    MTL::PixelFormat depth_format = MTL::PixelFormatInvalid;
    MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
    uint32_t normalized_color_mask = 0;
    uint32_t alpha_to_mask_enable = 0;
    uint32_t blendcontrol[4] = {};
    uint8_t priority = 0;
  };
  struct MslShaderCompileRequestCompare {
    bool operator()(const MslShaderCompileRequest& a,
                    const MslShaderCompileRequest& b) const {
      return a.priority < b.priority;
    }
  };
  struct MslPipelineCompileRequestCompare {
    bool operator()(const MslPipelineCompileRequest& a,
                    const MslPipelineCompileRequest& b) const {
      return a.priority < b.priority;
    }
  };
  std::priority_queue<MslShaderCompileRequest,
                      std::vector<MslShaderCompileRequest>,
                      MslShaderCompileRequestCompare>
      msl_shader_compile_queue_;
  std::priority_queue<MslPipelineCompileRequest,
                      std::vector<MslPipelineCompileRequest>,
                      MslPipelineCompileRequestCompare>
      msl_pipeline_compile_queue_;
  std::unordered_set<MslShader::MslTranslation*> msl_shader_compile_pending_;
  std::unordered_set<MslShader::MslTranslation*> msl_shader_compile_failed_;
  std::unordered_set<uint64_t> msl_pipeline_compile_pending_;
  std::unordered_set<uint64_t> msl_pipeline_compile_failed_;
  std::mutex msl_shader_compile_mutex_;
  std::condition_variable msl_shader_compile_cv_;
  std::vector<std::thread> msl_shader_compile_threads_;
  size_t msl_shader_compile_busy_ = 0;
  bool msl_shader_compile_shutdown_ = false;
  std::atomic<int64_t> msl_shader_compile_failure_last_log_ns_{0};
  std::atomic<int64_t> msl_pipeline_compile_failure_last_log_ns_{0};
  std::atomic<int64_t> msl_pipeline_pending_last_log_ns_{0};
  std::unordered_map<uint64_t, MTL::RenderPipelineState*> msl_pipeline_cache_;

  // SPIRV-Cross tessellation support.
  MTL::ComputePipelineState* tess_factor_pipeline_tri_ = nullptr;
  MTL::ComputePipelineState* tess_factor_pipeline_quad_ = nullptr;
  // Adaptive tessellation factor pipelines (read per-edge factors from shared
  // memory instead of using a uniform value).
  MTL::ComputePipelineState* tess_factor_pipeline_adaptive_tri_ = nullptr;
  MTL::ComputePipelineState* tess_factor_pipeline_adaptive_quad_ = nullptr;
  MTL::Buffer* tess_factor_buffer_ = nullptr;
  uint32_t tess_factor_buffer_patch_capacity_ = 0;
  std::unordered_map<uint64_t, MTL::RenderPipelineState*>
      msl_tess_pipeline_cache_;
  bool InitializeMslTessellation();
  void ShutdownMslTessellation();
  MTL::RenderPipelineState* GetOrCreateMslTessPipelineState(
      MslShader::MslTranslation* domain_translation,
      MslShader::MslTranslation* pixel_translation,
      Shader::HostVertexShaderType host_vertex_shader_type,
      const RegisterFile& regs);
  bool EnsureTessFactorBuffer(uint32_t patch_count);

  struct DepthStencilStateKey {
    uint32_t depth_control;
    uint32_t stencil_ref_mask_front;
    uint32_t stencil_ref_mask_back;
    uint32_t polygonal_and_backface;
    bool operator==(const DepthStencilStateKey& other) const {
      return depth_control == other.depth_control &&
             stencil_ref_mask_front == other.stencil_ref_mask_front &&
             stencil_ref_mask_back == other.stencil_ref_mask_back &&
             polygonal_and_backface == other.polygonal_and_backface;
    }
    struct Hasher {
      size_t operator()(const DepthStencilStateKey& key) const {
        size_t h = size_t(key.depth_control);
        h ^= size_t(key.stencil_ref_mask_front) << 1;
        h ^= size_t(key.stencil_ref_mask_back) << 2;
        h ^= size_t(key.polygonal_and_backface) << 3;
        return h;
      }
    };
  };

  std::unordered_map<DepthStencilStateKey, MTL::DepthStencilState*,
                     DepthStencilStateKey::Hasher>
      depth_stencil_state_cache_;

  bool mesh_shader_supported_ = false;

  // Texture cache for guest texture uploads
  std::unique_ptr<MetalTextureCache> texture_cache_;

  // Render target cache for framebuffer management
  std::unique_ptr<MetalRenderTargetCache> render_target_cache_;

  // Null resources for unbound slots (shared between MSC and SPIRV-Cross)
  MTL::Buffer* null_buffer_ = nullptr;
  MTL::Texture* null_texture_ = nullptr;
  MTL::SamplerState* null_sampler_ = nullptr;

  // Uniforms buffer and draw ring count (shared between MSC and SPIRV-Cross)
  MTL::Buffer* uniforms_buffer_ = nullptr;
  // SPIRV path may rotate through multiple uniforms buffers within a single
  // Metal command buffer (at ring wrap boundaries). Track all of them so
  // completion handlers can return every buffer to the available pool.
  std::vector<MTL::Buffer*> command_buffer_spirv_uniform_buffers_;
  size_t draw_ring_count_ = 0;
  // Owning storage for all SPIRV-Cross uniforms buffers allocated for the
  // current context.
  std::vector<MTL::Buffer*> spirv_uniforms_pool_;
  // Reusable SPIRV-Cross uniforms buffers returned from completed command
  // buffers to reduce iOS allocation churn.
  std::vector<MTL::Buffer*> spirv_uniforms_available_;
  std::mutex spirv_uniforms_mutex_;
  dispatch_semaphore_t spirv_uniforms_available_semaphore_ = nullptr;
  bool spirv_uniforms_pool_initialized_ = false;
  std::vector<std::shared_ptr<SpirvArgumentBufferPage>> spirv_argbuf_pool_;
  std::vector<std::shared_ptr<SpirvArgumentBufferPage>>
      command_buffer_spirv_argbuf_pages_;
  std::unordered_map<MTL::CommandBuffer*,
                     std::vector<std::shared_ptr<SpirvArgumentBufferPage>>>
      pending_spirv_argbuf_releases_;
  std::mutex spirv_argbuf_mutex_;

  MTL::Library* depth_only_pixel_library_ = nullptr;
  std::string depth_only_pixel_function_name_;

  bool logged_missing_texture_warning_ = false;
  // SPIRV-Cross system/clip/tess constants versioning.
  // Each ring-table slot tracks the last source version copied into it so
  // draws can skip per-draw memcmp/copy churn for unchanged constants.
  uint64_t msl_system_constants_version_ = 1;
  uint64_t msl_clip_plane_constants_version_ = 1;
  uint64_t msl_tessellation_constants_version_ = 1;
  MTL::Buffer* msl_constants_versioned_uniform_buffer_ = nullptr;
  std::vector<uint64_t> msl_system_constants_written_vertex_versions_;
  std::vector<uint64_t> msl_system_constants_written_pixel_versions_;
  std::vector<uint64_t> msl_clip_plane_constants_written_vertex_versions_;
  std::vector<uint64_t> msl_tessellation_constants_written_vertex_versions_;
  std::vector<uint64_t> msl_tessellation_constants_written_pixel_versions_;
  // SPIRV-Cross path: highest texture/sampler slot counts bound on the current
  // render encoder. Used to clear trailing slots when a later draw uses fewer
  // resources, preventing stale state leakage between draws.
  uint32_t msl_bound_vertex_texture_count_ = 0;
  uint32_t msl_bound_pixel_texture_count_ = 0;
  uint32_t msl_bound_vertex_sampler_count_ = 0;
  uint32_t msl_bound_pixel_sampler_count_ = 0;
  uint64_t msl_bound_vertex_texture_binding_uid_ = 0;
  uint64_t msl_bound_pixel_texture_binding_uid_ = 0;
  uint64_t msl_bound_vertex_sampler_binding_uid_ = 0;
  uint64_t msl_bound_pixel_sampler_binding_uid_ = 0;
  std::array<MTL::Texture*, MslTextureIndex::kMaxPerStage>
      msl_bound_vertex_textures_{};
  std::array<MTL::Texture*, MslTextureIndex::kMaxPerStage>
      msl_bound_pixel_textures_{};
  std::array<MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
      msl_bound_vertex_samplers_{};
  std::array<MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
      msl_bound_pixel_samplers_{};
  MTL::Buffer* msl_bound_vertex_argument_buffer_ = nullptr;
  MTL::Buffer* msl_bound_pixel_argument_buffer_ = nullptr;
  NS::UInteger msl_bound_vertex_argument_buffer_offset_ = 0;
  NS::UInteger msl_bound_pixel_argument_buffer_offset_ = 0;
  bool msl_bound_vertex_argument_buffer_offset_valid_ = false;
  bool msl_bound_pixel_argument_buffer_offset_valid_ = false;
  MTL::Buffer* msl_bound_shared_memory_buffer_ = nullptr;
  MTL::Buffer* msl_bound_null_buffer_ = nullptr;
  // Cached argument buffer content for change detection — skip re-encoding
  // when textures/samplers haven't changed between draws.
  std::array<const MTL::Texture*, MslTextureIndex::kMaxPerStage>
      msl_last_argbuf_vertex_textures_{};
  uint32_t msl_last_argbuf_vertex_texture_count_ = 0;
  std::array<const MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
      msl_last_argbuf_vertex_samplers_{};
  uint32_t msl_last_argbuf_vertex_sampler_count_ = 0;
  MTL::Buffer* msl_last_argbuf_vertex_buffer_ = nullptr;
  NS::UInteger msl_last_argbuf_vertex_offset_ = 0;
  const MslShader::MslTranslation* msl_last_argbuf_vertex_translation_ =
      nullptr;
  uint32_t msl_last_argbuf_vertex_encoded_length_ = 0;
  uint64_t msl_last_argbuf_vertex_layout_uid_ = 0;
  std::array<const MTL::Texture*, MslTextureIndex::kMaxPerStage>
      msl_last_argbuf_pixel_textures_{};
  uint32_t msl_last_argbuf_pixel_texture_count_ = 0;
  std::array<const MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
      msl_last_argbuf_pixel_samplers_{};
  uint32_t msl_last_argbuf_pixel_sampler_count_ = 0;
  MTL::Buffer* msl_last_argbuf_pixel_buffer_ = nullptr;
  NS::UInteger msl_last_argbuf_pixel_offset_ = 0;
  const MslShader::MslTranslation* msl_last_argbuf_pixel_translation_ = nullptr;
  uint32_t msl_last_argbuf_pixel_encoded_length_ = 0;
  uint64_t msl_last_argbuf_pixel_layout_uid_ = 0;
  // D3D12-style SPIRV constant cache state.
  std::array<uint64_t, 4> msl_current_float_constant_map_vertex_{};
  std::array<uint64_t, 4> msl_current_float_constant_map_pixel_{};
  bool msl_float_constants_dirty_vertex_ = true;
  bool msl_float_constants_dirty_pixel_ = true;
  bool msl_bool_loop_constants_dirty_ = true;
  bool msl_fetch_constants_dirty_ = true;
  std::array<uint8_t, kCbvSizeBytes> msl_cached_float_constants_vertex_{};
  std::array<uint8_t, kCbvSizeBytes> msl_cached_float_constants_pixel_{};
  std::array<uint8_t, kCbvSizeBytes> msl_cached_bool_loop_constants_{};
  std::array<uint8_t, kCbvSizeBytes> msl_cached_fetch_constants_{};
  // Uniforms slot binding dedupe.
  MTL::Buffer* msl_bound_uniforms_buffer_ = nullptr;
  NS::UInteger msl_bound_uniforms_vs_base_offset_ = 0;
  NS::UInteger msl_bound_uniforms_ps_base_offset_ = 0;
  bool msl_bound_uniforms_offsets_valid_ = false;
  MTL::RenderPipelineState* msl_bound_pipeline_state_ = nullptr;
  bool msl_viewport_valid_ = false;
  MTL::Viewport msl_viewport_ = {};
  bool msl_scissor_valid_ = false;
  MTL::ScissorRect msl_scissor_ = {};
  bool msl_rasterizer_state_valid_ = false;
  MTL::CullMode msl_cull_mode_ = MTL::CullModeNone;
  MTL::Winding msl_winding_ = MTL::WindingClockwise;
  MTL::TriangleFillMode msl_fill_mode_ = MTL::TriangleFillModeFill;
  float msl_depth_bias_constant_ = 0.0f;
  float msl_depth_bias_slope_ = 0.0f;
  float msl_depth_bias_clamp_ = 0.0f;
  MTL::DepthClipMode msl_depth_clip_mode_ = MTL::DepthClipModeClip;
  MTL::DepthStencilState* msl_depth_stencil_state_ = nullptr;
  bool msl_stencil_reference_valid_ = false;
  uint32_t msl_stencil_reference_ = 0;

  // Fixed-function dynamic state cached per render encoder.
  float ff_blend_factor_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  bool ff_blend_factor_valid_ = false;

  std::atomic<uint64_t> completed_command_buffers_{0};
  std::atomic<uint32_t> pending_completion_handlers_{0};
  uint64_t submission_current_ = 0;
  uint64_t submission_completed_processed_ = 0;

  // Draw counter for ring-buffer descriptor heap allocation
  // Each draw uses a different region of the descriptor heap to avoid
  // overwriting previous draws' descriptors before GPU execution
  uint32_t current_draw_index_ = 0;
  bool copy_resolve_writes_pending_ = false;

  // Memexport tracking for shared memory invalidation.
  std::vector<draw_util::MemExportRange> memexport_ranges_;

  bool gamma_ramp_256_entry_table_up_to_date_ = false;
  bool gamma_ramp_pwl_up_to_date_ = false;

  // Track memory regions written by IssueCopy (resolve) during trace playback.
  // This prevents the trace player from overwriting resolved data with stale
  // data from the trace file.
  struct ResolvedRange {
    uint32_t base;
    uint32_t length;
  };
  std::vector<ResolvedRange> resolved_memory_ranges_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_
