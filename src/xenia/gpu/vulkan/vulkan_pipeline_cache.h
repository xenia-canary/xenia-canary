/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_PIPELINE_STATE_CACHE_H_
#define XENIA_GPU_VULKAN_VULKAN_PIPELINE_STATE_CACHE_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/hash.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/primitive_processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

// TODO(Triang3l): Create a common base for both the Vulkan and the Direct3D
// implementations.
class VulkanPipelineCache {
 public:
  class PipelineLayoutProvider {
   public:
    virtual ~PipelineLayoutProvider() {}
    virtual VkPipelineLayout GetPipelineLayout() const = 0;

   protected:
    PipelineLayoutProvider() = default;
  };

  struct Pipeline {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    // The layouts are owned by the VulkanCommandProcessor, and must not be
    // destroyed by it while the pipeline cache is active.
    const PipelineLayoutProvider* pipeline_layout;

    // Placeholder pipeline support for reduced stutter.
    // When true, the current pipeline uses a placeholder pixel shader and
    // the real pipeline is being compiled in the background.
    std::atomic<bool> is_placeholder{false};

    Pipeline(const PipelineLayoutProvider* pipeline_layout_provider)
        : pipeline_layout(pipeline_layout_provider) {}

    // Copy constructor needed for unordered_map
    Pipeline(const Pipeline& other)
        : pipeline(other.pipeline.load(std::memory_order_acquire)),
          pipeline_layout(other.pipeline_layout),
          is_placeholder(other.is_placeholder.load(std::memory_order_acquire)) {
    }

    // Move constructor
    Pipeline(Pipeline&& other) noexcept
        : pipeline(other.pipeline.load(std::memory_order_acquire)),
          pipeline_layout(other.pipeline_layout),
          is_placeholder(other.is_placeholder.load(std::memory_order_acquire)) {
    }

    // Deleted copy assignment to prevent accidental copying
    Pipeline& operator=(const Pipeline&) = delete;

    // Deleted move assignment
    Pipeline& operator=(Pipeline&&) = delete;
  };

  static constexpr size_t kLayoutUIDEmpty = 0;

  VulkanPipelineCache(VulkanCommandProcessor& command_processor,
                      const RegisterFile& register_file,
                      VulkanRenderTargetCache& render_target_cache,
                      VkShaderStageFlags guest_shader_vertex_stages);
  ~VulkanPipelineCache();

  bool Initialize();
  void Shutdown();

