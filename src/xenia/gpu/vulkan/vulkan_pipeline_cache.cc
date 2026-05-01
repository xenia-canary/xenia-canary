/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"

#include <cstdint>
#include <cstring>
#include <set>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/pipeline_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/vulkan_util.h"

// Shader bytecode.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/adaptive_quad_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/adaptive_triangle_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/tessellation_adaptive_vs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/tessellation_indexed_vs.h"
// Placeholder pixel shader for pipeline hot-swap.
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_ps.h"
}  // namespace shaders

DEFINE_int32(
    vulkan_pipeline_creation_threads, -1,
    "Number of threads used for graphics pipeline creation. -1 to calculate "
    "automatically (75% of logical CPU cores), a positive number to specify "
    "the number of threads explicitly (up to the number of logical CPU cores), "
    "0 to disable multithreaded pipeline creation.",
    "Vulkan");
namespace xe {
namespace gpu {
namespace vulkan {

VulkanPipelineCache::VulkanPipelineCache(
    VulkanCommandProcessor& command_processor,
    const RegisterFile& register_file,
    VulkanRenderTargetCache& render_target_cache,
    VkShaderStageFlags guest_shader_vertex_stages)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      guest_shader_vertex_stages_(guest_shader_vertex_stages) {}

VulkanPipelineCache::~VulkanPipelineCache() { Shutdown(); }

bool VulkanPipelineCache::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;

  shader_translator_ = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(vulkan_device),
      render_target_cache_.msaa_2x_attachments_supported(),
      render_target_cache_.msaa_2x_no_attachments_supported(),
      edram_fragment_shader_interlock,
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y());

  if (edram_fragment_shader_interlock) {
    std::vector<uint8_t> depth_only_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader();
    depth_only_fragment_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device,
        reinterpret_cast<const uint32_t*>(
            depth_only_fragment_shader_code.data()),
        depth_only_fragment_shader_code.size());
    if (depth_only_fragment_shader_ == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanPipelineCache: Failed to create the depth/stencil-only "
          "fragment shader for the fragment shader interlock render backend "
          "implementation");
      return false;
    }
  }

  // Create tessellation shaders if tessellation is supported.
  if (vulkan_device->properties().tessellationShader) {
    // Vertex shaders for tessellation.
    tessellation_indexed_vs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::tessellation_indexed_vs,
        sizeof(shaders::tessellation_indexed_vs));
    tessellation_adaptive_vs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::tessellation_adaptive_vs,
        sizeof(shaders::tessellation_adaptive_vs));
    // Discrete mode hull shaders.
    discrete_triangle_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_triangle_1cp_hs,
        sizeof(shaders::discrete_triangle_1cp_hs));
    discrete_triangle_3cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_triangle_3cp_hs,
        sizeof(shaders::discrete_triangle_3cp_hs));
    discrete_quad_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_quad_1cp_hs,
        sizeof(shaders::discrete_quad_1cp_hs));
    discrete_quad_4cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_quad_4cp_hs,
        sizeof(shaders::discrete_quad_4cp_hs));
    // Continuous mode hull shaders.
    continuous_triangle_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_triangle_1cp_hs,
        sizeof(shaders::continuous_triangle_1cp_hs));
    continuous_triangle_3cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_triangle_3cp_hs,
        sizeof(shaders::continuous_triangle_3cp_hs));
    continuous_quad_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_quad_1cp_hs,
        sizeof(shaders::continuous_quad_1cp_hs));
    continuous_quad_4cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_quad_4cp_hs,
        sizeof(shaders::continuous_quad_4cp_hs));
    // Adaptive mode hull shaders.
    adaptive_triangle_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::adaptive_triangle_hs,
        sizeof(shaders::adaptive_triangle_hs));
    adaptive_quad_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::adaptive_quad_hs,
        sizeof(shaders::adaptive_quad_hs));

    // Verify all tessellation shaders were created successfully.
    if (tessellation_indexed_vs_ == VK_NULL_HANDLE ||
        tessellation_adaptive_vs_ == VK_NULL_HANDLE ||
        discrete_triangle_1cp_hs_ == VK_NULL_HANDLE ||
        discrete_triangle_3cp_hs_ == VK_NULL_HANDLE ||
        discrete_quad_1cp_hs_ == VK_NULL_HANDLE ||
        discrete_quad_4cp_hs_ == VK_NULL_HANDLE ||
        continuous_triangle_1cp_hs_ == VK_NULL_HANDLE ||
        continuous_triangle_3cp_hs_ == VK_NULL_HANDLE ||
        continuous_quad_1cp_hs_ == VK_NULL_HANDLE ||
        continuous_quad_4cp_hs_ == VK_NULL_HANDLE ||
        adaptive_triangle_hs_ == VK_NULL_HANDLE ||
        adaptive_quad_hs_ == VK_NULL_HANDLE) {
      XELOGW(
          "VulkanPipelineCache: Failed to create one or more tessellation "
          "shaders - tessellation will not be available");
    }
  }

  // Create placeholder pixel shader for pipeline hot-swap (stutter reduction).
  placeholder_pixel_shader_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::placeholder_ps, sizeof(shaders::placeholder_ps));
  if (placeholder_pixel_shader_ == VK_NULL_HANDLE) {
    XELOGW(
        "VulkanPipelineCache: Failed to create placeholder pixel shader - "
        "pipeline hot-swap will not be available");
  }

  // Create Vulkan pipeline cache for faster pipeline creation.
  VkPipelineCacheCreateInfo pipeline_cache_create_info = {};
  pipeline_cache_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  if (vulkan_device->functions().vkCreatePipelineCache(
          vulkan_device->device(), &pipeline_cache_create_info, nullptr,
          &vk_pipeline_cache_) != VK_SUCCESS) {
    XELOGW("VulkanPipelineCache: Failed to create pipeline cache");
    vk_pipeline_cache_ = VK_NULL_HANDLE;
  }

  uint32_t logical_processor_count = xe::threading::logical_processor_count();
  if (!logical_processor_count) {
    // Pick some reasonable amount if couldn't determine the number of cores.
    logical_processor_count = 6;
  }
  creation_completion_event_ =
      xe::threading::Event::CreateManualResetEvent(true);
  assert_not_null(creation_completion_event_);
  if (cvars::vulkan_pipeline_creation_threads != 0) {
    size_t creation_thread_count;
    if (cvars::vulkan_pipeline_creation_threads < 0) {
      creation_thread_count =
          std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count =
          std::min(uint32_t(cvars::vulkan_pipeline_creation_threads),
                   logical_processor_count);
    }
    creation_threads_shutdown_ = false;
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<xe::threading::Thread> creation_thread =
          xe::threading::Thread::Create({}, [this]() { CreationThread(); });
      assert_not_null(creation_thread);
      creation_thread->set_name("Vulkan Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }
  }
  return true;
}

void VulkanPipelineCache::Shutdown() {
  // Shut down shader storage first.
  ShutdownShaderStorage();

  // Shut down all threads, before destroying the pipelines since they may be
  // creating them.
  if (!creation_threads_.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_threads_shutdown_ = true;
    }
    creation_request_cond_.notify_all();
    for (size_t i = 0; i < creation_threads_.size(); ++i) {
      xe::threading::Wait(creation_threads_[i].get(), false);
    }
    creation_threads_.clear();
  }
  // Clear any pending completion callback (may capture 'this') and reset
  // startup state.
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    creation_completion_callback_ = nullptr;
  }
  startup_loading_ = false;
  creation_completion_event_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Process any remaining deferred destructions (force destroy all since
  // device should be idle at shutdown).
  {
    std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
    for (VkShaderModule module : deferred_destroy_shader_modules_) {
      if (module != VK_NULL_HANDLE) {
        dfn.vkDestroyShaderModule(device, module, nullptr);
      }
    }
    deferred_destroy_shader_modules_.clear();
    for (const auto& pipeline_pair : deferred_destroy_pipelines_) {
      if (pipeline_pair.first != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, pipeline_pair.first, nullptr);
      }
    }
    deferred_destroy_pipelines_.clear();
  }

  // Destroy all pipelines.
  last_pipeline_ = nullptr;
  for (const auto& pipeline_pair : pipelines_) {
    if (pipeline_pair.second.pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline_pair.second.pipeline, nullptr);
    }
  }
  pipelines_.clear();

  // Destroy the pipeline cache.
  if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineCache(device, vk_pipeline_cache_, nullptr);
    vk_pipeline_cache_ = VK_NULL_HANDLE;
  }

  // Destroy all internal shaders.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         depth_only_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         placeholder_pixel_shader_);
  // Destroy tessellation shaders.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         tessellation_indexed_vs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         tessellation_adaptive_vs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_triangle_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_triangle_3cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_quad_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_quad_4cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_triangle_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_triangle_3cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_quad_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_quad_4cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         adaptive_triangle_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         adaptive_quad_hs_);
  for (const auto& geometry_shader_pair : geometry_shaders_) {
    if (geometry_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, geometry_shader_pair.second, nullptr);
    }
  }
  geometry_shaders_.clear();

  // Destroy all translated shaders.
  for (auto it : shaders_) {
    delete it.second;
  }
  shaders_.clear();
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();

  // Shut down shader translation.
  shader_translator_.reset();
}

VulkanShader* VulkanPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                              const uint32_t* host_address,
                                              uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  uint64_t data_hash =
      XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }
  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  VulkanShader* shader =
      new VulkanShader(command_processor_.GetVulkanDevice(), shader_type,
                       data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

SpirvShaderTranslator::Modification
VulkanPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();

  SpirvShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  // User clip planes.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);

  if (host_vertex_shader_type ==
      Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                     xenos::PrimitiveType::kPointList);
  }

  return modification;
}

SpirvShaderTranslator::Modification
VulkanPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask,
    uint32_t param_gen_pos) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  SpirvShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
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

  if (render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
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

    // Check if MIN/MAX blend is used with non-trivial source factors.
    // Vulkan/D3D12 fixed-function blend ignores factors for MIN/MAX, but
    // Xbox 360 applies them. If the destination factor is ONE (or ZERO), we can
    // pre-multiply the shader output by the source factor to emulate this.
    // Only RT0 is supported for now.
    modification.pixel.rt0_blend_rgb_factor_for_premult =
        xenos::BlendFactor::kOne;
    modification.pixel.rt0_blend_a_factor_for_premult =
        xenos::BlendFactor::kOne;

    if (shader.writes_color_target(0)) {
      auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
          reg::RB_BLENDCONTROL::rt_register_indices[0]);

      // Pre-multiply by kSrcAlpha for MIN/MAX blend ops when dstFactor is ONE.
      if ((blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.color_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_rgb_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }

      if ((blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.alpha_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_a_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }
    }
  }

  return modification;
}

