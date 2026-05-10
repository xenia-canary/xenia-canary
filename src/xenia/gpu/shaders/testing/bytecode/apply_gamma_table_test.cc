/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/shaders/testing/util/compute_test_harness.h"
#include "xenia/gpu/shaders/testing/util/spirv_cross_wrapper.h"
#include "xenia/gpu/shaders/testing/util/vulkan_test_device.h"

#include <cstdint>

// Define BYTE for D3D12 headers
#ifndef BYTE
typedef uint8_t BYTE;
#endif

// Include DXBC bytecode
#define apply_gamma_table_cs apply_gamma_table_cs_dxbc
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/apply_gamma_table_cs.h"
#undef apply_gamma_table_cs

#define apply_gamma_table_fxaa_luma_cs apply_gamma_table_fxaa_luma_cs_dxbc
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/apply_gamma_table_fxaa_luma_cs.h"
#undef apply_gamma_table_fxaa_luma_cs

// Include SPIR-V bytecode
#define apply_gamma_table_cs apply_gamma_table_cs_spirv
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_cs.h"
#undef apply_gamma_table_cs

#define apply_gamma_table_fxaa_luma_cs apply_gamma_table_fxaa_luma_cs_spirv
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_fxaa_luma_cs.h"
#undef apply_gamma_table_fxaa_luma_cs

using namespace xe::gpu::shaders::testing;

// Tests for the production apply_gamma_table compute shader
// Simple lookup table based gamma correction

// Helper to convert half-float (FP16) to float
static float Half2Float(uint16_t h) {
  uint32_t sign = (h & 0x8000) << 16;
  uint32_t exp = (h & 0x7C00) >> 10;
  uint32_t frac = (h & 0x03FF);

  if (exp == 0) {
    if (frac == 0) {
      return 0.0f;  // Zero
    }
    // Denormalized
    exp = 1;
    while ((frac & 0x0400) == 0) {
      frac <<= 1;
      exp--;
    }
    frac &= 0x03FF;
  } else if (exp == 31) {
    // Inf or NaN
    return (frac == 0) ? INFINITY : NAN;
  }

  uint32_t f = sign | ((exp + 112) << 23) | (frac << 13);
  float result;
  std::memcpy(&result, &f, sizeof(float));
  return result;
}

// Helper to pack RGB into R10G10B10A2 format (BGR order as per shader)
// "blue in bits 0:9, green in 10:19, red in 20:29"
static uint32_t PackR10G10B10A2_BGR(uint32_t r, uint32_t g, uint32_t b,
                                    uint32_t a = 0) {
  return (b & 0x3FF) | ((g & 0x3FF) << 10) | ((r & 0x3FF) << 20) |
         ((a & 0x3) << 30);
}