  // Shader and pipeline storage.
  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr);
  void ShutdownShaderStorage();

  void EndSubmission();
  bool IsCreatingPipelines();
  // Waits for any pipeline creation needed by the current draw path to finish
  // before state is consumed. This was added so strict ZPD query paths stop
  // racing pipeline compilation and then blocking work on incomplete state.
  void AwaitPipelineCompletion();

  VulkanShader* LoadShader(xenos::ShaderType shader_type,
                           const uint32_t* host_address, uint32_t dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) {
    shader.AnalyzeUcode(ucode_disasm_buffer_);
  }

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed.
  SpirvShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, bool ps_param_gen_used) const;
  SpirvShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask,
      uint32_t param_gen_pos) const;

  bool EnsureShadersTranslated(VulkanShader::VulkanTranslation* vertex_shader,
                               VulkanShader::VulkanTranslation* pixel_shader);
  bool ConfigurePipeline(
      VulkanShader::VulkanTranslation* vertex_shader,
      VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key,
      Pipeline** pipeline_out);

 private:
  enum class PipelineGeometryShader : uint32_t {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };

  enum class PipelinePrimitiveTopology : uint32_t {
    kPointList,
    kLineList,
    kLineStrip,
    kTriangleList,
    kTriangleStrip,
    kTriangleFan,
    kLineListWithAdjacency,
    kPatchList,
  };

  enum class PipelinePolygonMode : uint32_t {
    kFill,
    kLine,
    kPoint,
  };

  // Tessellation mode for pipeline creation.
  // Must match the TCS (hull shader) selection logic.
  enum class PipelineTessellationMode : uint32_t {
    kNone,
    kDiscrete,    // Integer tessellation factors.
    kContinuous,  // Fractional (fractional_even) tessellation factors.
    kAdaptive,    // Per-edge factors from index buffer.
  };

  // Tessellation primitive type.
  enum class PipelineTessellationPatchType : uint32_t {
    kNone,
    kTriangle,
    kQuad,
  };

  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kOneMinusSrcColor,
    kDstColor,
    kOneMinusDstColor,
    kSrcAlpha,
    kOneMinusSrcAlpha,
    kDstAlpha,
    kOneMinusDstAlpha,
    kConstantColor,
    kOneMinusConstantColor,
    kConstantAlpha,
    kOneMinusConstantAlpha,
    kSrcAlphaSaturate,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  XEPACKEDSTRUCT(PipelineRenderTarget, {
    PipelineBlendFactor src_color_blend_factor : 4;  // 4
    PipelineBlendFactor dst_color_blend_factor : 4;  // 8
    xenos::BlendOp color_blend_op : 3;               // 11
    PipelineBlendFactor src_alpha_blend_factor : 4;  // 15
    PipelineBlendFactor dst_alpha_blend_factor : 4;  // 19
    xenos::BlendOp alpha_blend_op : 3;               // 22
    uint32_t color_write_mask : 4;                   // 26
  });

  XEPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 if no pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;
    VulkanRenderTargetCache::RenderPassKey render_pass_key;

    // Shader stages.
    PipelineGeometryShader geometry_shader : 2;            // 2
    PipelineTessellationMode tessellation_mode : 2;        // 4
    PipelineTessellationPatchType tessellation_patch : 2;  // 6
    // Input assembly.
    PipelinePrimitiveTopology primitive_topology : 3;  // 9
    uint32_t primitive_restart : 1;                    // 10
    // Rasterization.
    uint32_t depth_clamp_enable : 1;       // 7
    PipelinePolygonMode polygon_mode : 2;  // 9
    uint32_t cull_front : 1;               // 10
    uint32_t cull_back : 1;                // 11
    uint32_t front_face_clockwise : 1;     // 12
    // Depth / stencil.
    uint32_t depth_write_enable : 1;                      // 13
    xenos::CompareFunction depth_compare_op : 3;          // 15
    uint32_t stencil_test_enable : 1;                     // 17
    xenos::StencilOp stencil_front_fail_op : 3;           // 20
    xenos::StencilOp stencil_front_pass_op : 3;           // 23
    xenos::StencilOp stencil_front_depth_fail_op : 3;     // 26
    xenos::CompareFunction stencil_front_compare_op : 3;  // 29
    xenos::StencilOp stencil_back_fail_op : 3;            // 32

    xenos::StencilOp stencil_back_pass_op : 3;           // 3
    xenos::StencilOp stencil_back_depth_fail_op : 3;     // 6
    xenos::CompareFunction stencil_back_compare_op : 3;  // 9

    // Filled only for the attachments present in the render pass object.
    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    // Including all the padding, for a stable hash.
    PipelineDescription() { Reset(); }
    PipelineDescription(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
    }
    PipelineDescription& operator=(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
      return *this;
    }
    bool operator==(const PipelineDescription& description) const {
      return std::memcmp(this, &description, sizeof(*this)) == 0;
    }
    void Reset() { std::memset(this, 0, sizeof(*this)); }
    uint64_t GetHash() const { return XXH3_64bits(this, sizeof(*this)); }
    struct Hasher {
      size_t operator()(const PipelineDescription& description) const {
        return size_t(description.GetHash());
      }
    };

    static constexpr uint32_t kVersion = 0x20250118;
  });

  // Pipeline storage constants.
  static constexpr uint32_t kPipelineStorageVersionWithoutAPI = 0x20201219;
  static constexpr uint32_t kPipelineStorageAPIMagicVulkan = 'VLKN';

  // Pipeline storage description.
  XEPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

  // creation threads, with everything needed from caches pre-looked-up.
  struct PipelineCreationArguments {
    std::pair<const PipelineDescription, Pipeline>* pipeline;
    VulkanShader::VulkanTranslation* vertex_shader;
    VulkanShader::VulkanTranslation* pixel_shader;
    VkShaderModule geometry_shader;
    // Tessellation shaders (only used when tessellation is active).
    VkShaderModule tessellation_vertex_shader;   // VS for passing data to TCS.
    VkShaderModule tessellation_control_shader;  // TCS (hull shader).
    VkRenderPass render_pass;
    // Priority for async compilation (higher = compiled sooner).
    // Pipelines that write to visible render targets get higher priority.
    uint8_t priority = 0;
  };

  // Comparator for priority queue - higher priority first.
  struct PipelineCreationPriorityCompare {
    bool operator()(const PipelineCreationArguments& a,
                    const PipelineCreationArguments& b) const {
      return a.priority < b.priority;  // max-heap: lower priority at bottom
    }
  };

  union GeometryShaderKey {
    uint32_t key;
    struct {
      PipelineGeometryShader type : 2;
      uint32_t interpolator_count : 5;
      uint32_t has_user_clip_planes : 1;
      uint32_t user_clip_plane_cull : 1;
      uint32_t has_vertex_kill_and : 1;
      uint32_t has_point_size : 1;
      uint32_t has_point_coordinates : 1;
    };

    GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const GeometryShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const GeometryShaderKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const GeometryShaderKey& other_key) const {
      return !(*this == other_key);
    }
  };

  // Can be called from multiple threads.
  bool TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                               VulkanShader::VulkanTranslation& translation);

  // Translates shaders in parallel for storage loading.
  void TranslateShadersForStorage(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
      bool edram_fsi_used);

  void WritePipelineRenderTargetDescription(
      reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
      PipelineRenderTarget& render_target_out) const;
  bool GetCurrentStateDescription(
      const VulkanShader::VulkanTranslation* vertex_shader,
      const VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key,
      PipelineDescription& description_out) const;

  // Whether the pipeline for the given description is supported by the device.
  bool ArePipelineRequirementsMet(const PipelineDescription& description) const;

  static bool GetGeometryShaderKey(
      PipelineGeometryShader geometry_shader_type,
      SpirvShaderTranslator::Modification vertex_shader_modification,
      SpirvShaderTranslator::Modification pixel_shader_modification,
      GeometryShaderKey& key_out);
  VkShaderModule GetGeometryShader(GeometryShaderKey key);

  // Get the appropriate tessellation control shader (hull shader) module.
  VkShaderModule GetTessellationControlShader(
      PipelineTessellationMode mode, PipelineTessellationPatchType patch_type,
      bool use_control_point_count) const;

  // Get the appropriate tessellation vertex shader module.
  VkShaderModule GetTessellationVertexShader(
      PipelineTessellationMode mode) const;

  // Can be called from creation threads - all needed data must be fully set up
  // at the point of the call: shaders must be translated, pipeline layout and
  // render pass objects must be available.
  // If fragment_shader_override is not VK_NULL_HANDLE, it is used instead of
  // the pixel shader from creation_arguments (for placeholder pipelines).
  bool EnsurePipelineCreated(
      const PipelineCreationArguments& creation_arguments,
      VkShaderModule fragment_shader_override = VK_NULL_HANDLE);

  // Creates a placeholder pipeline using the placeholder pixel shader.
  // Used for pipeline hot-swap to reduce stutter.
  bool EnsurePipelineCreatedWithPlaceholder(
      const PipelineCreationArguments& creation_arguments) {
    return EnsurePipelineCreated(creation_arguments, placeholder_pixel_shader_);
  }

  VulkanCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  VulkanRenderTargetCache& render_target_cache_;
  VkShaderStageFlags guest_shader_vertex_stages_;

  // Temporary storage for AnalyzeUcode calls on the processor thread.
  StringBuffer ucode_disasm_buffer_;
  // Reusable shader translator on the command processor thread.
  std::unique_ptr<SpirvShaderTranslator> shader_translator_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  // Texture binding layouts of different shaders, for obtaining layout UIDs.
  std::vector<VulkanShader::TextureBinding> texture_binding_layouts_;
  // Map of texture binding layouts used by shaders, for obtaining UIDs. Keys
  // are XXH3 hashes of layouts, values need manual collision resolution using
  // layout_vector_offset:layout_length of texture_binding_layouts_.
  std::unordered_multimap<uint64_t, LayoutUID,
                          xe::hash::IdentityHasher<uint64_t>>
      texture_binding_layout_map_;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, VulkanShader*,
                     xe::hash::IdentityHasher<uint64_t>>
      shaders_;

  // Geometry shaders for Xenos primitive types not supported by Vulkan.
  // Stores VK_NULL_HANDLE if failed to create.
  std::unordered_map<GeometryShaderKey, VkShaderModule,
                     GeometryShaderKey::Hasher>
      geometry_shaders_;

  // Empty depth-only pixel shader for writing to depth buffer using fragment
  // shader interlock when no Xenos pixel shader provided.
  VkShaderModule depth_only_fragment_shader_ = VK_NULL_HANDLE;

  // Placeholder pixel shader for pipeline hot-swap to reduce stutter.
  // Outputs transparent black while the real shader compiles in background.
  VkShaderModule placeholder_pixel_shader_ = VK_NULL_HANDLE;

  // Tessellation shaders.
  // Vertex shaders for tessellation - pass indices/factors to TCS.
  VkShaderModule tessellation_indexed_vs_ = VK_NULL_HANDLE;
  VkShaderModule tessellation_adaptive_vs_ = VK_NULL_HANDLE;
  // Tessellation control shaders (hull shaders) for different modes and
  // primitive types.
  // Discrete mode (integer tessellation factors).
  VkShaderModule discrete_triangle_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule discrete_triangle_3cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule discrete_quad_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule discrete_quad_4cp_hs_ = VK_NULL_HANDLE;
  // Continuous mode (fractional_even tessellation factors).
  VkShaderModule continuous_triangle_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule continuous_triangle_3cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule continuous_quad_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule continuous_quad_4cp_hs_ = VK_NULL_HANDLE;
  // Adaptive mode (per-edge factors from index buffer).
  VkShaderModule adaptive_triangle_hs_ = VK_NULL_HANDLE;
  VkShaderModule adaptive_quad_hs_ = VK_NULL_HANDLE;

  // Vulkan pipeline cache for faster pipeline creation.
  VkPipelineCache vk_pipeline_cache_ = VK_NULL_HANDLE;

  std::unordered_map<PipelineDescription, Pipeline, PipelineDescription::Hasher>
      pipelines_;

  // Previously used pipeline, to avoid lookups if the state wasn't changed.
  std::pair<const PipelineDescription, Pipeline>* last_pipeline_ = nullptr;

  void CreationThread();

  // For asynchronous creation.
  std::vector<std::unique_ptr<xe::threading::Thread>> creation_threads_;
  std::atomic<bool> creation_threads_shutdown_{false};
  std::atomic<size_t> creation_threads_busy_{0};
  // Priority queue contains pointers to map entries. Pipelines are never
  // evicted as games have a finite set that should all remain cached for
  // performance. Higher priority pipelines (those writing to visible RTs)
  // are compiled first.
  std::priority_queue<PipelineCreationArguments,
                      std::vector<PipelineCreationArguments>,
                      PipelineCreationPriorityCompare>
      creation_queue_;
  std::mutex creation_request_lock_;
  std::condition_variable creation_request_cond_;
  std::unique_ptr<xe::threading::Event> creation_completion_event_ = nullptr;
  std::atomic<bool> creation_completion_set_event_{false};
  std::function<void()> creation_completion_callback_;
  // During startup loading, don't block on pipeline creation to allow game
  // boot.
  bool startup_loading_ = false;

  // Deferred destruction of replaced shader modules and pipelines.
  // Pipelines are only destroyed after the GPU submission that might reference
  // them has completed (tracked via submission numbers from command processor).
  void ProcessDeferredDestructions();
  std::vector<VkShaderModule> deferred_destroy_shader_modules_;
  // Pipelines pending destruction, paired with the submission number they were
  // last potentially used in. Only destroyed when that submission completes.
  std::vector<std::pair<VkPipeline, uint64_t>> deferred_destroy_pipelines_;
  std::mutex deferred_destroy_mutex_;

  // Shader and pipeline storage.
  uint32_t shader_storage_title_id_ = 0;
  std::atomic<bool> shader_storage_file_flush_needed_{false};
  std::atomic<bool> pipeline_storage_file_flush_needed_{false};

  // Storage writer for shaders and pipelines (owns file handles and storage
  // index).
  ShaderStorageWriter<PipelineStoredDescription> storage_writer_;

  // VkPipelineCache persistence path.
  std::filesystem::path vk_pipeline_cache_path_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_PIPELINE_STATE_CACHE_H_