bool VulkanPipelineCache::EnsureShadersTranslated(
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader) {
  // Edge flags are not supported yet (because polygon primitives are not).
  assert_true(register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
              register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  assert_false(register_file_.Get<reg::SQ_PROGRAM_CNTL>().gen_index_vtx);
  if (!vertex_shader->is_translated()) {
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
    if (!TranslateAnalyzedShader(*shader_translator_, *vertex_shader)) {
      XELOGE("Failed to translate the vertex shader!");
      return false;
    }
  }
  if (!vertex_shader->is_valid()) {
    // Translation attempted previously, but not valid.
    return false;
  }
  if (pixel_shader != nullptr) {
    if (!pixel_shader->is_translated()) {
      pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
      if (!TranslateAnalyzedShader(*shader_translator_, *pixel_shader)) {
        XELOGE("Failed to translate the pixel shader!");
        return false;
      }
    }
    if (!pixel_shader->is_valid()) {
      // Translation attempted previously, but not valid.
      return false;
    }
  }
  return true;
}

bool VulkanPipelineCache::ConfigurePipeline(
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key,
    VulkanPipelineCache::Pipeline** pipeline_out) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  PipelineDescription description;
  if (!GetCurrentStateDescription(
          vertex_shader, pixel_shader, primitive_processing_result,
          normalized_depth_control, normalized_color_mask, render_pass_key,
          description)) {
    return false;
  }
  if (last_pipeline_ && last_pipeline_->first == description) {
    *pipeline_out = &last_pipeline_->second;
    return true;
  }
  auto it = pipelines_.find(description);
  if (it != pipelines_.end()) {
    last_pipeline_ = &*it;
    *pipeline_out = &it->second;
    return true;
  }

  // Create the pipeline if not the latest and not already existing.
  const PipelineLayoutProvider* pipeline_layout =
      command_processor_.GetPipelineLayout(
          pixel_shader
              ? static_cast<const VulkanShader&>(pixel_shader->shader())
                    .GetTextureBindingsAfterTranslation()
                    .size()
              : 0,
          pixel_shader
              ? static_cast<const VulkanShader&>(pixel_shader->shader())
                    .GetSamplerBindingsAfterTranslation()
                    .size()
              : 0,
          static_cast<const VulkanShader&>(vertex_shader->shader())
              .GetTextureBindingsAfterTranslation()
              .size(),
          static_cast<const VulkanShader&>(vertex_shader->shader())
              .GetSamplerBindingsAfterTranslation()
              .size());
  if (!pipeline_layout) {
    return false;
  }

  VkShaderModule geometry_shader = VK_NULL_HANDLE;
  if (description.geometry_shader != PipelineGeometryShader::kNone) {
    GeometryShaderKey geometry_shader_key;
    GetGeometryShaderKey(
        description.geometry_shader,
        SpirvShaderTranslator::Modification(vertex_shader->modification()),
        SpirvShaderTranslator::Modification(
            pixel_shader ? pixel_shader->modification() : 0),
        geometry_shader_key);
    geometry_shader = GetGeometryShader(geometry_shader_key);
    if (geometry_shader == VK_NULL_HANDLE) {
      return false;
    }
  }

  VkRenderPass render_pass =
      render_target_cache_.GetPath() ==
              RenderTargetCache::Path::kPixelShaderInterlock
          ? render_target_cache_.GetFragmentShaderInterlockRenderPass()
          : render_target_cache_.GetHostRenderTargetsRenderPass(
                render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return false;
  }

  auto& pipeline_pair =
      *pipelines_.emplace(description, Pipeline(pipeline_layout)).first;

  if (storage_writer_.is_active()) {
    VulkanShader& vs = static_cast<VulkanShader&>(vertex_shader->shader());
    if (vs.try_set_ucode_storage_index(storage_writer_.storage_index())) {
      shader_storage_file_flush_needed_ = true;
      storage_writer_.QueueShaderWrite(&vs);
    }
    if (pixel_shader) {
      VulkanShader& ps = static_cast<VulkanShader&>(pixel_shader->shader());
      if (ps.try_set_ucode_storage_index(storage_writer_.storage_index())) {
        shader_storage_file_flush_needed_ = true;
        storage_writer_.QueueShaderWrite(&ps);
      }
    }

    pipeline_storage_file_flush_needed_ = true;
    PipelineStoredDescription stored_description;
    stored_description.description_hash = description.GetHash();
    std::memcpy(&stored_description.description, &description,
                sizeof(description));
    storage_writer_.QueuePipelineWrite(stored_description);
  }

  // Get tessellation shaders if needed.
  VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
  VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
  if (description.tessellation_mode != PipelineTessellationMode::kNone) {
    tessellation_vertex_shader =
        GetTessellationVertexShader(description.tessellation_mode);
    bool use_control_point_count =
        (description.tessellation_mode == PipelineTessellationMode::kAdaptive);
    tessellation_control_shader = GetTessellationControlShader(
        description.tessellation_mode, description.tessellation_patch,
        use_control_point_count);
    if (tessellation_vertex_shader == VK_NULL_HANDLE ||
        tessellation_control_shader == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanPipelineCache: Failed to get tessellation shaders for mode {} "
          "patch {}",
          static_cast<uint32_t>(description.tessellation_mode),
          static_cast<uint32_t>(description.tessellation_patch));
      return false;
    }
  }

  // Pipeline hot-swap: When async mode is enabled, we have creation threads and
  // a pixel shader, create a placeholder pipeline immediately (fast compile)
  // and queue the real pipeline creation in the background. This reduces
  // stutter from pipeline compilation.
  bool use_async = cvars::async_shader_compilation &&
                   !creation_threads_.empty() && pixel_shader &&
                   placeholder_pixel_shader_ != VK_NULL_HANDLE;

  if (use_async) {
    // Create placeholder pipeline immediately (uses simple PS, fast compile).
    // Set is_placeholder BEFORE creating the pipeline to avoid race condition
    // with the creation thread checking this flag.
    pipeline_pair.second.is_placeholder.store(true, std::memory_order_release);

    PipelineCreationArguments placeholder_args;
    placeholder_args.pipeline = &pipeline_pair;
    placeholder_args.vertex_shader = vertex_shader;
    placeholder_args.pixel_shader = nullptr;  // Will use placeholder PS
    placeholder_args.geometry_shader = geometry_shader;
    placeholder_args.tessellation_vertex_shader = tessellation_vertex_shader;
    placeholder_args.tessellation_control_shader = tessellation_control_shader;
    placeholder_args.render_pass = render_pass;

    if (EnsurePipelineCreatedWithPlaceholder(placeholder_args)) {
      // Queue real pipeline creation in background.
      // Calculate priority based on whether shader writes to visible RTs.
      uint8_t priority = 0;
      if (pixel_shader) {
        uint32_t bound_rts =
            pipeline_util::GetBoundRTMaskFromNormalizedColorMask(
                normalized_color_mask);
        priority = pipeline_util::CalculatePipelinePriority(
            bound_rts, pixel_shader->shader().writes_color_targets(),
            pixel_shader->shader().writes_depth());
      }

      {
        std::lock_guard<std::mutex> lock(creation_request_lock_);
        PipelineCreationArguments creation_arguments;
        creation_arguments.pipeline = &pipeline_pair;
        creation_arguments.vertex_shader = vertex_shader;
        creation_arguments.pixel_shader = pixel_shader;
        creation_arguments.geometry_shader = geometry_shader;
        creation_arguments.tessellation_vertex_shader =
            tessellation_vertex_shader;
        creation_arguments.tessellation_control_shader =
            tessellation_control_shader;
        creation_arguments.render_pass = render_pass;
        creation_arguments.priority = priority;
        creation_queue_.push(creation_arguments);
      }
      creation_request_cond_.notify_one();
    } else {
      // Placeholder creation failed, fall back to sync creation.
      // Reset the flag we set earlier.
      pipeline_pair.second.is_placeholder.store(false,
                                                std::memory_order_release);
      use_async = false;
    }
  }

  if (!use_async) {
    // Sync mode or no creation threads: create synchronously.
    PipelineCreationArguments creation_arguments;
    creation_arguments.pipeline = &pipeline_pair;
    creation_arguments.vertex_shader = vertex_shader;
    creation_arguments.pixel_shader = pixel_shader;
    creation_arguments.geometry_shader = geometry_shader;
    creation_arguments.tessellation_vertex_shader = tessellation_vertex_shader;
    creation_arguments.tessellation_control_shader =
        tessellation_control_shader;
    creation_arguments.render_pass = render_pass;
    if (!EnsurePipelineCreated(creation_arguments)) {
      return false;
    }
  }

  last_pipeline_ = &pipeline_pair;
  *pipeline_out = &pipeline_pair.second;
  return true;
}

void VulkanPipelineCache::EndSubmission() {
  if (shader_storage_file_flush_needed_ ||
      pipeline_storage_file_flush_needed_) {
    storage_writer_.RequestFlush(shader_storage_file_flush_needed_,
                                 pipeline_storage_file_flush_needed_);
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (creation_threads_.empty()) {
    // Process deferred destructions when GPU is idle
    ProcessDeferredDestructions();
    return;
  }

  if (startup_loading_) {
    // Non-blocking: let background threads work asynchronously.
    creation_request_cond_.notify_one();
  } else {
    // Blocking: wait for all queued pipelines.
    bool await_creation_completion_event;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      await_creation_completion_event =
          !creation_queue_.empty() || creation_threads_busy_ != 0;
      if (await_creation_completion_event) {
        creation_completion_event_->Reset();
        creation_completion_set_event_.store(true, std::memory_order_release);
      }
    }
    if (await_creation_completion_event) {
      creation_request_cond_.notify_one();
      xe::threading::Wait(creation_completion_event_.get(), false);
    }
  }

  // Process deferred destructions
  ProcessDeferredDestructions();
}

bool VulkanPipelineCache::IsCreatingPipelines() {
  if (creation_threads_.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(creation_request_lock_);
  return !creation_queue_.empty() || creation_threads_busy_ != 0;
}

void VulkanPipelineCache::AwaitPipelineCompletion() {
  if (creation_threads_.empty()) {
    return;
  }

  bool await_creation_completion_event;
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    await_creation_completion_event =
        !creation_queue_.empty() || creation_threads_busy_ != 0;
    if (await_creation_completion_event) {
      creation_completion_event_->Reset();
      creation_completion_set_event_.store(true, std::memory_order_release);
    }
  }

  if (await_creation_completion_event) {
    creation_request_cond_.notify_one();
    xe::threading::Wait(creation_completion_event_.get(), false);
  }
}

void VulkanPipelineCache::CreationThread() {
  for (;;) {
    PipelineCreationArguments creation_arguments;
    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      creation_request_cond_.wait(lock, [this]() {
        return !creation_queue_.empty() || creation_threads_shutdown_;
      });
      if (creation_threads_shutdown_) {
        break;
      }
      creation_arguments = creation_queue_.top();
      creation_queue_.pop();
      ++creation_threads_busy_;
    }

    if (!EnsureShadersTranslated(creation_arguments.vertex_shader,
                                 creation_arguments.pixel_shader)) {
      XELOGE("Failed to translate shaders for pipeline creation");
    } else if (!EnsurePipelineCreated(creation_arguments)) {
      XELOGE("Failed to create Vulkan pipeline");
    }
    // On failure: if a placeholder exists it will remain in use permanently.
    // Clear the flag so we're not in a misleading "waiting for real" state.
    if (creation_arguments.pipeline->second.is_placeholder.load(
            std::memory_order_acquire)) {
      XELOGW(
          "Real pipeline creation failed - placeholder will remain in use "
          "(may cause visual artifacts)");
      creation_arguments.pipeline->second.is_placeholder.store(
          false, std::memory_order_release);
    }

    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      --creation_threads_busy_;
      if (creation_threads_busy_ == 0 && creation_queue_.empty()) {
        // All pipelines created.
        if (creation_completion_set_event_.load(std::memory_order_acquire)) {
          // Signal the event (blocking mode).
          creation_completion_set_event_.store(false,
                                               std::memory_order_release);
          creation_completion_event_->Set();
        }
        if (creation_completion_callback_) {
          // Invoke completion callback (non-blocking mode).
          auto callback = std::move(creation_completion_callback_);
          creation_completion_callback_ = nullptr;
          lock.unlock();
          callback();
          lock.lock();
        }
      }
    }
  }
}

bool VulkanPipelineCache::TranslateAnalyzedShader(
    SpirvShaderTranslator& translator,
    VulkanShader::VulkanTranslation& translation) {
  VulkanShader& shader = static_cast<VulkanShader&>(translation.shader());

  // Perform translation.
  // If this fails the shader will be marked as invalid and ignored later.
  if (!translator.TranslateAnalyzedShader(translation)) {
    XELOGE("Shader {:016X} translation failed; marking as ignored",
           shader.ucode_data_hash());
    return false;
  }
  if (translation.GetOrCreateShaderModule() == VK_NULL_HANDLE) {
    return false;
  }

  // TODO(Triang3l): Log that the shader has been successfully translated in
  // common code.

  // Set up the texture binding layout.
  if (shader.EnterBindingLayoutUserUIDSetup()) {
    // Obtain the unique IDs of the binding layout if there are any texture
    // bindings, for invalidation in the command processor.
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    const std::vector<VulkanShader::TextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    if (texture_binding_count) {
      size_t texture_binding_layout_bytes =
          texture_binding_count * sizeof(*texture_bindings.data());
      uint64_t texture_binding_layout_hash =
          XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
      auto found_range =
          texture_binding_layout_map_.equal_range(texture_binding_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length == texture_binding_count &&
            !std::memcmp(
                texture_binding_layouts_.data() + it->second.vector_span_offset,
                texture_bindings.data(), texture_binding_layout_bytes)) {
          texture_binding_layout_uid = it->second.uid;
          break;
        }
      }
      if (texture_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(
            kLayoutUIDEmpty == 0,
            "Layout UID is size + 1 because it's assumed that 0 is the UID for "
            "an empty layout");
        texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
        LayoutUID new_uid;
        new_uid.uid = texture_binding_layout_uid;
        new_uid.vector_span_offset = texture_binding_layouts_.size();
        new_uid.vector_span_length = texture_binding_count;
        texture_binding_layouts_.resize(new_uid.vector_span_offset +
                                        texture_binding_count);
        std::memcpy(
            texture_binding_layouts_.data() + new_uid.vector_span_offset,
            texture_bindings.data(), texture_binding_layout_bytes);
        texture_binding_layout_map_.emplace(texture_binding_layout_hash,
                                            new_uid);
      }
    }
    shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);

    // Use the sampler count for samplers because it's the only thing that must
    // be the same for layouts to be compatible in this case
    // (instruction-specified parameters are used as overrides for creating
    // actual samplers).
    static_assert(
        kLayoutUIDEmpty == 0,
        "Empty layout UID is assumed to be 0 because for bindful samplers, the "
        "UID is their count");
    shader.SetSamplerBindingLayoutUserUID(
        shader.GetSamplerBindingsAfterTranslation().size());
  }

  return true;
}