TEST_CASE("apply_gamma_table: DXBC-converted production shader",
          "[shader][apply_gamma_table][cross_backend][dxbc]") {
  VulkanTestDevice device;
  if (!device.Initialize()) {
    WARN("Vulkan not available, skipping test");
    return;
  }

  // Load DXBC and convert to SPIR-V
  std::vector<uint8_t> dxbc_bytes(
      apply_gamma_table_cs_dxbc,
      apply_gamma_table_cs_dxbc + sizeof(apply_gamma_table_cs_dxbc));

  std::vector<uint32_t> spirv = SPIRVCrossWrapper::DXBCToSPIRV(dxbc_bytes);

  if (spirv.empty()) {
    FAIL("DXBC to SPIR-V conversion failed: "
         << SPIRVCrossWrapper::GetLastError());
  }

  // Use SPIRV reflection to detect descriptor bindings
  auto bindings = SPIRVCrossWrapper::ReflectDescriptorBindings(spirv);

  uint32_t gamma_ramp_set = 0, gamma_ramp_binding = 0;
  uint32_t source_tex_set = 0, source_tex_binding = 0;
  uint32_t output_img_set = 0, output_img_binding = 0;

  for (const auto& binding : bindings) {
    // Map resources based on name
    if (binding.name == "t0") {  // gamma ramp
      gamma_ramp_set = binding.set;
      gamma_ramp_binding = binding.binding;
    } else if (binding.name == "t1") {  // source texture
      source_tex_set = binding.set;
      source_tex_binding = binding.binding;
    } else if (binding.name == "u0") {  // output image
      output_img_set = binding.set;
      output_img_binding = binding.binding;
    }
  }

  ComputeTestHarness harness(&device, spirv);
  REQUIRE(harness.IsValid());

  const uint32_t width = 4;
  const uint32_t height = 2;

  struct PushConstants {
    uint32_t width;
    uint32_t height;
  } push_const = {width, height};
  harness.SetPushConstants(push_const);

  // DXBC converter puts push constants into cb1_struct uniform buffer
  harness.SetUniformBuffer(0, push_const, 0);

  // Create gamma lookup table (256 entries)
  // Format: R10G10B10A2 with BGR packing
  // For testing: create a simple power curve (gamma 2.2)
  std::vector<uint32_t> gamma_table(256);
  for (uint32_t i = 0; i < 256; ++i) {
    float normalized = float(i) / 255.0f;
    float corrected = std::pow(normalized, 1.0f / 2.2f);  // Gamma correction
    uint32_t output_10bit = uint32_t(corrected * 1023.0f + 0.5f);

    // Pack as BGR in R10G10B10A2
    gamma_table[i] =
        PackR10G10B10A2_BGR(output_10bit, output_10bit, output_10bit, 0);
  }

  // Bind gamma table as texel buffer (R10G10B10A2 format)
  harness.SetTexelBuffer(gamma_ramp_binding, gamma_table,
                         vk::Format::eA2B10G10R10UnormPack32, gamma_ramp_set);

  // Create source texture with test values (8-bit)
  std::vector<float> source_tex(width * height * 4);
  uint8_t test_values[] = {0, 64, 128, 192, 32, 96, 160, 224};

  for (uint32_t i = 0; i < width * height; ++i) {
    uint32_t idx = i * 4;
    source_tex[idx + 0] = float(test_values[i]) / 255.0f;  // R
    source_tex[idx + 1] = float(test_values[i]) / 255.0f;  // G
    source_tex[idx + 2] = float(test_values[i]) / 255.0f;  // B
    source_tex[idx + 3] = 1.0f;                            // A
  }

  harness.SetTexture2D(source_tex_binding, width, height,
                       vk::Format::eR32G32B32A32Sfloat, source_tex,
                       source_tex_set);

  harness.AllocateOutputImage2D(output_img_binding, width, height,
                                vk::Format::eR16G16B16A16Sfloat,
                                output_img_set);

  harness.Dispatch(1, 1, 1);

  auto output =
      harness.ReadOutputImage2D<uint16_t>(output_img_binding, output_img_set);
  REQUIRE(output.size() == width * height * 4);

  // Validate lookup table output
  for (uint32_t i = 0; i < width * height; ++i) {
    uint8_t input_val = test_values[i];

    // Calculate expected output from gamma table
    uint32_t packed = gamma_table[input_val];
    uint32_t expected_r = (packed >> 20) & 0x3FF;
    uint32_t expected_g = (packed >> 10) & 0x3FF;
    uint32_t expected_b = (packed >> 0) & 0x3FF;

    float expected_r_norm = float(expected_r) / 1023.0f;
    float expected_g_norm = float(expected_g) / 1023.0f;
    float expected_b_norm = float(expected_b) / 1023.0f;

    // Read output RGB channels
    float actual_r = Half2Float(output[i * 4 + 0]);
    float actual_g = Half2Float(output[i * 4 + 1]);
    float actual_b = Half2Float(output[i * 4 + 2]);

    REQUIRE(actual_r == Approx(expected_r_norm).margin(0.01f));
    REQUIRE(actual_g == Approx(expected_g_norm).margin(0.01f));
    REQUIRE(actual_b == Approx(expected_b_norm).margin(0.01f));
  }

  // Verify alpha channel is 1.0 (not FXAA luma variant)
  for (uint32_t i = 0; i < width * height; ++i) {
    float alpha = Half2Float(output[i * 4 + 3]);
    REQUIRE(alpha == Approx(1.0f).margin(0.001f));
  }
}