void VulkanPipelineCache::TranslateShadersForStorage(
    const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
    bool edram_fsi_used) {
  uint64_t translation_start = xe::Clock::QueryHostTickCount();

  std::vector<std::pair<VulkanShader*, uint64_t>> translations_to_do;
  translations_to_do.reserve(translations_needed.size());
  for (const auto& needed : translations_needed) {
    auto shader_it = shaders_.find(needed.first);
    if (shader_it == shaders_.end()) {
      continue;
    }
    VulkanShader* shader = shader_it->second;
    VulkanShader::VulkanTranslation* translation =
        static_cast<VulkanShader::VulkanTranslation*>(
            shader->GetOrCreateTranslation(needed.second));
    if (translation && !translation->is_translated()) {
      translations_to_do.emplace_back(shader, needed.second);
    }
  }

  if (translations_to_do.empty()) {
    return;
  }

  std::atomic<size_t> translation_index{0};
  std::atomic<size_t> translations_completed{0};

  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  bool msaa_2x_attachments =
      render_target_cache_.msaa_2x_attachments_supported();
  bool msaa_2x_no_attachments =
      render_target_cache_.msaa_2x_no_attachments_supported();
  uint32_t draw_res_x = render_target_cache_.draw_resolution_scale_x();
  uint32_t draw_res_y = render_target_cache_.draw_resolution_scale_y();

  auto translate_function = [this, &translations_to_do, &translation_index,
                             &translations_completed, vulkan_device,
                             msaa_2x_attachments, msaa_2x_no_attachments,
                             edram_fsi_used, draw_res_x, draw_res_y]() {
    // Each thread needs its own translator.
    SpirvShaderTranslator translator(
        SpirvShaderTranslator::Features(vulkan_device), msaa_2x_attachments,
        msaa_2x_no_attachments, edram_fsi_used, draw_res_x, draw_res_y);

    while (true) {
      size_t index = translation_index.fetch_add(1);
      if (index >= translations_to_do.size()) {
        break;
      }
      VulkanShader* shader = translations_to_do[index].first;
      uint64_t modification = translations_to_do[index].second;
      VulkanShader::VulkanTranslation* translation =
          static_cast<VulkanShader::VulkanTranslation*>(
              shader->GetTranslation(modification));
      if (translation && !translation->is_translated()) {
        if (TranslateAnalyzedShader(translator, *translation)) {
          translations_completed.fetch_add(1);
        }
      }
    }
  };

  size_t thread_count = 0;
  if (cvars::vulkan_pipeline_creation_threads != 0) {
    uint32_t logical_processor_count =
        std::max(uint32_t(1), xe::threading::logical_processor_count());
    if (cvars::vulkan_pipeline_creation_threads < 0) {
      thread_count = std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      thread_count = std::min(uint32_t(cvars::vulkan_pipeline_creation_threads),
                              logical_processor_count);
    }
    thread_count = std::min(thread_count, translations_to_do.size());
  }

  std::vector<std::unique_ptr<xe::threading::Thread>> translation_threads;
  for (size_t i = 0; i < thread_count; ++i) {
    auto thread = xe::threading::Thread::Create({}, translate_function);
    if (thread) {
      thread->set_name("Shader Translation");
      translation_threads.push_back(std::move(thread));
    }
  }

  // Main thread also participates.
  translate_function();

  for (auto& thread : translation_threads) {
    xe::threading::Wait(thread.get(), false);
  }

  XELOGI("Translated {} shaders in {} ms", translations_completed.load(),
         (xe::Clock::QueryHostTickCount() - translation_start) * 1000 /
             xe::Clock::QueryHostTickFrequency());
}

void VulkanPipelineCache::WritePipelineRenderTargetDescription(
    reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
    PipelineRenderTarget& render_target_out) const {
  if (write_mask) {
    assert_zero(write_mask & ~uint32_t(0b1111));
    // 32 because of 0x1F mask, for safety (all unknown to zero).
    static constexpr PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kOneMinusSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kOneMinusSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDstColor,
        /*  9 */ PipelineBlendFactor::kOneMinusDstColor,
        /* 10 */ PipelineBlendFactor::kDstAlpha,
        /* 11 */ PipelineBlendFactor::kOneMinusDstAlpha,
        /* 12 */ PipelineBlendFactor::kConstantColor,
        /* 13 */ PipelineBlendFactor::kOneMinusConstantColor,
        /* 14 */ PipelineBlendFactor::kConstantAlpha,
        /* 15 */ PipelineBlendFactor::kOneMinusConstantAlpha,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSaturate,
    };
    render_target_out.src_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_srcblend)];
    render_target_out.dst_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_destblend)];
    render_target_out.color_blend_op = blend_control.color_comb_fcn;
    render_target_out.src_alpha_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.alpha_srcblend)];
    render_target_out.dst_alpha_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.alpha_destblend)];
    render_target_out.alpha_blend_op = blend_control.alpha_comb_fcn;
    if (!command_processor_.GetVulkanDevice()
             ->properties()
             .constantAlphaColorBlendFactors) {
      if (blend_control.color_srcblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.src_color_blend_factor =
            PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_srcblend ==
                 xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.src_color_blend_factor =
            PipelineBlendFactor::kOneMinusConstantColor;
      }
      if (blend_control.color_destblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.dst_color_blend_factor =
            PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_destblend ==
                 xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.dst_color_blend_factor =
            PipelineBlendFactor::kOneMinusConstantColor;
      }
    }
  } else {
    render_target_out.src_color_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_color_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.color_blend_op = xenos::BlendOp::kAdd;
    render_target_out.src_alpha_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_alpha_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.alpha_blend_op = xenos::BlendOp::kAdd;
  }
  render_target_out.color_write_mask = write_mask;
}

bool VulkanPipelineCache::GetCurrentStateDescription(
    const VulkanShader::VulkanTranslation* vertex_shader,
    const VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key,
    PipelineDescription& description_out) const {
  description_out.Reset();

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  const RegisterFile& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  description_out.vertex_shader_hash =
      vertex_shader->shader().ucode_data_hash();
  description_out.vertex_shader_modification = vertex_shader->modification();
  if (pixel_shader) {
    description_out.pixel_shader_hash =
        pixel_shader->shader().ucode_data_hash();
    description_out.pixel_shader_modification = pixel_shader->modification();
  }
  description_out.render_pass_key = render_pass_key;

  // TODO(Triang3l): Implement primitive types currently using geometry shaders
  // without them.
  PipelineGeometryShader geometry_shader = PipelineGeometryShader::kNone;
  PipelinePrimitiveTopology primitive_topology;

  // Handle tessellated and non-tessellated draws separately, like D3D12.
  if (primitive_processing_result.IsTessellated()) {
    // Tessellation is enabled - use patch list topology.
    primitive_topology = PipelinePrimitiveTopology::kPatchList;

    // Get tessellation mode from registers.
    auto vgt_hos_cntl = regs.Get<reg::VGT_HOS_CNTL>();
    switch (vgt_hos_cntl.tess_mode) {
      case xenos::TessellationMode::kDiscrete:
        description_out.tessellation_mode = PipelineTessellationMode::kDiscrete;
        break;
      case xenos::TessellationMode::kContinuous:
        description_out.tessellation_mode =
            PipelineTessellationMode::kContinuous;
        break;
      case xenos::TessellationMode::kAdaptive:
        description_out.tessellation_mode = PipelineTessellationMode::kAdaptive;
        break;
      default:
        // Unknown tessellation mode, fall back to discrete.
        description_out.tessellation_mode = PipelineTessellationMode::kDiscrete;
        break;
    }

    // Determine patch type based on primitive type.
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kTrianglePatch:
        description_out.tessellation_patch =
            PipelineTessellationPatchType::kTriangle;
        break;
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::kQuadPatch:
        description_out.tessellation_patch =
            PipelineTessellationPatchType::kQuad;
        break;
      default:
        XELOGE("VulkanPipelineCache: Unsupported tessellated primitive type {}",
               uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }
  } else {
    // Non-tessellated draw.
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        geometry_shader = PipelineGeometryShader::kPointList;
        primitive_topology = PipelinePrimitiveTopology::kPointList;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = PipelinePrimitiveTopology::kLineList;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = PipelinePrimitiveTopology::kLineStrip;
        break;
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = PipelinePrimitiveTopology::kTriangleList;
        break;
      case xenos::PrimitiveType::kTriangleFan:
        // The check should be performed at primitive processing time.
        assert_true(device_properties.triangleFans);
        primitive_topology = PipelinePrimitiveTopology::kTriangleFan;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = PipelinePrimitiveTopology::kTriangleStrip;
        break;
      case xenos::PrimitiveType::kRectangleList:
        // Only use geometry shader if not using the fallback AsTriangleStrip
        // vertex shader type (which is used when geometry shaders aren't
        // supported).
        if (primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
          geometry_shader = PipelineGeometryShader::kRectangleList;
        }
        primitive_topology = PipelinePrimitiveTopology::kTriangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        geometry_shader = PipelineGeometryShader::kQuadList;
        primitive_topology = PipelinePrimitiveTopology::kLineListWithAdjacency;
        break;
      default:
        // TODO(Triang3l): Remaining primitive types.
        return false;
    }
  }
  description_out.geometry_shader = geometry_shader;
  description_out.primitive_topology = primitive_topology;
  description_out.primitive_restart =
      primitive_processing_result.host_primitive_reset_enabled;

  description_out.depth_clamp_enable =
      device_properties.depthClamp &&
      regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;

  // TODO(Triang3l): Tessellation.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (primitive_polygonal) {
    // Vulkan only allows the polygon mode to be set for both faces - pick the
    // most special one (more likely to represent the developer's deliberate
    // intentions - fill is very generic, wireframe is common in debug, points
    // are for pretty unusual things, but closer to debug purposes too - on the
    // Xenos, points have the lowest register value and triangles have the
    // highest) based on which faces are not culled.
    bool cull_front = pa_su_sc_mode_cntl.cull_front;
    bool cull_back = pa_su_sc_mode_cntl.cull_back;
    description_out.cull_front = cull_front;
    description_out.cull_back = cull_back;
    if (device_properties.fillModeNonSolid) {
      xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
      if (!cull_front) {
        polygon_type =
            std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
      }
      if (!cull_back) {
        polygon_type =
            std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
      }
      if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
        polygon_type = xenos::PolygonType::kTriangles;
      }
      switch (polygon_type) {
        case xenos::PolygonType::kPoints:
          // When points are not supported, use lines instead, preserving
          // debug-like purpose.
          description_out.polygon_mode = device_properties.pointPolygons
                                             ? PipelinePolygonMode::kPoint
                                             : PipelinePolygonMode::kLine;
          break;
        case xenos::PolygonType::kLines:
          description_out.polygon_mode = PipelinePolygonMode::kLine;
          break;
        case xenos::PolygonType::kTriangles:
          description_out.polygon_mode = PipelinePolygonMode::kFill;
          break;
        default:
          assert_unhandled_case(polygon_type);
          return false;
      }
    } else {
      description_out.polygon_mode = PipelinePolygonMode::kFill;
    }
    description_out.front_face_clockwise = pa_su_sc_mode_cntl.face != 0;
  } else {
    description_out.polygon_mode = PipelinePolygonMode::kFill;
  }

  if (render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    if (render_pass_key.depth_and_color_used & 1) {
      if (normalized_depth_control.z_enable) {
        description_out.depth_write_enable =
            normalized_depth_control.z_write_enable;
        description_out.depth_compare_op = normalized_depth_control.zfunc;
      } else {
        description_out.depth_compare_op = xenos::CompareFunction::kAlways;
      }
      if (normalized_depth_control.stencil_enable) {
        description_out.stencil_test_enable = 1;
        description_out.stencil_front_fail_op =
            normalized_depth_control.stencilfail;
        description_out.stencil_front_pass_op =
            normalized_depth_control.stencilzpass;
        description_out.stencil_front_depth_fail_op =
            normalized_depth_control.stencilzfail;
        description_out.stencil_front_compare_op =
            normalized_depth_control.stencilfunc;
        if (primitive_polygonal && normalized_depth_control.backface_enable) {
          description_out.stencil_back_fail_op =
              normalized_depth_control.stencilfail_bf;
          description_out.stencil_back_pass_op =
              normalized_depth_control.stencilzpass_bf;
          description_out.stencil_back_depth_fail_op =
              normalized_depth_control.stencilzfail_bf;
          description_out.stencil_back_compare_op =
              normalized_depth_control.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op =
              description_out.stencil_front_fail_op;
          description_out.stencil_back_pass_op =
              description_out.stencil_front_pass_op;
          description_out.stencil_back_depth_fail_op =
              description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_compare_op =
              description_out.stencil_front_compare_op;
        }
      }
    }

    // Color blending and write masks (filled only for the attachments present
    // in the render pass object).
    uint32_t render_pass_color_rts = render_pass_key.depth_and_color_used >> 1;
    assert_true(device_properties.independentBlend);
    uint32_t render_pass_color_rts_remaining = render_pass_color_rts;
    uint32_t color_rt_index;
    while (xe::bit_scan_forward(render_pass_color_rts_remaining,
                                &color_rt_index)) {
      render_pass_color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      WritePipelineRenderTargetDescription(
          regs.Get<reg::RB_BLENDCONTROL>(
              reg::RB_BLENDCONTROL::rt_register_indices[color_rt_index]),
          (normalized_color_mask >> (color_rt_index * 4)) & 0b1111,
          description_out.render_targets[color_rt_index]);
    }
  }

  return true;
}

bool VulkanPipelineCache::ArePipelineRequirementsMet(
    const PipelineDescription& description) const {
  VkShaderStageFlags vertex_shader_stage =
      Shader::IsHostVertexShaderTypeDomain(
          SpirvShaderTranslator::Modification(
              description.vertex_shader_modification)
              .vertex.host_vertex_shader_type)
          ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
          : VK_SHADER_STAGE_VERTEX_BIT;
  if (!(guest_shader_vertex_stages_ & vertex_shader_stage)) {
    return false;
  }

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  if (!device_properties.geometryShader &&
      description.geometry_shader != PipelineGeometryShader::kNone) {
    return false;
  }

  if (!device_properties.triangleFans &&
      description.primitive_topology ==
          PipelinePrimitiveTopology::kTriangleFan) {
    return false;
  }

  if (!device_properties.depthClamp && description.depth_clamp_enable) {
    return false;
  }

  if (!device_properties.pointPolygons &&
      description.polygon_mode == PipelinePolygonMode::kPoint) {
    return false;
  }

  if (!device_properties.fillModeNonSolid &&
      description.polygon_mode != PipelinePolygonMode::kFill) {
    return false;
  }

  assert_true(device_properties.independentBlend);

  if (!device_properties.constantAlphaColorBlendFactors) {
    uint32_t color_rts_remaining =
        description.render_pass_key.depth_and_color_used >> 1;
    uint32_t color_rt_index;
    while (xe::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
      color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      const PipelineRenderTarget& color_rt =
          description.render_targets[color_rt_index];
      if (color_rt.src_color_blend_factor ==
              PipelineBlendFactor::kConstantAlpha ||
          color_rt.src_color_blend_factor ==
              PipelineBlendFactor::kOneMinusConstantAlpha ||
          color_rt.dst_color_blend_factor ==
              PipelineBlendFactor::kConstantAlpha ||
          color_rt.dst_color_blend_factor ==
              PipelineBlendFactor::kOneMinusConstantAlpha) {
        return false;
      }
    }
  }

  return true;
}

bool VulkanPipelineCache::GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    SpirvShaderTranslator::Modification vertex_shader_modification,
    SpirvShaderTranslator::Modification pixel_shader_modification,
    GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  // For kPointListAsTriangleStrip, output_point_parameters has a different
  // meaning (the coordinates, not the size). However, the AsTriangleStrip host
  // vertex shader types are needed specifically when geometry shaders are not
  // supported as fallbacks - in that case, geometry_shader_type should be kNone
  // and this function shouldn't be called.
  if (vertex_shader_modification.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      vertex_shader_modification.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    XELOGE(
        "GetGeometryShaderKey: AsTriangleStrip vertex shader types should not "
        "be used with geometry shaders");
    return false;
  }
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  // TODO(Triang3l): Once all needed inputs and outputs are added, uncomment the
  // real counts here.
  key.interpolator_count =
      xe::bit_count(vertex_shader_modification.vertex.interpolator_mask);
  key.has_vertex_kill_and =
      /* vertex_shader_modification.vertex.vertex_kill_and */ 0;
  key.has_point_size =
      vertex_shader_modification.vertex.output_point_parameters;
  key.has_point_coordinates = pixel_shader_modification.pixel.param_gen_point;
  // Single bit to indicate if clip planes are enabled.
  key.has_user_clip_planes =
      uint32_t(vertex_shader_modification.vertex.user_clip_plane_count > 0);
  key.user_clip_plane_cull =
      vertex_shader_modification.vertex.user_clip_plane_cull;
  key_out = key;
  return true;
}