TEST_CASE("apply_gamma_table: Native SPIR-V production shader",
          "[shader][apply_gamma_table][cross_backend][spirv]") {
  VulkanTestDevice device;
  if (!device.Initialize()) {
    WARN("Vulkan not available, skipping test");
    return;
  }

  std::vector<uint32_t> spirv(
      apply_gamma_table_cs_spirv,
      apply_gamma_table_cs_spirv +
          sizeof(apply_gamma_table_cs_spirv) / sizeof(uint32_t));

  REQUIRE(SPIRVCrossWrapper::ValidateSPIRV(spirv));

  ComputeTestHarness harness(&device, spirv);
  REQUIRE(harness.IsValid());

  const uint32_t width = 4;
  const uint32_t height = 2;

  struct PushConstants {
    uint32_t width;
    uint32_t height;
  } push_const = {width, height};
  harness.SetPushConstants(push_const);

  // Create gamma lookup table with identity mapping
  std::vector<uint32_t> gamma_table(256);
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t output_10bit = (i * 1023) / 255;  // Scale 8-bit to 10-bit
    gamma_table[i] =
        PackR10G10B10A2_BGR(output_10bit, output_10bit, output_10bit, 0);
  }

  harness.SetTexelBuffer(0, gamma_table, vk::Format::eA2B10G10R10UnormPack32,
                         0);

  // Create source texture
  std::vector<float> source_tex(width * height * 4);
  uint8_t test_values[] = {0, 64, 128, 192, 32, 96, 160, 224};

  for (uint32_t i = 0; i < width * height; ++i) {
    uint32_t idx = i * 4;
    source_tex[idx + 0] = float(test_values[i]) / 255.0f;
    source_tex[idx + 1] = float(test_values[i]) / 255.0f;
    source_tex[idx + 2] = float(test_values[i]) / 255.0f;
    source_tex[idx + 3] = 1.0f;
  }

  harness.SetTexture2D(0, width, height, vk::Format::eR32G32B32A32Sfloat,
                       source_tex, 1);

  harness.AllocateOutputImage2D(0, width, height,
                                vk::Format::eR16G16B16A16Sfloat, 2);

  harness.Dispatch(1, 1, 1);

  auto output = harness.ReadOutputImage2D<uint16_t>(0, 2);
  REQUIRE(output.size() == width * height * 4);

  // Validate identity mapping
  for (uint32_t i = 0; i < width * height; ++i) {
    uint8_t input_val = test_values[i];
    float expected = float((input_val * 1023) / 255) / 1023.0f;

    float actual_r = Half2Float(output[i * 4 + 0]);
    float actual_g = Half2Float(output[i * 4 + 1]);
    float actual_b = Half2Float(output[i * 4 + 2]);

    REQUIRE(actual_r == Approx(expected).margin(0.01f));
    REQUIRE(actual_g == Approx(expected).margin(0.01f));
    REQUIRE(actual_b == Approx(expected).margin(0.01f));
  }

  // Verify alpha channel
  for (uint32_t i = 0; i < width * height; ++i) {
    float alpha = Half2Float(output[i * 4 + 3]);
    REQUIRE(alpha == Approx(1.0f).margin(0.001f));
  }
}

TEST_CASE("apply_gamma_table: FXAA luma variant",
          "[shader][apply_gamma_table][fxaa_luma]") {
  VulkanTestDevice device;
  if (!device.Initialize()) {
    WARN("Vulkan not available, skipping test");
    return;
  }

  std::vector<uint32_t> spirv(
      apply_gamma_table_fxaa_luma_cs_spirv,
      apply_gamma_table_fxaa_luma_cs_spirv +
          sizeof(apply_gamma_table_fxaa_luma_cs_spirv) / sizeof(uint32_t));

  REQUIRE(SPIRVCrossWrapper::ValidateSPIRV(spirv));

  ComputeTestHarness harness(&device, spirv);
  REQUIRE(harness.IsValid());

  const uint32_t width = 4;
  const uint32_t height = 2;

  struct PushConstants {
    uint32_t width;
    uint32_t height;
  } push_const = {width, height};
  harness.SetPushConstants(push_const);

  // Create identity gamma table
  std::vector<uint32_t> gamma_table(256);
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t output_10bit = (i * 1023) / 255;
    gamma_table[i] =
        PackR10G10B10A2_BGR(output_10bit, output_10bit, output_10bit, 0);
  }

  harness.SetTexelBuffer(0, gamma_table, vk::Format::eA2B10G10R10UnormPack32,
                         0);

  // Create source with different RGB values to test luma
  std::vector<float> source_tex(width * height * 4);
  uint8_t test_values[][3] = {
      {128, 128, 128},  // Gray
      {255, 0, 0},      // Red
      {0, 255, 0},      // Green
      {0, 0, 255},      // Blue
      {128, 64, 32},    // Mixed
      {192, 128, 64},   // Mixed
      {64, 192, 128},   // Mixed
      {32, 96, 224},    // Mixed
  };

  for (uint32_t i = 0; i < width * height; ++i) {
    uint32_t idx = i * 4;
    source_tex[idx + 0] = float(test_values[i][0]) / 255.0f;
    source_tex[idx + 1] = float(test_values[i][1]) / 255.0f;
    source_tex[idx + 2] = float(test_values[i][2]) / 255.0f;
    source_tex[idx + 3] = 1.0f;
  }

  harness.SetTexture2D(0, width, height, vk::Format::eR32G32B32A32Sfloat,
                       source_tex, 1);

  harness.AllocateOutputImage2D(0, width, height,
                                vk::Format::eR16G16B16A16Sfloat, 2);

  harness.Dispatch(1, 1, 1);

  auto output = harness.ReadOutputImage2D<uint16_t>(0, 2);
  REQUIRE(output.size() == width * height * 4);

  // Validate RGB and luma
  for (uint32_t i = 0; i < width * height; ++i) {
    float expected_rgb[3];
    for (uint32_t c = 0; c < 3; ++c) {
      uint8_t input_val = test_values[i][c];
      float expected = float((input_val * 1023) / 255) / 1023.0f;
      expected_rgb[c] = expected;

      float actual = Half2Float(output[i * 4 + c]);
      REQUIRE(actual == Approx(expected).margin(0.01f));
    }

    // Verify alpha contains perceptual luma
    float expected_luma = expected_rgb[0] * 0.299f + expected_rgb[1] * 0.587f +
                          expected_rgb[2] * 0.114f;
    float actual_luma = Half2Float(output[i * 4 + 3]);
    REQUIRE(actual_luma == Approx(expected_luma).margin(0.01f));
  }
}