VkShaderModule VulkanPipelineCache::GetGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  spv::ExecutionMode input_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t input_primitive_vertex_count = 0;
  spv::ExecutionMode output_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t output_max_vertices = 0;
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      // Point to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputPoints;
      input_primitive_vertex_count = 1;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kRectangleList:
      // Triangle to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeTriangles;
      input_primitive_vertex_count = 3;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kQuadList:
      // 4 vertices passed via a line list with adjacency to a strip of 2
      // triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputLinesAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    default:
      assert_unhandled_case(key.type);
  }

  // When enabled, use max size to reduce variants from different counts.
  constexpr uint32_t kMaxUserClipPlanes = 6;
  uint32_t user_clip_plane_count =
      key.has_user_clip_planes ? kMaxUserClipPlanes : 0;
  uint32_t clip_distance_count = 0;
  uint32_t cull_distance_count = 0;
  if (key.user_clip_plane_cull) {
    cull_distance_count = user_clip_plane_count;
  } else {
    clip_distance_count = user_clip_plane_count;
  }
  cull_distance_count += key.has_vertex_kill_and;

  SpirvBuilder builder(spv::Spv_1_0,
                       (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityGeometry);
  if (clip_distance_count) {
    builder.addCapability(spv::CapabilityClipDistance);
  }
  if (cull_distance_count) {
    builder.addCapability(spv::CapabilityCullDistance);
  }
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  // TODO(Triang3l): Shader float controls (NaN preservation most importantly).

  std::vector<spv::Id> main_interface;

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_bool4 = builder.makeVectorType(type_bool, 4);
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);
  spv::Id type_clip_distances =
      clip_distance_count
          ? builder.makeArrayType(
                type_float, builder.makeUintConstant(clip_distance_count), 0)
          : spv::NoType;
  spv::Id type_cull_distances =
      cull_distance_count
          ? builder.makeArrayType(
                type_float, builder.makeUintConstant(cull_distance_count), 0)
          : spv::NoType;

  // System constants.
  // For points:
  // - float2 point_constant_diameter
  // - float2 point_screen_diameter_to_ndc_radius
  enum PointConstant : uint32_t {
    kPointConstantConstantDiameter,
    kPointConstantScreenDiameterToNdcRadius,
    kPointConstantCount,
  };
  spv::Id type_system_constants = spv::NoType;
  if (key.type == PipelineGeometryShader::kPointList) {
    id_vector_temp.clear();
    id_vector_temp.resize(kPointConstantCount);
    id_vector_temp[kPointConstantConstantDiameter] = type_float2;
    id_vector_temp[kPointConstantScreenDiameterToNdcRadius] = type_float2;
    type_system_constants =
        builder.makeStructType(id_vector_temp, "XeSystemConstants");
    builder.addMemberName(type_system_constants, kPointConstantConstantDiameter,
                          "point_constant_diameter");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantConstantDiameter,
        spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants,
                     point_constant_diameter)));
    builder.addMemberName(type_system_constants,
                          kPointConstantScreenDiameterToNdcRadius,
                          "point_screen_diameter_to_ndc_radius");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantScreenDiameterToNdcRadius,
        spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants,
                     point_screen_diameter_to_ndc_radius)));
  }
  spv::Id uniform_system_constants = spv::NoResult;
  if (type_system_constants != spv::NoType) {
    builder.addDecoration(type_system_constants, spv::DecorationBlock);
    uniform_system_constants = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniform, type_system_constants,
        "xe_uniform_system_constants");
    builder.addDecoration(uniform_system_constants,
                          spv::DecorationDescriptorSet,
                          int(SpirvShaderTranslator::kDescriptorSetConstants));
    builder.addDecoration(uniform_system_constants, spv::DecorationBinding,
                          int(SpirvShaderTranslator::kConstantBufferSystem));
    // Generating SPIR-V 1.0, no need to add bindings to the entry point's
    // interface until SPIR-V 1.4.
  }

  // Inputs and outputs - matching glslang order, in gl_PerVertex gl_in[],
  // user-defined outputs, user-defined inputs, out gl_PerVertex.
  // TODO(Triang3l): Point parameters from the system uniform buffer.

  spv::Id const_input_primitive_vertex_count =
      builder.makeUintConstant(input_primitive_vertex_count);

  // in gl_PerVertex gl_in[].
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_in_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_in_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_in_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_in_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_in_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_in_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_in_gl_per_vertex_clip_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_clip_distance));
  }
  // gl_CullDistance.
  uint32_t member_in_gl_per_vertex_cull_distance = UINT32_MAX;
  if (cull_distance_count) {
    member_in_gl_per_vertex_cull_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_cull_distances);
  }
  // Structure and array.
  spv::Id type_struct_in_gl_per_vertex =
      builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_in_gl_per_vertex,
                        member_in_gl_per_vertex_position, "gl_Position");
  builder.addMemberDecoration(
      type_struct_in_gl_per_vertex, member_in_gl_per_vertex_position,
      spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::Position));
  if (clip_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex,
                          member_in_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(
        type_struct_in_gl_per_vertex, member_in_gl_per_vertex_clip_distance,
        spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::ClipDistance));
  }
  if (cull_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex,
                          member_in_gl_per_vertex_cull_distance,
                          "gl_CullDistance");
    builder.addMemberDecoration(
        type_struct_in_gl_per_vertex, member_in_gl_per_vertex_cull_distance,
        spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::CullDistance));
  }
  builder.addDecoration(type_struct_in_gl_per_vertex, spv::DecorationBlock);
  spv::Id type_array_in_gl_per_vertex = builder.makeArrayType(
      type_struct_in_gl_per_vertex, const_input_primitive_vertex_count, 0);
  spv::Id in_gl_per_vertex =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                             type_array_in_gl_per_vertex, "gl_in");
  main_interface.push_back(in_gl_per_vertex);

  uint32_t output_location = 0;

  // Interpolators outputs.
  std::array<spv::Id, xenos::kMaxInterpolators> out_interpolators;
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    spv::Id out_interpolator = builder.createVariable(
        spv::NoPrecision, spv::StorageClassOutput, type_float4,
        fmt::format("xe_out_interpolator_{}", i).c_str());
    out_interpolators[i] = out_interpolator;
    builder.addDecoration(out_interpolator, spv::DecorationLocation,
                          int(output_location));
    builder.addDecoration(out_interpolator, spv::DecorationInvariant);
    main_interface.push_back(out_interpolator);
    ++output_location;
  }

  // Point coordinate output.
  spv::Id out_point_coordinates = spv::NoResult;
  if (key.has_point_coordinates) {
    out_point_coordinates =
        builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                               type_float2, "xe_out_point_coordinates");
    builder.addDecoration(out_point_coordinates, spv::DecorationLocation,
                          int(output_location));
    builder.addDecoration(out_point_coordinates, spv::DecorationInvariant);
    main_interface.push_back(out_point_coordinates);
    ++output_location;
  }

  uint32_t input_location = 0;

  // Interpolator inputs.
  std::array<spv::Id, xenos::kMaxInterpolators> in_interpolators;
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    spv::Id in_interpolator = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float4, const_input_primitive_vertex_count,
                              0),
        fmt::format("xe_in_interpolator_{}", i).c_str());
    in_interpolators[i] = in_interpolator;
    builder.addDecoration(in_interpolator, spv::DecorationLocation,
                          int(input_location));
    main_interface.push_back(in_interpolator);
    ++input_location;
  }

  // Point size input.
  spv::Id in_point_size = spv::NoResult;
  if (key.has_point_size) {
    in_point_size = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float, const_input_primitive_vertex_count,
                              0),
        "xe_in_point_size");
    builder.addDecoration(in_point_size, spv::DecorationLocation,
                          int(input_location));
    main_interface.push_back(in_point_size);
    ++input_location;
  }

  // out gl_PerVertex.
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_out_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_out_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_out_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_out_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_out_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_out_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_out_gl_per_vertex_clip_distance = builder.makeIntConstant(
        int32_t(member_out_gl_per_vertex_clip_distance));
  }
  // Structure.
  spv::Id type_struct_out_gl_per_vertex =
      builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_out_gl_per_vertex,
                        member_out_gl_per_vertex_position, "gl_Position");
  builder.addMemberDecoration(
      type_struct_out_gl_per_vertex, member_out_gl_per_vertex_position,
      spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::Position));
  if (clip_distance_count) {
    builder.addMemberName(type_struct_out_gl_per_vertex,
                          member_out_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(
        type_struct_out_gl_per_vertex, member_out_gl_per_vertex_clip_distance,
        spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::ClipDistance));
  }
  builder.addDecoration(type_struct_out_gl_per_vertex, spv::DecorationBlock);
  spv::Id out_gl_per_vertex =
      builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                             type_struct_out_gl_per_vertex, "");
  builder.addDecoration(out_gl_per_vertex, spv::DecorationInvariant);
  main_interface.push_back(out_gl_per_vertex);

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function =
      builder.makeFunctionEntry(spv::NoPrecision, type_void, "main",
                                main_param_types, main_precisions, &main_entry);
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelGeometry, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }
  builder.addExecutionMode(main_function, input_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeInvocations, 1);
  builder.addExecutionMode(main_function, output_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeOutputVertices,
                           int(output_max_vertices));

  // Note that after every OpEmitVertex, all output variables are undefined.

  // Discard the whole primitive if any vertex has a NaN position (may also be
  // set to NaN for emulation of vertex killing with the OR operator).
  for (uint32_t i = 0; i < input_primitive_vertex_count; ++i) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
    id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
    spv::Id position_is_nan = builder.createUnaryOp(
        spv::OpAny, type_bool,
        builder.createUnaryOp(
            spv::OpIsNan, type_bool4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision)));
    spv::Block& discard_predecessor = *builder.getBuildPoint();
    spv::Block& discard_then_block = builder.makeNewBlock();
    spv::Block& discard_merge_block = builder.makeNewBlock();
    builder.createSelectionMerge(&discard_merge_block,
                                 spv::SelectionControlDontFlattenMask);
    {
      std::unique_ptr<spv::Instruction> branch_conditional_op(
          std::make_unique<spv::Instruction>(spv::OpBranchConditional));
      branch_conditional_op->addIdOperand(position_is_nan);
      branch_conditional_op->addIdOperand(discard_then_block.getId());
      branch_conditional_op->addIdOperand(discard_merge_block.getId());
      branch_conditional_op->addImmediateOperand(1);
      branch_conditional_op->addImmediateOperand(2);
      discard_predecessor.addInstruction(std::move(branch_conditional_op));
    }
    discard_then_block.addPredecessor(&discard_predecessor);
    discard_merge_block.addPredecessor(&discard_predecessor);
    builder.setBuildPoint(&discard_then_block);
    builder.createNoResultOp(spv::OpReturn);
    builder.setBuildPoint(&discard_merge_block);
  }

  // Cull the whole primitive if any cull distance for all vertices in the
  // primitive is < 0.
  // TODO(Triang3l): For points, handle ps_ucp_mode (transform the host clip
  // space to the guest one, calculate the distances to the user clip planes,
  // cull using the distance from the center for modes 0, 1 and 2, cull and clip
  // per-vertex for modes 2 and 3) - except for the vertex kill flag.
  if (cull_distance_count) {
    spv::Id const_member_in_gl_per_vertex_cull_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_cull_distance));
    spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
    spv::Id cull_condition = spv::NoResult;
    for (uint32_t i = 0; i < cull_distance_count; ++i) {
      for (uint32_t j = 0; j < input_primitive_vertex_count; ++j) {
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(j)));
        id_vector_temp.push_back(const_member_in_gl_per_vertex_cull_distance);
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
        spv::Id cull_distance_is_negative = builder.createBinOp(
            spv::OpFOrdLessThan, type_bool,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision),
            const_float_0);
        if (cull_condition != spv::NoResult) {
          cull_condition =
              builder.createBinOp(spv::OpLogicalAnd, type_bool, cull_condition,
                                  cull_distance_is_negative);
        } else {
          cull_condition = cull_distance_is_negative;
        }
      }
    }
    assert_true(cull_condition != spv::NoResult);
    spv::Block& discard_predecessor = *builder.getBuildPoint();
    spv::Block& discard_then_block = builder.makeNewBlock();
    spv::Block& discard_merge_block = builder.makeNewBlock();
    builder.createSelectionMerge(&discard_merge_block,
                                 spv::SelectionControlDontFlattenMask);
    {
      std::unique_ptr<spv::Instruction> branch_conditional_op(
          std::make_unique<spv::Instruction>(spv::OpBranchConditional));
      branch_conditional_op->addIdOperand(cull_condition);
      branch_conditional_op->addIdOperand(discard_then_block.getId());
      branch_conditional_op->addIdOperand(discard_merge_block.getId());
      branch_conditional_op->addImmediateOperand(1);
      branch_conditional_op->addImmediateOperand(2);
      discard_predecessor.addInstruction(std::move(branch_conditional_op));
    }
    discard_then_block.addPredecessor(&discard_predecessor);
    discard_merge_block.addPredecessor(&discard_predecessor);
    builder.setBuildPoint(&discard_then_block);
    builder.createNoResultOp(spv::OpReturn);
    builder.setBuildPoint(&discard_merge_block);
  }

  switch (key.type) {
    case PipelineGeometryShader::kPointList: {
      // Expand the point sprite, with left-to-right, top-to-bottom UVs.

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_float_0 = builder.makeFloatConstant(0.0f);

      // Load the point diameter in guest pixels.
      id_vector_temp.clear();
      id_vector_temp.push_back(
          builder.makeIntConstant(int32_t(kPointConstantConstantDiameter)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_guest_diameter_x = builder.createLoad(
          builder.createAccessChain(spv::StorageClassUniform,
                                    uniform_system_constants, id_vector_temp),
          spv::NoPrecision);
      id_vector_temp.back() = const_int_1;
      spv::Id point_guest_diameter_y = builder.createLoad(
          builder.createAccessChain(spv::StorageClassUniform,
                                    uniform_system_constants, id_vector_temp),
          spv::NoPrecision);
      if (key.has_point_size) {
        // The vertex shader's header writes -1.0 to point_size by default, so
        // any non-negative value means that it was overwritten by the
        // translated vertex shader, and needs to be used instead of the
        // constant size. The per-vertex diameter is already clamped in the
        // vertex shader (combined with making it non-negative).
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        spv::Id point_vertex_diameter = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_point_size,
                                      id_vector_temp),
            spv::NoPrecision);
        spv::Id point_vertex_diameter_written =
            builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool,
                                point_vertex_diameter, const_float_0);
        point_guest_diameter_x = builder.createTriOp(
            spv::OpSelect, type_float, point_vertex_diameter_written,
            point_vertex_diameter, point_guest_diameter_x);
        point_guest_diameter_y = builder.createTriOp(
            spv::OpSelect, type_float, point_vertex_diameter_written,
            point_vertex_diameter, point_guest_diameter_y);
      }

      // 4D5307F1 has zero-size snowflakes, drop them quicker, and also drop
      // points with a constant size of zero since point lists may also be used
      // as just "compute" with memexport.
      spv::Id point_size_not_zero = builder.createBinOp(
          spv::OpLogicalAnd, type_bool,
          builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                              point_guest_diameter_x, const_float_0),
          builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                              point_guest_diameter_y, const_float_0));
      spv::Block& point_size_zero_predecessor = *builder.getBuildPoint();
      spv::Block& point_size_zero_then_block = builder.makeNewBlock();
      spv::Block& point_size_zero_merge_block = builder.makeNewBlock();
      builder.createSelectionMerge(&point_size_zero_merge_block,
                                   spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> branch_conditional_op(
            std::make_unique<spv::Instruction>(spv::OpBranchConditional));
        branch_conditional_op->addIdOperand(point_size_not_zero);
        branch_conditional_op->addIdOperand(
            point_size_zero_merge_block.getId());
        branch_conditional_op->addIdOperand(point_size_zero_then_block.getId());
        branch_conditional_op->addImmediateOperand(2);
        branch_conditional_op->addImmediateOperand(1);
        point_size_zero_predecessor.addInstruction(
            std::move(branch_conditional_op));
      }
      point_size_zero_then_block.addPredecessor(&point_size_zero_predecessor);
      point_size_zero_merge_block.addPredecessor(&point_size_zero_predecessor);
      builder.setBuildPoint(&point_size_zero_then_block);
      builder.createNoResultOp(spv::OpReturn);
      builder.setBuildPoint(&point_size_zero_merge_block);

      // Transform the diameter in the guest screen coordinates to radius in the
      // normalized device coordinates, and then to the clip space by
      // multiplying by W.
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.makeIntConstant(
          int32_t(kPointConstantScreenDiameterToNdcRadius)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_radius_x = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_x,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants,
                                                       id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.back() = const_int_1;
      spv::Id point_radius_y = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_y,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants,
                                                       id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id point_position = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                    id_vector_temp),
          spv::NoPrecision);
      spv::Id point_w =
          builder.createCompositeExtract(point_position, type_float, 3);
      point_radius_x = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_radius_x, point_w);
      point_radius_y = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_radius_y, point_w);

      // Load the inputs for the guest point.
      // Interpolators.
      std::array<spv::Id, xenos::kMaxInterpolators> point_interpolators;
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        point_interpolators[i] = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput,
                                      in_interpolators[i], id_vector_temp),
            spv::NoPrecision);
      }
      // Positions.
      spv::Id point_x =
          builder.createCompositeExtract(point_position, type_float, 0);
      spv::Id point_y =
          builder.createCompositeExtract(point_position, type_float, 1);
      std::array<spv::Id, 2> point_edge_x, point_edge_y;
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Op point_radius_add_op = i ? spv::OpFAdd : spv::OpFSub;
        point_edge_x[i] = builder.createNoContractionBinOp(
            point_radius_add_op, type_float, point_x, point_radius_x);
        point_edge_y[i] = builder.createNoContractionBinOp(
            point_radius_add_op, type_float, point_y, point_radius_y);
      };
      spv::Id point_z =
          builder.createCompositeExtract(point_position, type_float, 2);
      // Clip distances.
      spv::Id point_clip_distances = spv::NoResult;
      if (clip_distance_count) {
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        point_clip_distances = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
      }

      for (uint32_t i = 0; i < 4; ++i) {
        // Same interpolators for the entire sprite.
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(point_interpolators[j], out_interpolators[j]);
        }
        // Top-left, bottom-left, top-right, bottom-right order (chosen
        // arbitrarily, simply based on counterclockwise meaning front with
        // frontFace = VkFrontFace(0), but faceness is ignored for non-polygon
        // primitive types).
        uint32_t point_vertex_x = i >> 1;
        uint32_t point_vertex_y = i & 1;
        // Point coordinates.
        if (key.has_point_coordinates) {
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.makeFloatConstant(float(point_vertex_x)));
          id_vector_temp.push_back(
              builder.makeFloatConstant(float(point_vertex_y)));
          builder.createStore(
              builder.makeCompositeConstant(type_float2, id_vector_temp),
              out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(point_edge_x[point_vertex_x]);
        id_vector_temp.push_back(point_edge_y[point_vertex_y]);
        id_vector_temp.push_back(point_z);
        id_vector_temp.push_back(point_w);
        spv::Id point_vertex_position =
            builder.createCompositeConstruct(type_float4, id_vector_temp);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            point_vertex_position,
            builder.createAccessChain(spv::StorageClassOutput,
                                      out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        // TODO(Triang3l): Handle ps_ucp_mode properly, clip expanded points if
        // needed.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(
              const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(
              point_clip_distances,
              builder.createAccessChain(spv::StorageClassOutput,
                                        out_gl_per_vertex, id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kRectangleList: {
      // Construct a strip with the fourth vertex generated by mirroring a
      // vertex across the longest edge (the diagonal).
      //
      // Possible options:
      //
      // 0---1
      // |  /|
      // | / |  - 12 is the longest edge, strip 0123 (most commonly used)
      // |/  |    v3 = v0 + (v1 - v0) + (v2 - v0), or v3 = -v0 + v1 + v2
      // 2--[3]
      //
      // 1---2
      // |  /|
      // | / |  - 20 is the longest edge, strip 1203
      // |/  |
      // 0--[3]
      //
      // 2---0
      // |  /|
      // | / |  - 01 is the longest edge, strip 2013
      // |/  |
      // 1--[3]

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_int_2 = builder.makeIntConstant(2);
      spv::Id const_int_3 = builder.makeIntConstant(3);

      // Get squares of edge lengths to choose the longest edge.
      // [0] - 12, [1] - 20, [2] - 01.
      spv::Id edge_lengths[3];
      id_vector_temp.resize(3);
      id_vector_temp[1] = const_member_in_gl_per_vertex_position;
      for (uint32_t i = 0; i < 3; ++i) {
        id_vector_temp[0] = builder.makeIntConstant(int32_t((1 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_0_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_0_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = builder.makeIntConstant(int32_t((2 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_1_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_1_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        spv::Id edge_x =
            builder.createBinOp(spv::OpFSub, type_float, edge_1_x, edge_0_x);
        spv::Id edge_y =
            builder.createBinOp(spv::OpFSub, type_float, edge_1_y, edge_0_y);
        edge_lengths[i] = builder.createBinOp(
            spv::OpFAdd, type_float,
            builder.createBinOp(spv::OpFMul, type_float, edge_x, edge_x),
            builder.createBinOp(spv::OpFMul, type_float, edge_y, edge_y));
      }

      // Choose the index of the first vertex in the strip based on which edge
      // is the longest, and calculate the indices of the other vertices.
      spv::Id vertex_indices[3];
      // If 12 > 20 && 12 > 01, then 12 is the longest edge, and the strip is
      // 0123. Otherwise, if 20 > 01, then 20 is the longest, and the strip is
      // 1203, but if not, 01 is the longest, and the strip is 2013.
      vertex_indices[0] = builder.createTriOp(
          spv::OpSelect, type_int,
          builder.createBinOp(
              spv::OpLogicalAnd, type_bool,
              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                  edge_lengths[0], edge_lengths[1]),
              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                  edge_lengths[0], edge_lengths[2])),
          const_int_0,
          builder.createTriOp(
              spv::OpSelect, type_int,
              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                  edge_lengths[1], edge_lengths[2]),
              const_int_1, const_int_2));
      for (uint32_t i = 1; i < 3; ++i) {
        // vertex_indices[i] = (vertex_indices[0] + i) % 3
        spv::Id vertex_index_without_wrapping =
            builder.createBinOp(spv::OpIAdd, type_int, vertex_indices[0],
                                builder.makeIntConstant(int32_t(i)));
        vertex_indices[i] = builder.createTriOp(
            spv::OpSelect, type_int,
            builder.createBinOp(spv::OpSLessThan, type_bool,
                                vertex_index_without_wrapping, const_int_3),
            vertex_index_without_wrapping,
            builder.createBinOp(spv::OpISub, type_int,
                                vertex_index_without_wrapping, const_int_3));
      }

      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero =
            builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Emit the triangle in the strip that consists of the original vertices.
      for (uint32_t i = 0; i < 3; ++i) {
        spv::Id vertex_index = vertex_indices[i];
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(
                                     spv::StorageClassInput,
                                     in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero,
                              out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput,
                                      out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput,
                                        in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(
              vertex_clip_distances,
              builder.createAccessChain(spv::StorageClassOutput,
                                        out_gl_per_vertex, id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }

      // Construct the fourth vertex.
      // Interpolators.
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        spv::Id in_interpolator = in_interpolators[i];
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        spv::Id vertex_interpolator_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolator,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_interpolator_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_interpolator, id_vector_temp),
                spv::NoPrecision),
            vertex_interpolator_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_interpolator_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float4, vertex_interpolator_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_interpolator, id_vector_temp),
                spv::NoPrecision));
        builder.createStore(vertex_interpolator_v3, out_interpolators[i]);
      }
      // Point coordinates.
      if (key.has_point_coordinates) {
        builder.createStore(const_point_coordinates_zero,
                            out_point_coordinates);
      }
      // Position.
      id_vector_temp.clear();
      id_vector_temp.push_back(vertex_indices[0]);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id vertex_position_v0 = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                    id_vector_temp),
          spv::NoPrecision);
      id_vector_temp[0] = vertex_indices[1];
      spv::Id vertex_position_v01 = builder.createNoContractionBinOp(
          spv::OpFSub, type_float4,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput,
                                        in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision),
          vertex_position_v0);
      id_vector_temp[0] = vertex_indices[2];
      spv::Id vertex_position_v3 = builder.createNoContractionBinOp(
          spv::OpFAdd, type_float4, vertex_position_v01,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput,
                                        in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision));
      id_vector_temp.clear();
      id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
      builder.createStore(
          vertex_position_v3,
          builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                    id_vector_temp));
      // Clip distances.
      for (uint32_t i = 0; i < clip_distance_count; ++i) {
        spv::Id const_int_i = builder.makeIntConstant(int32_t(i));
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        spv::Id vertex_clip_distance_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_clip_distance_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision),
            vertex_clip_distance_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_clip_distance_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float, vertex_clip_distance_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision));
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        builder.createStore(
            vertex_clip_distance_v3,
            builder.createAccessChain(spv::StorageClassOutput,
                                      out_gl_per_vertex, id_vector_temp));
      }
      // Emit the vertex.
      builder.createNoResultOp(spv::OpEmitVertex);
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kQuadList: {
      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero =
            builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Build the triangle strip from the original quad vertices in the
      // 0, 1, 3, 2 order (like specified for GL_QUAD_STRIP).
      // TODO(Triang3l): Find the correct decomposition of quads into triangles
      // on the real hardware.
      for (uint32_t i = 0; i < 4; ++i) {
        spv::Id const_vertex_index =
            builder.makeIntConstant(int32_t(i ^ (i >> 1)));
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(
                                     spv::StorageClassInput,
                                     in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero,
                              out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput,
                                      out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(const_vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput,
                                        in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(
              vertex_clip_distances,
              builder.createAccessChain(spv::StorageClassOutput,
                                        out_gl_per_vertex, id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    default:
      assert_unhandled_case(key.type);
  }

  // End the main function.
  builder.leaveFunction();

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      command_processor_.GetVulkanDevice(),
      reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    XELOGE(
        "VulkanPipelineCache: Failed to create the primitive type geometry "
        "shader 0x{:08X}",
        key.key);
  }
  geometry_shaders_.emplace(key, shader_module);
  return shader_module;
}

VkShaderModule VulkanPipelineCache::GetTessellationControlShader(
    PipelineTessellationMode mode, PipelineTessellationPatchType patch_type,
    bool use_control_point_count) const {
  if (mode == PipelineTessellationMode::kNone ||
      patch_type == PipelineTessellationPatchType::kNone) {
    return VK_NULL_HANDLE;
  }

  switch (mode) {
    case PipelineTessellationMode::kDiscrete:
      if (patch_type == PipelineTessellationPatchType::kTriangle) {
        return use_control_point_count ? discrete_triangle_3cp_hs_
                                       : discrete_triangle_1cp_hs_;
      } else {
        return use_control_point_count ? discrete_quad_4cp_hs_
                                       : discrete_quad_1cp_hs_;
      }
    case PipelineTessellationMode::kContinuous:
      if (patch_type == PipelineTessellationPatchType::kTriangle) {
        return use_control_point_count ? continuous_triangle_3cp_hs_
                                       : continuous_triangle_1cp_hs_;
      } else {
        return use_control_point_count ? continuous_quad_4cp_hs_
                                       : continuous_quad_1cp_hs_;
      }
    case PipelineTessellationMode::kAdaptive:
      // Adaptive mode always uses per-corner control points.
      if (patch_type == PipelineTessellationPatchType::kTriangle) {
        return adaptive_triangle_hs_;
      } else {
        return adaptive_quad_hs_;
      }
    default:
      return VK_NULL_HANDLE;
  }
}

VkShaderModule VulkanPipelineCache::GetTessellationVertexShader(
    PipelineTessellationMode mode) const {
  if (mode == PipelineTessellationMode::kNone) {
    return VK_NULL_HANDLE;
  }
  // Adaptive mode reads edge factors from index buffer; other modes pass
  // vertex indices.
  return (mode == PipelineTessellationMode::kAdaptive)
             ? tessellation_adaptive_vs_
             : tessellation_indexed_vs_;
}

bool VulkanPipelineCache::EnsurePipelineCreated(
    const PipelineCreationArguments& creation_arguments,
    VkShaderModule fragment_shader_override) {
  // Check if we already have a pipeline.
  // If it's a placeholder and we're not creating another placeholder,
  // we need to replace it with the real pipeline.
  VkPipeline existing_pipeline =
      creation_arguments.pipeline->second.pipeline.load(
          std::memory_order_acquire);
  bool is_placeholder = creation_arguments.pipeline->second.is_placeholder.load(
      std::memory_order_acquire);
  bool creating_placeholder = fragment_shader_override != VK_NULL_HANDLE;

  if (existing_pipeline != VK_NULL_HANDLE) {
    if (!is_placeholder || creating_placeholder) {
      // Already have a real pipeline, or trying to create another placeholder.
      return true;
    }
    // Have a placeholder, and we're creating the real pipeline to replace it.
  }

  // This function preferably should validate the description to prevent
  // unsupported behavior that may be dangerous/crashing because pipelines can
  // be created from the disk storage.

  if (creation_arguments.pixel_shader) {
    XELOGGPU("Creating graphics pipeline state with VS {:016X}, PS {:016X}",
             creation_arguments.vertex_shader->shader().ucode_data_hash(),
             creation_arguments.pixel_shader->shader().ucode_data_hash());
  } else {
    XELOGGPU("Creating graphics pipeline state with VS {:016X}",
             creation_arguments.vertex_shader->shader().ucode_data_hash());
  }

  const PipelineDescription& description = creation_arguments.pipeline->first;
  if (!ArePipelineRequirementsMet(description)) {
    assert_always(
        "When creating a new pipeline, the description must not require "
        "unsupported features, and when loading the pipeline storage, "
        "pipelines with unsupported features must be filtered out");
    return false;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;

  bool is_tessellated =
      description.tessellation_mode != PipelineTessellationMode::kNone;

  // Up to 5 shader stages: VS, TCS, TES, GS, FS.
  std::array<VkPipelineShaderStageCreateInfo, 5> shader_stages;
  uint32_t shader_stage_count = 0;

  // Vertex shader or tessellation evaluation shader.
  assert_true(creation_arguments.vertex_shader->is_translated());
  if (!creation_arguments.vertex_shader->is_valid()) {
    return false;
  }

  if (is_tessellated) {
    // For tessellation: use our pre-compiled VS for passing data to TCS,
    // then TCS (hull shader), then the translated Xenos vertex shader as TES
    // (domain shader).

    // Tessellation vertex shader (passes indices to TCS).
    VkPipelineShaderStageCreateInfo& shader_stage_tess_vs =
        shader_stages[shader_stage_count++];
    shader_stage_tess_vs.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tess_vs.pNext = nullptr;
    shader_stage_tess_vs.flags = 0;
    shader_stage_tess_vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stage_tess_vs.module = creation_arguments.tessellation_vertex_shader;
    shader_stage_tess_vs.pName = "main";
    shader_stage_tess_vs.pSpecializationInfo = nullptr;

    // Tessellation control shader (hull shader).
    VkPipelineShaderStageCreateInfo& shader_stage_tcs =
        shader_stages[shader_stage_count++];
    shader_stage_tcs.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tcs.pNext = nullptr;
    shader_stage_tcs.flags = 0;
    shader_stage_tcs.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    shader_stage_tcs.module = creation_arguments.tessellation_control_shader;
    shader_stage_tcs.pName = "main";
    shader_stage_tcs.pSpecializationInfo = nullptr;

    // Tessellation evaluation shader (domain shader) - the translated Xenos
    // vertex shader.
    VkPipelineShaderStageCreateInfo& shader_stage_tes =
        shader_stages[shader_stage_count++];
    shader_stage_tes.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tes.pNext = nullptr;
    shader_stage_tes.flags = 0;
    shader_stage_tes.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    shader_stage_tes.module = creation_arguments.vertex_shader->shader_module();
    assert_true(shader_stage_tes.module != VK_NULL_HANDLE);
    shader_stage_tes.pName = "main";
    shader_stage_tes.pSpecializationInfo = nullptr;
  } else {
    // Non-tessellated: standard vertex shader.
    VkPipelineShaderStageCreateInfo& shader_stage_vertex =
        shader_stages[shader_stage_count++];
    shader_stage_vertex.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_vertex.pNext = nullptr;
    shader_stage_vertex.flags = 0;
    shader_stage_vertex.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stage_vertex.module =
        creation_arguments.vertex_shader->shader_module();
    assert_true(shader_stage_vertex.module != VK_NULL_HANDLE);
    shader_stage_vertex.pName = "main";
    shader_stage_vertex.pSpecializationInfo = nullptr;
  }

  // Geometry shader.
  if (creation_arguments.geometry_shader != VK_NULL_HANDLE) {
    VkPipelineShaderStageCreateInfo& shader_stage_geometry =
        shader_stages[shader_stage_count++];
    shader_stage_geometry.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_geometry.pNext = nullptr;
    shader_stage_geometry.flags = 0;
    shader_stage_geometry.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    shader_stage_geometry.module = creation_arguments.geometry_shader;
    shader_stage_geometry.pName = "main";
    shader_stage_geometry.pSpecializationInfo = nullptr;
  }
  // Fragment shader.
  VkPipelineShaderStageCreateInfo& shader_stage_fragment =
      shader_stages[shader_stage_count++];
  shader_stage_fragment.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stage_fragment.pNext = nullptr;
  shader_stage_fragment.flags = 0;
  shader_stage_fragment.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stage_fragment.module = VK_NULL_HANDLE;
  shader_stage_fragment.pName = "main";
  shader_stage_fragment.pSpecializationInfo = nullptr;
  if (fragment_shader_override != VK_NULL_HANDLE) {
    // Use the override shader (for placeholder pipelines).
    shader_stage_fragment.module = fragment_shader_override;
  } else if (creation_arguments.pixel_shader) {
    assert_true(creation_arguments.pixel_shader->is_translated());
    if (!creation_arguments.pixel_shader->is_valid()) {
      return false;
    }
    shader_stage_fragment.module =
        creation_arguments.pixel_shader->shader_module();
    assert_true(shader_stage_fragment.module != VK_NULL_HANDLE);
  } else {
    if (edram_fragment_shader_interlock) {
      shader_stage_fragment.module = depth_only_fragment_shader_;
    }
  }
  if (shader_stage_fragment.module == VK_NULL_HANDLE) {
    --shader_stage_count;
  }

  VkPipelineVertexInputStateCreateInfo vertex_input_state = {};
  vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  switch (description.primitive_topology) {
    case PipelinePrimitiveTopology::kPointList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kTriangleStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleFan:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      break;
    case PipelinePrimitiveTopology::kLineListWithAdjacency:
      input_assembly_state.topology =
          VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kPatchList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    default:
      assert_unhandled_case(description.primitive_topology);
      return false;
  }
  input_assembly_state.primitiveRestartEnable =
      description.primitive_restart ? VK_TRUE : VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.depthClampEnable =
      description.depth_clamp_enable ? VK_TRUE : VK_FALSE;
  switch (description.polygon_mode) {
    case PipelinePolygonMode::kFill:
      rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
      break;
    case PipelinePolygonMode::kLine:
      rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
      break;
    case PipelinePolygonMode::kPoint:
      rasterization_state.polygonMode = VK_POLYGON_MODE_POINT;
      break;
    default:
      assert_unhandled_case(description.polygon_mode);
      return false;
  }
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  if (description.cull_front) {
    rasterization_state.cullMode |= VK_CULL_MODE_FRONT_BIT;
  }
  if (description.cull_back) {
    rasterization_state.cullMode |= VK_CULL_MODE_BACK_BIT;
  }
  rasterization_state.frontFace = description.front_face_clockwise
                                      ? VK_FRONT_FACE_CLOCKWISE
                                      : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Depth bias is dynamic (even toggling - pipeline creation is expensive).
  // "If no depth attachment is present, r is undefined" in the depth bias
  // formula, though Z has no effect on anything if a depth attachment is not
  // used (the guest shader can't access Z), enabling only when there's a
  // depth / stencil attachment for correctness.
  rasterization_state.depthBiasEnable =
      (!edram_fragment_shader_interlock &&
       (description.render_pass_key.depth_and_color_used & 0b1))
          ? VK_TRUE
          : VK_FALSE;
  // TODO(Triang3l): Wide lines.
  rasterization_state.lineWidth = 1.0f;

  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  if (description.render_pass_key.msaa_samples == xenos::MsaaSamples::k2X &&
      !render_target_cache_.IsMsaa2xSupported(
          !edram_fragment_shader_interlock &&
          description.render_pass_key.depth_and_color_used != 0)) {
    // Using sample 0 as 0 and 3 as 1 for 2x instead (not exactly the same
    // sample locations, but still top-left and bottom-right - however, this can
    // be adjusted with custom sample locations).
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    sample_mask = 0b1001;
    // TODO(Triang3l): Research sample mask behavior without attachments (in
    // Direct3D, it's completely ignored in this case).
    multisample_state.pSampleMask = &sample_mask;
  } else {
    multisample_state.rasterizationSamples = VkSampleCountFlagBits(
        uint32_t(1) << uint32_t(description.render_pass_key.msaa_samples));
  }

  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil_state.pNext = nullptr;
  if (!edram_fragment_shader_interlock) {
    if (description.depth_write_enable ||
        description.depth_compare_op != xenos::CompareFunction::kAlways) {
      depth_stencil_state.depthTestEnable = VK_TRUE;
      depth_stencil_state.depthWriteEnable =
          description.depth_write_enable ? VK_TRUE : VK_FALSE;
      depth_stencil_state.depthCompareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                      uint32_t(description.depth_compare_op));
    }
    if (description.stencil_test_enable) {
      depth_stencil_state.stencilTestEnable = VK_TRUE;
      depth_stencil_state.front.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_front_fail_op));
      depth_stencil_state.front.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_front_pass_op));
      depth_stencil_state.front.depthFailOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_front_depth_fail_op));
      depth_stencil_state.front.compareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                      uint32_t(description.stencil_front_compare_op));
      depth_stencil_state.back.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_back_fail_op));
      depth_stencil_state.back.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_back_pass_op));
      depth_stencil_state.back.depthFailOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_back_depth_fail_op));
      depth_stencil_state.back.compareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                      uint32_t(description.stencil_back_compare_op));
    }
  }

  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  VkPipelineColorBlendAttachmentState
      color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  if (!edram_fragment_shader_interlock) {
    uint32_t color_rts_used =
        description.render_pass_key.depth_and_color_used >> 1;
    {
      static constexpr VkBlendFactor kBlendFactorMap[] = {
          VK_BLEND_FACTOR_ZERO,
          VK_BLEND_FACTOR_ONE,
          VK_BLEND_FACTOR_SRC_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
          VK_BLEND_FACTOR_DST_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
          VK_BLEND_FACTOR_SRC_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          VK_BLEND_FACTOR_DST_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
          VK_BLEND_FACTOR_CONSTANT_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
          VK_BLEND_FACTOR_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
      };
      // 8 entries for safety since 3 bits from the guest are passed directly.
      static constexpr VkBlendOp kBlendOpMap[] = {VK_BLEND_OP_ADD,
                                                  VK_BLEND_OP_SUBTRACT,
                                                  VK_BLEND_OP_MIN,
                                                  VK_BLEND_OP_MAX,
                                                  VK_BLEND_OP_REVERSE_SUBTRACT,
                                                  VK_BLEND_OP_ADD,
                                                  VK_BLEND_OP_ADD,
                                                  VK_BLEND_OP_ADD};
      // Check if the shader pre-multiplies by blend factors for MIN/MAX.
      SpirvShaderTranslator::Modification pixel_shader_modification(
          description.pixel_shader_modification);
      bool rt0_rgb_premult =
          pixel_shader_modification.pixel.rt0_blend_rgb_factor_for_premult !=
          xenos::BlendFactor::kOne;
      bool rt0_a_premult =
          pixel_shader_modification.pixel.rt0_blend_a_factor_for_premult !=
          xenos::BlendFactor::kOne;

      uint32_t color_rts_remaining = color_rts_used;
      uint32_t color_rt_index;
      while (xe::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
        color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
        VkPipelineColorBlendAttachmentState& color_blend_attachment =
            color_blend_attachments[color_rt_index];
        const PipelineRenderTarget& color_rt =
            description.render_targets[color_rt_index];
        if (color_rt.src_color_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_color_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.color_blend_op != xenos::BlendOp::kAdd ||
            color_rt.src_alpha_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_alpha_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.alpha_blend_op != xenos::BlendOp::kAdd) {
          color_blend_attachment.blendEnable = VK_TRUE;
          color_blend_attachment.srcColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_color_blend_factor)];
          color_blend_attachment.dstColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_color_blend_factor)];
          color_blend_attachment.colorBlendOp =
              kBlendOpMap[uint32_t(color_rt.color_blend_op)];
          color_blend_attachment.srcAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_alpha_blend_factor)];
          color_blend_attachment.dstAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_alpha_blend_factor)];
          color_blend_attachment.alphaBlendOp =
              kBlendOpMap[uint32_t(color_rt.alpha_blend_op)];

          // If the shader pre-multiplies by the source blend factor for RT0
          // MIN/MAX, set the pipeline source factor to ONE since it's already
          // applied in the shader.
          if (color_rt_index == 0) {
            if (rt0_rgb_premult) {
              color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            }
            if (rt0_a_premult) {
              color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            }
          }
        }
        color_blend_attachment.colorWriteMask =
            VkColorComponentFlags(color_rt.color_write_mask);
      }
    }
    color_blend_state.attachmentCount = 32 - xe::lzcnt(color_rts_used);
    color_blend_state.pAttachments = color_blend_attachments;
  }

  std::array<VkDynamicState, 7> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  // Regardless of whether some of this state actually has any effect on the
  // pipeline, marking all as dynamic because otherwise, binding any pipeline
  // with such state not marked as dynamic will cause the dynamic state to be
  // invalidated (again, even if it has no effect).
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (!edram_fragment_shader_interlock) {
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_DEPTH_BIAS;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_REFERENCE;
  }

  // Tessellation state (only used when tessellation is active).
  VkPipelineTessellationStateCreateInfo tessellation_state = {};
  tessellation_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
  if (is_tessellated) {
    // Determine patch control point count based on mode and patch type.
    // For adaptive mode, we use the actual patch corner count (3 for triangles,
    // 4 for quads) since each control point has its own edge factor.
    // For discrete/continuous modes, we use 1 control point (the Xenos vertex
    // shader receives the patch index and computes all corners internally).
    if (description.tessellation_mode == PipelineTessellationMode::kAdaptive) {
      tessellation_state.patchControlPoints =
          (description.tessellation_patch ==
           PipelineTessellationPatchType::kTriangle)
              ? 3
              : 4;
    } else {
      tessellation_state.patchControlPoints = 1;
    }
  }

  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = nullptr;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stageCount = shader_stage_count;
  pipeline_create_info.pStages = shader_stages.data();
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState =
      is_tessellated ? &tessellation_state : nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout =
      creation_arguments.pipeline->second.pipeline_layout->GetPipelineLayout();
  pipeline_create_info.renderPass = creation_arguments.render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkPipeline pipeline;
  VkResult result = dfn.vkCreateGraphicsPipelines(
      device, vk_pipeline_cache_, 1, &pipeline_create_info, nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    if (creation_arguments.pixel_shader) {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X}, PS {:016X} "
          "(tessellated={}, result={})",
          creation_arguments.vertex_shader->shader().ucode_data_hash(),
          creation_arguments.pixel_shader->shader().ucode_data_hash(),
          is_tessellated, static_cast<int>(result));
    } else {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X} "
          "(tessellated={}, result={})",
          creation_arguments.vertex_shader->shader().ucode_data_hash(),
          is_tessellated, static_cast<int>(result));
    }
    return false;
  }

  // Store the new pipeline, handling placeholder hot-swap.
  VkPipeline old_pipeline =
      creation_arguments.pipeline->second.pipeline.exchange(
          pipeline, std::memory_order_acq_rel);

  if (old_pipeline != VK_NULL_HANDLE) {
    // We're replacing a placeholder pipeline with the real one.
    // Queue the old placeholder for deferred destruction, recording the
    // current submission number so we only destroy after the GPU is done.
    uint64_t current_submission = command_processor_.GetCurrentSubmission();
    {
      std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
      deferred_destroy_pipelines_.emplace_back(old_pipeline,
                                               current_submission);
    }
  }

  // Mark as no longer a placeholder (for the case where we just created real).
  if (!creating_placeholder) {
    creation_arguments.pipeline->second.is_placeholder.store(
        false, std::memory_order_release);
  }

  return true;
}