TEST_CASE("apply_gamma_table: Non-linear gamma curves",
          "[shader][apply_gamma_table][curves]") {
  VulkanTestDevice device;
  if (!device.Initialize()) {
    WARN("Vulkan not available, skipping test");
    return;
  }

  std::vector<uint32_t> spirv(
      apply_gamma_table_cs_spirv,
      apply_gamma_table_cs_spirv +
          sizeof(apply_gamma_table_cs_spirv) / sizeof(uint32_t));

  REQUIRE(SPIRVCrossWrapper::ValidateSPIRV(spirv));

  ComputeTestHarness harness(&device, spirv);
  REQUIRE(harness.IsValid());

  const uint32_t width = 8;
  const uint32_t height = 1;

  struct PushConstants {
    uint32_t width;
    uint32_t height;
  } push_const = {width, height};
  harness.SetPushConstants(push_const);

  // Create non-linear gamma table (inverted curve)
  std::vector<uint32_t> gamma_table(256);
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t inverted = 255 - i;
    uint32_t output_10bit = (inverted * 1023) / 255;
    gamma_table[i] =
        PackR10G10B10A2_BGR(output_10bit, output_10bit, output_10bit, 0);
  }

  harness.SetTexelBuffer(0, gamma_table, vk::Format::eA2B10G10R10UnormPack32,
                         0);

  // Test various input values
  std::vector<float> source_tex(width * height * 4);
  uint8_t test_values[] = {0, 32, 64, 96, 128, 160, 192, 224};

  for (uint32_t i = 0; i < width; ++i) {
    uint32_t idx = i * 4;
    source_tex[idx + 0] = float(test_values[i]) / 255.0f;
    source_tex[idx + 1] = float(test_values[i]) / 255.0f;
    source_tex[idx + 2] = float(test_values[i]) / 255.0f;
    source_tex[idx + 3] = 1.0f;
  }

  harness.SetTexture2D(0, width, height, vk::Format::eR32G32B32A32Sfloat,
                       source_tex, 1);

  harness.AllocateOutputImage2D(0, width, height,
                                vk::Format::eR16G16B16A16Sfloat, 2);

  harness.Dispatch(1, 1, 1);

  auto output = harness.ReadOutputImage2D<uint16_t>(0, 2);
  REQUIRE(output.size() == width * height * 4);

  // Validate inverted curve
  for (uint32_t i = 0; i < width; ++i) {
    uint8_t input_val = test_values[i];
    uint8_t inverted = 255 - input_val;
    float expected = float((inverted * 1023) / 255) / 1023.0f;

    float actual_r = Half2Float(output[i * 4 + 0]);
    float actual_g = Half2Float(output[i * 4 + 1]);
    float actual_b = Half2Float(output[i * 4 + 2]);

    REQUIRE(actual_r == Approx(expected).margin(0.01f));
    REQUIRE(actual_g == Approx(expected).margin(0.01f));
    REQUIRE(actual_b == Approx(expected).margin(0.01f));
  }
}