void VulkanPipelineCache::ProcessDeferredDestructions() {
  std::vector<VkShaderModule> modules_to_destroy;
  std::vector<VkPipeline> pipelines_to_destroy;

  uint64_t completed_submission = command_processor_.GetCompletedSubmission();

  {
    std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
    if (deferred_destroy_shader_modules_.empty() &&
        deferred_destroy_pipelines_.empty()) {
      return;
    }
    modules_to_destroy = std::move(deferred_destroy_shader_modules_);
    deferred_destroy_shader_modules_.clear();

    // Only destroy pipelines whose submission has completed on the GPU.
    // Keep pipelines that are still potentially in-flight.
    auto it = deferred_destroy_pipelines_.begin();
    while (it != deferred_destroy_pipelines_.end()) {
      if (it->second <= completed_submission) {
        // This submission has completed, safe to destroy.
        pipelines_to_destroy.push_back(it->first);
        it = deferred_destroy_pipelines_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Destroy the modules and pipelines now that we know GPU is done with them.
  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  for (VkShaderModule module : modules_to_destroy) {
    if (module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, module, nullptr);
    }
  }

  for (VkPipeline pipeline : pipelines_to_destroy) {
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
}

void VulkanPipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  ShutdownShaderStorage();

  shader_storage_title_id_ = title_id;

  if (!blocking) {
    startup_loading_ = true;
    if (completion_callback) {
      completion_callback = [this, orig = std::move(completion_callback)]() {
        startup_loading_ = false;
        orig();
      };
    } else {
      completion_callback = [this]() { startup_loading_ = false; };
    }
  }

  bool edram_fsi_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  ShaderStorageWriter<PipelineStoredDescription>::PipelineStorageConfig
      pipeline_config;
  pipeline_config.file_suffix =
      fmt::format(".{}.vk.xpso", edram_fsi_used ? "fsi" : "fbo");
  pipeline_config.api_magic = kPipelineStorageAPIMagicVulkan;
  pipeline_config.version =
      std::max(PipelineDescription::kVersion,
               SpirvShaderTranslator::Modification::kVersion);

  uint32_t storage_index = storage_writer_.storage_index() + 1;

  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
  if (!storage_writer_.InitializeShaderStorage(
          cache_root, title_id, pipeline_config,
          // Shader load callback.
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
              uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            VulkanShader* shader =
                LoadShader(type, ucode_dwords, ucode_dword_count);
            if (!shader || shader->ucode_storage_index() == storage_index) {
              return true;  // Continue reading.
            }
            shader->set_ucode_storage_index(storage_index);
            if (!shader->is_ucode_analyzed()) {
              shader->AnalyzeUcode(ucode_disasm_buffer_);
            }
            return true;
          },
          // Shader translate callback - handles parallel translation.
          [this, edram_fsi_used](const std::set<std::pair<uint64_t, uint64_t>>
                                     & translations_needed) {
            TranslateShadersForStorage(translations_needed, edram_fsi_used);
          },
          pipeline_stored_descriptions)) {
    if (completion_callback) {
      completion_callback();
    }
    return;
  }
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;

  // Load VkPipelineCache from disk if available.
  auto shader_storage_local_root = GetShaderStorageLocalRoot(cache_root);
  if (!std::filesystem::exists(shader_storage_local_root)) {
    std::error_code ec;
    std::filesystem::create_directories(shader_storage_local_root, ec);
  }
  vk_pipeline_cache_path_ =
      shader_storage_local_root /
      fmt::format("{:08X}.vk.bin", shader_storage_title_id_);

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Try to load existing VkPipelineCache data.
  std::vector<uint8_t> pipeline_cache_data;
  if (FILE* cache_file =
          xe::filesystem::OpenFile(vk_pipeline_cache_path_, "rb")) {
    xe::filesystem::Seek(cache_file, 0, SEEK_END);
    int64_t cache_size = xe::filesystem::Tell(cache_file);
    if (cache_size > 0) {
      pipeline_cache_data.resize(size_t(cache_size));
      xe::filesystem::Seek(cache_file, 0, SEEK_SET);
      pipeline_cache_data.resize(fread(pipeline_cache_data.data(), 1,
                                       pipeline_cache_data.size(), cache_file));
    }
    fclose(cache_file);
    XELOGI("Loaded {} bytes of VkPipelineCache data",
           pipeline_cache_data.size());
  }

  // Recreate the VkPipelineCache with the loaded data.
  if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineCache(device, vk_pipeline_cache_, nullptr);
    vk_pipeline_cache_ = VK_NULL_HANDLE;
  }
  VkPipelineCacheCreateInfo pipeline_cache_create_info = {};
  pipeline_cache_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  pipeline_cache_create_info.initialDataSize = pipeline_cache_data.size();
  pipeline_cache_create_info.pInitialData =
      pipeline_cache_data.empty() ? nullptr : pipeline_cache_data.data();
  if (dfn.vkCreatePipelineCache(device, &pipeline_cache_create_info, nullptr,
                                &vk_pipeline_cache_) != VK_SUCCESS) {
    XELOGW(
        "VulkanPipelineCache: Failed to create pipeline cache with "
        "initial data, creating empty cache");
    vk_pipeline_cache_ = VK_NULL_HANDLE;
    pipeline_cache_create_info.initialDataSize = 0;
    pipeline_cache_create_info.pInitialData = nullptr;
    dfn.vkCreatePipelineCache(device, &pipeline_cache_create_info, nullptr,
                              &vk_pipeline_cache_);
  }

  // Create pipelines from stored descriptions.
  if (!pipeline_stored_descriptions.empty()) {
    uint64_t pipeline_creation_start = xe::Clock::QueryHostTickCount();

    size_t pipelines_created = 0;
    size_t pipelines_already_exist = 0;
    size_t pipelines_vs_not_found = 0;
    size_t pipelines_vs_translation_missing = 0;
    size_t pipelines_ps_not_found = 0;
    size_t pipelines_ps_translation_missing = 0;
    size_t pipelines_render_pass_failed = 0;
    size_t pipelines_layout_failed = 0;
    size_t pipelines_requirements_not_met = 0;

    for (const PipelineStoredDescription& pipeline_stored_description :
         pipeline_stored_descriptions) {
      const PipelineDescription& pipeline_description =
          pipeline_stored_description.description;

      // Skip pipelines not supported by this device.
      if (!ArePipelineRequirementsMet(pipeline_description)) {
        ++pipelines_requirements_not_met;
        continue;
      }

      // Skip already known pipelines.
      auto it = pipelines_.find(pipeline_description);
      if (it != pipelines_.end()) {
        ++pipelines_already_exist;
        continue;
      }

      // Look up vertex shader.
      auto vertex_shader_it =
          shaders_.find(pipeline_description.vertex_shader_hash);
      if (vertex_shader_it == shaders_.end()) {
        ++pipelines_vs_not_found;
        continue;
      }
      VulkanShader* vertex_shader = vertex_shader_it->second;
      VulkanShader::VulkanTranslation* vertex_translation =
          static_cast<VulkanShader::VulkanTranslation*>(
              vertex_shader->GetTranslation(
                  pipeline_description.vertex_shader_modification));
      if (!vertex_translation || !vertex_translation->is_translated() ||
          !vertex_translation->is_valid()) {
        ++pipelines_vs_translation_missing;
        continue;
      }

      // Look up pixel shader if present.
      VulkanShader* pixel_shader = nullptr;
      VulkanShader::VulkanTranslation* pixel_translation = nullptr;
      if (pipeline_description.pixel_shader_hash) {
        auto pixel_shader_it =
            shaders_.find(pipeline_description.pixel_shader_hash);
        if (pixel_shader_it == shaders_.end()) {
          ++pipelines_ps_not_found;
          continue;
        }
        pixel_shader = pixel_shader_it->second;
        pixel_translation = static_cast<VulkanShader::VulkanTranslation*>(
            pixel_shader->GetTranslation(
                pipeline_description.pixel_shader_modification));
        if (!pixel_translation || !pixel_translation->is_translated() ||
            !pixel_translation->is_valid()) {
          ++pipelines_ps_translation_missing;
          continue;
        }
      }

      // Get render pass.
      VkRenderPass render_pass =
          render_target_cache_.GetPath() ==
                  RenderTargetCache::Path::kPixelShaderInterlock
              ? render_target_cache_.GetFragmentShaderInterlockRenderPass()
              : render_target_cache_.GetHostRenderTargetsRenderPass(
                    pipeline_description.render_pass_key);
      if (render_pass == VK_NULL_HANDLE) {
        ++pipelines_render_pass_failed;
        continue;
      }

      // Get pipeline layout.
      const PipelineLayoutProvider* pipeline_layout =
          command_processor_.GetPipelineLayout(
              pixel_shader
                  ? pixel_shader->GetTextureBindingsAfterTranslation().size()
                  : 0,
              pixel_shader
                  ? pixel_shader->GetSamplerBindingsAfterTranslation().size()
                  : 0,
              vertex_shader->GetTextureBindingsAfterTranslation().size(),
              vertex_shader->GetSamplerBindingsAfterTranslation().size());
      if (!pipeline_layout) {
        ++pipelines_layout_failed;
        continue;
      }

      // Get geometry shader if needed.
      VkShaderModule geometry_shader = VK_NULL_HANDLE;
      if (pipeline_description.geometry_shader !=
          PipelineGeometryShader::kNone) {
        GeometryShaderKey geometry_shader_key;
        GetGeometryShaderKey(
            pipeline_description.geometry_shader,
            SpirvShaderTranslator::Modification(
                vertex_translation->modification()),
            SpirvShaderTranslator::Modification(
                pixel_translation ? pixel_translation->modification() : 0),
            geometry_shader_key);
        geometry_shader = GetGeometryShader(geometry_shader_key);
        if (geometry_shader == VK_NULL_HANDLE) {
          continue;
        }
      }

      // Get tessellation shaders if needed.
      VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
      VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
      if (pipeline_description.tessellation_mode !=
          PipelineTessellationMode::kNone) {
        tessellation_vertex_shader =
            GetTessellationVertexShader(pipeline_description.tessellation_mode);
        bool use_control_point_count =
            (pipeline_description.tessellation_mode ==
             PipelineTessellationMode::kAdaptive);
        tessellation_control_shader = GetTessellationControlShader(
            pipeline_description.tessellation_mode,
            pipeline_description.tessellation_patch, use_control_point_count);
        if (tessellation_vertex_shader == VK_NULL_HANDLE ||
            tessellation_control_shader == VK_NULL_HANDLE) {
          continue;
        }
      }

      // Create the pipeline entry.
      auto& pipeline_pair =
          *pipelines_.emplace(pipeline_description, Pipeline(pipeline_layout))
               .first;

      // Queue for creation.
      if (!creation_threads_.empty()) {
        // Calculate priority based on whether shader writes to visible RTs.
        uint8_t priority = 0;
        if (pixel_shader) {
          uint32_t bound_rts =
              (pipeline_description.render_targets[0].color_write_mask ? 1
                                                                       : 0) |
              (pipeline_description.render_targets[1].color_write_mask ? 2
                                                                       : 0) |
              (pipeline_description.render_targets[2].color_write_mask ? 4
                                                                       : 0) |
              (pipeline_description.render_targets[3].color_write_mask ? 8 : 0);
          priority = pipeline_util::CalculatePipelinePriority(
              bound_rts, pixel_shader->writes_color_targets(),
              pixel_shader->writes_depth());
        }

        std::lock_guard<std::mutex> lock(creation_request_lock_);
        PipelineCreationArguments creation_arguments;
        creation_arguments.pipeline = &pipeline_pair;
        creation_arguments.vertex_shader = vertex_translation;
        creation_arguments.pixel_shader = pixel_translation;
        creation_arguments.geometry_shader = geometry_shader;
        creation_arguments.tessellation_vertex_shader =
            tessellation_vertex_shader;
        creation_arguments.tessellation_control_shader =
            tessellation_control_shader;
        creation_arguments.render_pass = render_pass;
        creation_arguments.priority = priority;
        creation_queue_.push(creation_arguments);
        creation_request_cond_.notify_one();
      } else {
        // No creation threads - create synchronously.
        PipelineCreationArguments creation_arguments;
        creation_arguments.pipeline = &pipeline_pair;
        creation_arguments.vertex_shader = vertex_translation;
        creation_arguments.pixel_shader = pixel_translation;
        creation_arguments.geometry_shader = geometry_shader;
        creation_arguments.tessellation_vertex_shader =
            tessellation_vertex_shader;
        creation_arguments.tessellation_control_shader =
            tessellation_control_shader;
        creation_arguments.render_pass = render_pass;
        EnsurePipelineCreated(creation_arguments);
      }

      ++pipelines_created;
    }

    if (!creation_threads_.empty()) {
      if (blocking) {
        // Blocking mode: wait for all pipelines to be created.
        bool await_creation_completion_event;
        {
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          await_creation_completion_event =
              !creation_queue_.empty() || creation_threads_busy_ != 0;
          if (await_creation_completion_event) {
            creation_completion_event_->Reset();
            creation_completion_set_event_.store(true,
                                                 std::memory_order_release);
          }
        }
        if (await_creation_completion_event) {
          creation_request_cond_.notify_one();
          xe::threading::Wait(creation_completion_event_.get(), false);
        }
      } else {
        // Non-blocking mode: store callback for later invocation.
        std::lock_guard<std::mutex> lock(creation_request_lock_);
        if (creation_queue_.empty() && creation_threads_busy_ == 0) {
          // No work pending - callback will be invoked at end of function.
        } else {
          creation_completion_callback_ = std::move(completion_callback);
          completion_callback = nullptr;  // Prevent invocation at end
        }
      }
    }

    XELOGI("Pipeline cache: {} created, {} already exist, {} total in {} ms",
           pipelines_created, pipelines_already_exist,
           pipeline_stored_descriptions.size(),
           (xe::Clock::QueryHostTickCount() - pipeline_creation_start) * 1000 /
               xe::Clock::QueryHostTickFrequency());

    if (pipelines_vs_not_found || pipelines_vs_translation_missing ||
        pipelines_ps_not_found || pipelines_ps_translation_missing ||
        pipelines_render_pass_failed || pipelines_layout_failed ||
        pipelines_requirements_not_met) {
      XELOGI(
          "Pipeline cache skipped: {} VS not found, {} VS translation missing, "
          "{} PS not found, {} PS translation missing, {} render pass failed, "
          "{} layout failed, {} requirements not met",
          pipelines_vs_not_found, pipelines_vs_translation_missing,
          pipelines_ps_not_found, pipelines_ps_translation_missing,
          pipelines_render_pass_failed, pipelines_layout_failed,
          pipelines_requirements_not_met);
    }
  }

  // Invoke completion callback if no async work was queued.
  if (completion_callback) {
    completion_callback();
  }
}

void VulkanPipelineCache::ShutdownShaderStorage() {
  // Save VkPipelineCache to disk before shutting down storage.
  if (vk_pipeline_cache_ != VK_NULL_HANDLE &&
      !vk_pipeline_cache_path_.empty()) {
    const ui::vulkan::VulkanDevice* const vulkan_device =
        command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();

    size_t cache_size = 0;
    if (dfn.vkGetPipelineCacheData(device, vk_pipeline_cache_, &cache_size,
                                   nullptr) == VK_SUCCESS &&
        cache_size > 0) {
      std::vector<uint8_t> cache_data(cache_size);
      if (dfn.vkGetPipelineCacheData(device, vk_pipeline_cache_, &cache_size,
                                     cache_data.data()) == VK_SUCCESS) {
        if (FILE* cache_file =
                xe::filesystem::OpenFile(vk_pipeline_cache_path_, "wb")) {
          fwrite(cache_data.data(), 1, cache_size, cache_file);
          fclose(cache_file);
          XELOGI("Saved {} bytes of VkPipelineCache data", cache_size);
        }
      }
    }
  }
  vk_pipeline_cache_path_.clear();

  // Shut down the storage writer (closes files, stops write thread).
  storage_writer_.ShutdownShaderStorage();
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;
  shader_storage_title_id_ = 0;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
