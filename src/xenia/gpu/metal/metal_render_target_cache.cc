/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_render_target_cache.h"
#include "xenia/gpu/gpu_flags.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "third_party/stb/stb_image_write.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_heap_pool.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/shaders/bytecode/metal/host_depth_store_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/host_depth_store_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/host_depth_store_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_128bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_128bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_16bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_16bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_8bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_8bpp_scaled_cs.h"

#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(
    metal_allow_gamma_unorm16, false,
    "Allow gamma_render_target_as_unorm16 on Metal despite known issues",
    "Metal");
DEFINE_bool(metal_transfer_fast_divmod, true,
            "Use fast exact div/mod in Metal transfer shaders", "Metal");
DEFINE_bool(metal_transfer_tile_instancing, false,
            "Use per-tile instanced draws for Metal transfer shaders", "Metal");
DEFINE_bool(
    metal_transfer_msaa_sample_id, true,
    "Use sample_id in Metal transfer shaders for MSAA (sample-rate shading)",
    "Metal");
DEFINE_int32(metal_memory_log_rate, 0,
             "Log Metal render target/pipeline/instance buffer sizes every N "
             "frames (0 to disable)",
             "Metal");
DEFINE_bool(metal_use_heaps, true,
            "Use MTLHeap-backed texture allocations in Metal to reduce "
            "allocation overhead and fragmentation.",
            "Metal");
DEFINE_int32(metal_heap_min_bytes, 33554432,
             "Minimum heap size (bytes) for Metal heap allocations.", "Metal");

namespace xe {
namespace gpu {
namespace metal {

namespace {

class ScopedAutoreleasePool {
 public:
  ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}
  ~ScopedAutoreleasePool() {
    if (pool_) {
      pool_->release();
    }
  }

  ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
  ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

 private:
  NS::AutoreleasePool* pool_;
};

#if XE_PLATFORM_IOS
constexpr size_t kTransferTileInstanceBufferMaxBytes =
    64ull * 1024ull * 1024ull;
constexpr size_t kTransferTileInstanceSoftBaseBytes = 4ull * 1024ull * 1024ull;
constexpr size_t kTransferTileInstanceSoftLowCoverageBytes =
    8ull * 1024ull * 1024ull;
constexpr uint64_t kTransferTileInstanceLowCoverageRatioDivisor = 8ull;
constexpr uint64_t kTransferTileInstanceMediumCoverageRatioDivisor = 3ull;
constexpr size_t kTransferTileInstanceSmallRectPenaltyCount = 4;
constexpr size_t kTransferTileInstanceSmallRectPenaltyNumerator = 1;
constexpr size_t kTransferTileInstanceSmallRectPenaltyDenominator = 2;
constexpr size_t kTransferTileInstanceNearCapReserveBytes =
    8ull * 1024ull * 1024ull;
constexpr size_t kTransferTileInstanceNearCapUsagePercent = 84;
#else
constexpr size_t kTransferTileInstanceBufferMaxBytes =
    256ull * 1024ull * 1024ull;
constexpr size_t kTransferTileInstanceSoftBaseBytes = 32ull * 1024ull * 1024ull;
constexpr size_t kTransferTileInstanceSoftLowCoverageBytes =
    64ull * 1024ull * 1024ull;
constexpr uint64_t kTransferTileInstanceLowCoverageRatioDivisor = 3ull;
constexpr uint64_t kTransferTileInstanceMediumCoverageRatioDivisor = 2ull;
constexpr size_t kTransferTileInstanceSmallRectPenaltyCount = 2;
constexpr size_t kTransferTileInstanceSmallRectPenaltyNumerator = 3;
constexpr size_t kTransferTileInstanceSmallRectPenaltyDenominator = 4;
constexpr size_t kTransferTileInstanceNearCapReserveBytes =
    32ull * 1024ull * 1024ull;
constexpr size_t kTransferTileInstanceNearCapUsagePercent = 90;
#endif

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

// Packing formats for transferring host RT contents to the EDRAM buffer.
// Keep numeric values in sync with Metal dump shaders in
// InitializeEdramComputeShaders.
enum class MetalEdramDumpFormat : uint32_t {
  kColorRGBA8 = 0,
  kColorRGB10A2Unorm = 1,
  kColorRGB10A2Float = 2,
  kColorRG16Snorm = 3,
  kColorRG16Float = 4,
  kColorR32Float = 5,
  kColorRGBA16Snorm = 6,
  kColorRGBA16Float = 7,
  kColorRGBA16Unorm = 8,
  kColorRG32Float = 9,
  kDepthD24S8 = 16,
  kDepthD24FS8 = 17,
};

constexpr uint32_t kMetalEdramDumpFlagHasStencil = 1u << 0;
constexpr uint32_t kMetalEdramDumpFlagDepthRound = 1u << 1;
constexpr uint32_t kMetalEdramDumpFlagGammaAsLinear = 1u << 2;

struct DebugColor {
  float r;
  float g;
  float b;
  float a;
};

uint32_t FloatToBits(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float BitsToFloat(uint32_t value) {
  float out = 0.0f;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

float HalfToFloat(uint16_t value) {
  uint32_t sign = (value >> 15) & 1u;
  uint32_t exponent = (value >> 10) & 0x1Fu;
  uint32_t mantissa = value & 0x3FFu;
  if (exponent == 0u) {
    if (mantissa == 0u) {
      return sign ? -0.0f : 0.0f;
    }
    float base = float(mantissa) * (1.0f / 1024.0f);
    float result = std::ldexp(base, -14);
    return sign ? -result : result;
  }
  if (exponent == 31u) {
    float inf = std::numeric_limits<float>::infinity();
    return sign ? -inf : inf;
  }
  float base = 1.0f + float(mantissa) * (1.0f / 1024.0f);
  float result = std::ldexp(base, int(exponent) - 15);
  return sign ? -result : result;
}

uint16_t FloatToHalf(float value) {
  uint32_t bits = FloatToBits(value);
  uint32_t sign = (bits >> 16) & 0x8000u;
  int exponent = int((bits >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFFu;
  if (exponent <= 0) {
    if (exponent < -10) {
      return uint16_t(sign);
    }
    mantissa |= 0x800000u;
    uint32_t shift = uint32_t(14 - exponent);
    uint32_t half = mantissa >> shift;
    if ((mantissa >> (shift - 1u)) & 1u) {
      ++half;
    }
    return uint16_t(sign | half);
  }
  if (exponent >= 31) {
    return uint16_t(sign | 0x7C00u);
  }
  uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
  if (mantissa & 0x1000u) {
    ++half;
  }
  return uint16_t(sign | half);
}

uint32_t PackUnorm(float value, float scale) {
  float clamped = std::min(std::max(value, 0.0f), 1.0f);
  return uint32_t(clamped * scale + 0.5f);
}

uint32_t PackSnorm16(float value) {
  float clamped = std::min(std::max(value, -1.0f), 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint32_t(packed) & 0xFFFFu;
}

uint32_t XePreClampedFloat32To7e3(float value) {
  uint32_t f32 = FloatToBits(value);
  uint32_t biased_f32;
  if (f32 < 0x3E800000u) {
    uint32_t f32_exp = f32 >> 23u;
    uint32_t shift = 125u - f32_exp;
    shift = std::min(shift, 24u);
    uint32_t mantissa = (f32 & 0x7FFFFFu) | 0x800000u;
    biased_f32 = mantissa >> shift;
  } else {
    biased_f32 = f32 + 0xC2000000u;
  }
  uint32_t round_bit = (biased_f32 >> 16u) & 1u;
  uint32_t f10 = biased_f32 + 0x7FFFu + round_bit;
  return (f10 >> 16u) & 0x3FFu;
}

uint32_t XeUnclampedFloat32To7e3(float value) {
  if (!std::isfinite(value)) {
    value = 0.0f;
  }
  float clamped = std::min(std::max(value, 0.0f), 31.875f);
  return XePreClampedFloat32To7e3(clamped);
}

float XeFloat7e3To32(uint32_t f10) {
  f10 &= 0x3FFu;
  if (!f10) {
    return 0.0f;
  }
  uint32_t mantissa = f10 & 0x7Fu;
  uint32_t exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint32_t lzcnt = 0;
    if (mantissa != 0u) {
      lzcnt = uint32_t(__builtin_clz(mantissa)) - 24u;
    }
    exponent = uint32_t(int32_t(1) - int32_t(lzcnt));
    mantissa = (mantissa << lzcnt) & 0x7Fu;
  }
  uint32_t f32 = ((exponent + 124u) << 23u) | (mantissa << 16u);
  return BitsToFloat(f32);
}

uint32_t PackR8G8B8A8Unorm(const DebugColor& color) {
  uint32_t r = PackUnorm(color.r, 255.0f);
  uint32_t g = PackUnorm(color.g, 255.0f);
  uint32_t b = PackUnorm(color.b, 255.0f);
  uint32_t a = PackUnorm(color.a, 255.0f);
  return r | (g << 8u) | (b << 16u) | (a << 24u);
}

bool PackColor32bpp(uint32_t format, const DebugColor& color,
                    uint32_t* packed_out) {
  switch (format) {
    case uint32_t(MetalEdramDumpFormat::kColorRGBA8): {
      *packed_out = PackR8G8B8A8Unorm(color);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRGB10A2Unorm): {
      uint32_t r = PackUnorm(color.r, 1023.0f);
      uint32_t g = PackUnorm(color.g, 1023.0f);
      uint32_t b = PackUnorm(color.b, 1023.0f);
      uint32_t a = PackUnorm(color.a, 3.0f);
      *packed_out = r | (g << 10u) | (b << 20u) | (a << 30u);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRGB10A2Float): {
      uint32_t r = XeUnclampedFloat32To7e3(color.r);
      uint32_t g = XeUnclampedFloat32To7e3(color.g);
      uint32_t b = XeUnclampedFloat32To7e3(color.b);
      uint32_t a = PackUnorm(color.a, 3.0f);
      *packed_out = (r & 0x3FFu) | ((g & 0x3FFu) << 10u) |
                    ((b & 0x3FFu) << 20u) | ((a & 0x3u) << 30u);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRG16Snorm): {
      uint32_t r = PackSnorm16(color.r);
      uint32_t g = PackSnorm16(color.g);
      *packed_out = r | (g << 16u);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRG16Float): {
      uint16_t r = FloatToHalf(color.r);
      uint16_t g = FloatToHalf(color.g);
      *packed_out = uint32_t(r) | (uint32_t(g) << 16u);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorR32Float): {
      *packed_out = FloatToBits(color.r);
      return true;
    }
    default:
      break;
  }
  return false;
}

bool UnpackColor32bpp(uint32_t format, uint32_t packed, DebugColor* color_out) {
  if (!color_out) {
    return false;
  }
  switch (format) {
    case uint32_t(MetalEdramDumpFormat::kColorRGBA8): {
      color_out->r = float(packed & 0xFFu) * (1.0f / 255.0f);
      color_out->g = float((packed >> 8u) & 0xFFu) * (1.0f / 255.0f);
      color_out->b = float((packed >> 16u) & 0xFFu) * (1.0f / 255.0f);
      color_out->a = float(packed >> 24u) * (1.0f / 255.0f);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRGB10A2Unorm): {
      color_out->r = float(packed & 0x3FFu) * (1.0f / 1023.0f);
      color_out->g = float((packed >> 10u) & 0x3FFu) * (1.0f / 1023.0f);
      color_out->b = float((packed >> 20u) & 0x3FFu) * (1.0f / 1023.0f);
      color_out->a = float((packed >> 30u) & 0x3u) * (1.0f / 3.0f);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRGB10A2Float): {
      color_out->r = XeFloat7e3To32(packed & 0x3FFu);
      color_out->g = XeFloat7e3To32((packed >> 10u) & 0x3FFu);
      color_out->b = XeFloat7e3To32((packed >> 20u) & 0x3FFu);
      color_out->a = float((packed >> 30u) & 0x3u) * (1.0f / 3.0f);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRG16Snorm): {
      int16_t r = int16_t(packed & 0xFFFFu);
      int16_t g = int16_t(packed >> 16u);
      color_out->r = std::max(float(r) * (1.0f / 32767.0f), -1.0f);
      color_out->g = std::max(float(g) * (1.0f / 32767.0f), -1.0f);
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRG16Float): {
      uint16_t r = uint16_t(packed & 0xFFFFu);
      uint16_t g = uint16_t(packed >> 16u);
      color_out->r = HalfToFloat(r);
      color_out->g = HalfToFloat(g);
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorR32Float): {
      color_out->r = BitsToFloat(packed);
      color_out->g = 0.0f;
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    default:
      break;
  }
  return false;
}

bool DecodeColorTexel(MTL::PixelFormat format, const uint8_t* bytes,
                      DebugColor* color_out) {
  if (!color_out) {
    return false;
  }
  switch (format) {
    case MTL::PixelFormatRGBA16Float: {
      uint16_t components[4];
      std::memcpy(components, bytes, sizeof(components));
      color_out->r = HalfToFloat(components[0]);
      color_out->g = HalfToFloat(components[1]);
      color_out->b = HalfToFloat(components[2]);
      color_out->a = HalfToFloat(components[3]);
      return true;
    }
    case MTL::PixelFormatRG16Float: {
      uint16_t components[2];
      std::memcpy(components, bytes, sizeof(components));
      color_out->r = HalfToFloat(components[0]);
      color_out->g = HalfToFloat(components[1]);
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    case MTL::PixelFormatRGBA8Unorm: {
      color_out->r = float(bytes[0]) * (1.0f / 255.0f);
      color_out->g = float(bytes[1]) * (1.0f / 255.0f);
      color_out->b = float(bytes[2]) * (1.0f / 255.0f);
      color_out->a = float(bytes[3]) * (1.0f / 255.0f);
      return true;
    }
    case MTL::PixelFormatBGRA8Unorm: {
      color_out->b = float(bytes[0]) * (1.0f / 255.0f);
      color_out->g = float(bytes[1]) * (1.0f / 255.0f);
      color_out->r = float(bytes[2]) * (1.0f / 255.0f);
      color_out->a = float(bytes[3]) * (1.0f / 255.0f);
      return true;
    }
    case MTL::PixelFormatRGB10A2Unorm:
    case MTL::PixelFormatBGR10A2Unorm: {
      uint32_t packed = 0;
      std::memcpy(&packed, bytes, sizeof(packed));
      DebugColor unpacked;
      unpacked.r = float(packed & 0x3FFu) * (1.0f / 1023.0f);
      unpacked.g = float((packed >> 10u) & 0x3FFu) * (1.0f / 1023.0f);
      unpacked.b = float((packed >> 20u) & 0x3FFu) * (1.0f / 1023.0f);
      unpacked.a = float((packed >> 30u) & 0x3u) * (1.0f / 3.0f);
      if (format == MTL::PixelFormatBGR10A2Unorm) {
        std::swap(unpacked.r, unpacked.b);
      }
      *color_out = unpacked;
      return true;
    }
    case MTL::PixelFormatR32Float: {
      uint32_t packed = 0;
      std::memcpy(&packed, bytes, sizeof(packed));
      color_out->r = BitsToFloat(packed);
      color_out->g = 0.0f;
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    case MTL::PixelFormatRG32Float: {
      uint32_t packed[2] = {};
      std::memcpy(packed, bytes, sizeof(packed));
      color_out->r = BitsToFloat(packed[0]);
      color_out->g = BitsToFloat(packed[1]);
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    default:
      break;
  }
  return false;
}

size_t MsaaSamplesToIndex(xenos::MsaaSamples samples) {
  switch (samples) {
    case xenos::MsaaSamples::k1X:
      return 0;
    case xenos::MsaaSamples::k2X:
      return 1;
    case xenos::MsaaSamples::k4X:
      return 2;
    default:
      return 0;
  }
}

uint32_t MsaaSamplesToCount(xenos::MsaaSamples samples) {
  switch (samples) {
    case xenos::MsaaSamples::k1X:
      return 1;
    case xenos::MsaaSamples::k2X:
      return 2;
    case xenos::MsaaSamples::k4X:
      return 4;
    default:
      return 1;
  }
}

struct TransferAddressConstants {
  uint32_t dest_pitch;
  uint32_t source_pitch;
  int32_t source_to_dest;
};

struct TransferShaderConstants {
  TransferAddressConstants address;
  TransferAddressConstants host_depth_address;
  uint32_t source_format;
  uint32_t dest_format;
  uint32_t source_is_depth;
  uint32_t dest_is_depth;
  uint32_t source_is_uint;
  uint32_t dest_is_uint;
  uint32_t source_is_64bpp;
  uint32_t dest_is_64bpp;
  uint32_t source_msaa_samples;
  uint32_t dest_msaa_samples;
  uint32_t host_depth_source_msaa_samples;
  uint32_t host_depth_source_is_copy;
  uint32_t depth_round;
  uint32_t msaa_2x_supported;
  uint32_t tile_width_samples;
  uint32_t tile_height_samples;
  uint32_t dest_tile_width_pixels;
  uint32_t dest_tile_height_pixels;
  float dest_tile_width_pixels_inv;
  float dest_tile_height_pixels_inv;
  float source_pitch_tiles_inv;
  float host_depth_source_pitch_tiles_inv;
  float dest_pixel_to_ndc_x;
  float dest_pixel_to_ndc_y;
  uint32_t dest_sample_id;
  uint32_t stencil_mask;
  uint32_t stencil_clear;
};

struct TransferTileInstance {
  float origin_x;
  float origin_y;
  uint32_t tile_index;
  uint32_t padding;
  uint32_t source_base_x;
  uint32_t source_base_y;
  uint32_t host_base_x;
  uint32_t host_base_y;
};

struct TransferRectInstance {
  float origin_x;
  float origin_y;
  float size_x;
  float size_y;
};

struct TransferClearColorFloatConstants {
  float color[4];
};

struct TransferClearColorUintConstants {
  uint32_t color[4];
};

struct TransferClearDepthConstants {
  float depth;
  float padding[3];
};

enum class TransferOutput {
  kColor,
  kDepth,
  kStencilBit,
};

struct TransferModeInfo {
  TransferOutput output;
  bool source_is_color;
  bool uses_host_depth;
};

constexpr TransferModeInfo kTransferModeInfos[] = {
    {TransferOutput::kColor, true, false},        // kColorToColor
    {TransferOutput::kDepth, true, false},        // kColorToDepth
    {TransferOutput::kColor, false, false},       // kDepthToColor
    {TransferOutput::kDepth, false, false},       // kDepthToDepth
    {TransferOutput::kStencilBit, true, false},   // kColorToStencilBit
    {TransferOutput::kStencilBit, false, false},  // kDepthToStencilBit
    {TransferOutput::kDepth, true, true},         // kColorAndHostDepthToDepth
    {TransferOutput::kDepth, false, true},        // kDepthAndHostDepthToDepth
};

}  // namespace

bool MetalRenderTargetCache::IsKey64bpp(RenderTargetKey key) const {
  // For host texture storage and transfers, gamma-as-unorm16 uses RGBA16Unorm
  // which is 64bpp. This is needed for correct transfer calculations.
  // NOTE: EDRAM dump path needs special handling - the EDRAM buffer is still
  // 32bpp even when host storage is 64bpp. See DumpRenderTargets.
  return key.Is64bpp() ||
         (!key.is_depth &&
          key.GetColorFormat() ==
              xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
          gamma_render_target_as_unorm16_);
}

uint32_t MetalRenderTargetCache::GetMetalEdramDumpFormat(RenderTargetKey key) {
  if (key.is_depth) {
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24FS8:
        return static_cast<uint32_t>(MetalEdramDumpFormat::kDepthD24FS8);
      case xenos::DepthRenderTargetFormat::kD24S8:
      default:
        return static_cast<uint32_t>(MetalEdramDumpFormat::kDepthD24S8);
    }
  }
  switch (key.GetColorFormat()) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGBA8);
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGB10A2Unorm);
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGB10A2Float);
    case xenos::ColorRenderTargetFormat::k_16_16:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRG16Snorm);
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRG16Float);
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorR32Float);
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGBA16Snorm);
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGBA16Float);
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRG32Float);
    default:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGBA8);
  }
}

// MetalRenderTarget implementation
MetalRenderTargetCache::MetalRenderTarget::~MetalRenderTarget() {
  if (stencil_view_) {
    stencil_view_->release();
    stencil_view_ = nullptr;
  }
  if (draw_texture_ && draw_texture_ != texture_) {
    draw_texture_->release();
    draw_texture_ = nullptr;
  }
  if (transfer_texture_ && transfer_texture_ != texture_) {
    transfer_texture_->release();
    transfer_texture_ = nullptr;
  }
  if (msaa_draw_texture_ && msaa_draw_texture_ != msaa_texture_) {
    msaa_draw_texture_->release();
    msaa_draw_texture_ = nullptr;
  }
  if (msaa_transfer_texture_ && msaa_transfer_texture_ != msaa_texture_) {
    msaa_transfer_texture_->release();
    msaa_transfer_texture_ = nullptr;
  }
  if (texture_) {
    texture_->release();
    texture_ = nullptr;
  }
  if (msaa_texture_) {
    msaa_texture_->release();
    msaa_texture_ = nullptr;
  }
}

// MetalRenderTargetCache implementation
MetalRenderTargetCache::MetalRenderTargetCache(
    const RegisterFile& register_file, const Memory& memory,
    TraceWriter* trace_writer, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y, MetalCommandProcessor& command_processor)
    : RenderTargetCache(register_file, memory, trace_writer,
                        draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {}

MetalRenderTargetCache::~MetalRenderTargetCache() { Shutdown(true); }

RenderTargetCache::Path MetalRenderTargetCache::GetPath() const {
  return Path::kHostRenderTargets;
}

bool MetalRenderTargetCache::Initialize() {
  device_ = command_processor_.GetMetalDevice();
  if (!device_) {
    XELOGE("MetalRenderTargetCache: No Metal device available");
    return false;
  }

  // 2x msaa and unorm16 support virtually guarunteed as minimum OS target /
  // Metal version currently is MacOS 15 / Metal 3
  msaa_2x_supported_ = device_->supportsTextureSampleCount(2);

  gamma_render_target_as_unorm16_ = ::cvars::gamma_render_target_as_unorm16 &&
                                    ::cvars::metal_allow_gamma_unorm16;
  if (::cvars::gamma_render_target_as_unorm16 &&
      !::cvars::metal_allow_gamma_unorm16) {
    XELOGW(
        "Metal: gamma_render_target_as_unorm16 disabled due to known issues; "
        "set --metal_allow_gamma_unorm16=true to force");
  }

  if (::cvars::metal_use_heaps) {
    size_t min_heap_bytes = std::max<int32_t>(0, ::cvars::metal_heap_min_bytes);
    render_target_heap_pool_ = std::make_unique<MetalHeapPool>(
        device_, MTL::StorageModePrivate, min_heap_bytes, "XeniaRT");
  }

  // Create the EDRAM buffer.
  //
  // The guest has 10 MiB of EDRAM for samples, but with host resolution
  // scaling enabled the compute path addresses a scaled EDRAM layout (the
  // shaders multiply the tile dimensions by resolution_scale_x/y). The buffer
  // therefore must be scaled by the same factor to avoid out-of-bounds writes.
  const uint32_t scale_x = std::max<uint32_t>(1u, draw_resolution_scale_x());
  const uint32_t scale_y = std::max<uint32_t>(1u, draw_resolution_scale_y());
  const size_t edram_dwords = size_t(xenos::kEdramTileCount) *
                              size_t(xenos::kEdramTileWidthSamples) *
                              size_t(xenos::kEdramTileHeightSamples) *
                              size_t(scale_x) * size_t(scale_y);
  const size_t edram_size_bytes = edram_dwords * sizeof(uint32_t);
  const bool edram_cpu_visible = false;
  const MTL::ResourceOptions edram_storage_mode =
      edram_cpu_visible ? MTL::ResourceStorageModeShared
                        : MTL::ResourceStorageModePrivate;
  edram_buffer_ = device_->newBuffer(edram_size_bytes, edram_storage_mode);
  if (!edram_buffer_) {
    XELOGE("MetalRenderTargetCache: Failed to create EDRAM buffer");
    return false;
  }
  edram_buffer_->setLabel(
      NS::String::string("EDRAM Buffer", NS::UTF8StringEncoding));
  if (edram_cpu_visible) {
    void* edram_contents = edram_buffer_->contents();
    if (edram_contents) {
      std::memset(edram_contents, 0, edram_size_bytes);
    }
  } else {
    ScopedAutoreleasePool autorelease_pool;
    MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
    if (queue) {
      MTL::CommandBuffer* cmd = queue->commandBuffer();
      if (cmd) {
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        if (blit) {
          blit->fillBuffer(
              edram_buffer_,
              NS::Range::Make(0, static_cast<NS::UInteger>(edram_size_bytes)),
              0);
          blit->endEncoding();
          cmd->commit();
        }
      }
    }
  }
  // Initialize EDRAM compute shaders
  if (!InitializeEdramComputeShaders()) {
    XELOGE(
        "MetalRenderTargetCache: Failed to initialize EDRAM compute shaders");
    return false;
  }

  // Initialize base class
  InitializeCommon();

  return true;
}

void MetalRenderTargetCache::Shutdown(bool from_destructor) {
  if (!from_destructor) {
    ClearCache();
  }

  // Clean up dummy target
  dummy_color_targets_.clear();
  dummy_color_target_ = nullptr;
  if (cached_render_pass_descriptor_) {
    cached_render_pass_descriptor_->release();
    cached_render_pass_descriptor_ = nullptr;
  }

  for (auto& it : transfer_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_pipelines_.clear();
  for (auto& it : transfer_tile_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_tile_pipelines_.clear();
  for (auto& it : edram_load_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  edram_load_pipelines_.clear();
  for (auto& it : transfer_clear_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_clear_pipelines_.clear();
  if (transfer_library_) {
    transfer_library_->release();
    transfer_library_ = nullptr;
  }
  if (edram_load_library_) {
    edram_load_library_->release();
    edram_load_library_ = nullptr;
  }
  if (edram_load_library_msaa_) {
    edram_load_library_msaa_->release();
    edram_load_library_msaa_ = nullptr;
  }
  if (transfer_depth_state_) {
    transfer_depth_state_->release();
    transfer_depth_state_ = nullptr;
  }
  if (transfer_depth_state_none_) {
    transfer_depth_state_none_->release();
    transfer_depth_state_none_ = nullptr;
  }
  if (transfer_depth_clear_state_) {
    transfer_depth_clear_state_->release();
    transfer_depth_clear_state_ = nullptr;
  }
  if (transfer_stencil_clear_state_) {
    transfer_stencil_clear_state_->release();
    transfer_stencil_clear_state_ = nullptr;
  }
  for (auto& state : transfer_stencil_bit_states_) {
    if (state) {
      state->release();
      state = nullptr;
    }
  }
  if (transfer_dummy_buffer_) {
    transfer_dummy_buffer_->release();
    transfer_dummy_buffer_ = nullptr;
  }
  for (auto& buffer : transfer_tile_instance_buffers_) {
    if (buffer) {
      buffer->release();
      buffer = nullptr;
    }
  }
  for (auto& retired_list : transfer_tile_instance_retired_buffers_) {
    for (auto* buffer : retired_list) {
      if (buffer) {
        buffer->release();
      }
    }
    retired_list.clear();
  }
  transfer_tile_instance_buffer_sizes_.fill(0);
  transfer_tile_instance_buffer_offset_ = 0;
  for (size_t i = 0; i < xe::countof(transfer_dummy_color_float_); ++i) {
    if (transfer_dummy_color_float_[i]) {
      transfer_dummy_color_float_[i]->release();
      transfer_dummy_color_float_[i] = nullptr;
    }
    if (transfer_dummy_color_uint_[i]) {
      transfer_dummy_color_uint_[i]->release();
      transfer_dummy_color_uint_[i] = nullptr;
    }
    if (transfer_dummy_depth_[i]) {
      transfer_dummy_depth_[i]->release();
      transfer_dummy_depth_[i] = nullptr;
    }
    if (transfer_dummy_stencil_[i]) {
      transfer_dummy_stencil_[i]->release();
      transfer_dummy_stencil_[i] = nullptr;
    }
  }

  // Clean up EDRAM compute shaders
  ShutdownEdramComputeShaders();

  if (edram_buffer_) {
    edram_buffer_->release();
    edram_buffer_ = nullptr;
  }

  // Destroy all render targets
  DestroyAllRenderTargets(!from_destructor);
  render_target_map_.clear();

  if (render_target_heap_pool_) {
    render_target_heap_pool_->Shutdown();
    render_target_heap_pool_.reset();
  }

  // Shutdown base class
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool MetalRenderTargetCache::InitializeEdramComputeShaders() {
  // Initialize the resolve / EDRAM compute pipelines used by the Metal backend.
  const bool draw_resolution_scaled = IsDrawResolutionScaled();
  edram_load_pipeline_ = nullptr;
  edram_store_pipeline_ = nullptr;
  edram_dump_color_32bpp_1xmsaa_pipeline_ = nullptr;
  edram_dump_color_32bpp_2xmsaa_pipeline_ = nullptr;
  edram_dump_color_32bpp_4xmsaa_pipeline_ = nullptr;
  edram_dump_color_64bpp_1xmsaa_pipeline_ = nullptr;
  edram_dump_color_64bpp_2xmsaa_pipeline_ = nullptr;
  edram_dump_color_64bpp_4xmsaa_pipeline_ = nullptr;
  edram_dump_depth_32bpp_1xmsaa_pipeline_ = nullptr;
  edram_dump_depth_32bpp_2xmsaa_pipeline_ = nullptr;
  edram_dump_depth_32bpp_4xmsaa_pipeline_ = nullptr;
  resolve_full_8bpp_pipeline_ = nullptr;
  resolve_full_16bpp_pipeline_ = nullptr;
  resolve_full_32bpp_pipeline_ = nullptr;
  resolve_full_64bpp_pipeline_ = nullptr;
  resolve_full_128bpp_pipeline_ = nullptr;
  resolve_fast_32bpp_1x2xmsaa_pipeline_ = nullptr;
  resolve_fast_32bpp_4xmsaa_pipeline_ = nullptr;
  resolve_fast_64bpp_1x2xmsaa_pipeline_ = nullptr;
  resolve_fast_64bpp_4xmsaa_pipeline_ = nullptr;
  resolve_full_8bpp_scaled_pipeline_ = nullptr;
  resolve_full_16bpp_scaled_pipeline_ = nullptr;
  resolve_full_32bpp_scaled_pipeline_ = nullptr;
  resolve_full_64bpp_scaled_pipeline_ = nullptr;
  resolve_full_128bpp_scaled_pipeline_ = nullptr;
  resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  resolve_fast_32bpp_4xmsaa_scaled_pipeline_ = nullptr;
  resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  resolve_fast_64bpp_4xmsaa_scaled_pipeline_ = nullptr;
  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    host_depth_store_pipelines_[i] = nullptr;
  }

  NS::Error* error = nullptr;

  // Resolve compute pipelines.
  resolve_full_8bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_8bpp_cs_metallib,
      sizeof(resolve_full_8bpp_cs_metallib), "resolve_full_8bpp");
  resolve_full_16bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_16bpp_cs_metallib,
      sizeof(resolve_full_16bpp_cs_metallib), "resolve_full_16bpp");
  resolve_full_32bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_32bpp_cs_metallib,
      sizeof(resolve_full_32bpp_cs_metallib), "resolve_full_32bpp");
  resolve_full_64bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_64bpp_cs_metallib,
      sizeof(resolve_full_64bpp_cs_metallib), "resolve_full_64bpp");
  resolve_full_128bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_128bpp_cs_metallib,
      sizeof(resolve_full_128bpp_cs_metallib), "resolve_full_128bpp");
  resolve_fast_32bpp_1x2xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_32bpp_1x2xmsaa_cs_metallib,
          sizeof(resolve_fast_32bpp_1x2xmsaa_cs_metallib),
          "resolve_fast_32bpp_1x2xmsaa");
  resolve_fast_32bpp_4xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_32bpp_4xmsaa_cs_metallib,
          sizeof(resolve_fast_32bpp_4xmsaa_cs_metallib),
          "resolve_fast_32bpp_4xmsaa");
  resolve_fast_64bpp_1x2xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_64bpp_1x2xmsaa_cs_metallib,
          sizeof(resolve_fast_64bpp_1x2xmsaa_cs_metallib),
          "resolve_fast_64bpp_1x2xmsaa");
  resolve_fast_64bpp_4xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_64bpp_4xmsaa_cs_metallib,
          sizeof(resolve_fast_64bpp_4xmsaa_cs_metallib),
          "resolve_fast_64bpp_4xmsaa");

  if (!resolve_full_8bpp_pipeline_ || !resolve_full_16bpp_pipeline_ ||
      !resolve_full_32bpp_pipeline_ || !resolve_full_64bpp_pipeline_ ||
      !resolve_full_128bpp_pipeline_ ||
      !resolve_fast_32bpp_1x2xmsaa_pipeline_ ||
      !resolve_fast_32bpp_4xmsaa_pipeline_ ||
      !resolve_fast_64bpp_1x2xmsaa_pipeline_ ||
      !resolve_fast_64bpp_4xmsaa_pipeline_) {
    XELOGE("Metal: failed to initialize resolve compute pipelines");
    return false;
  }

  if (draw_resolution_scaled) {
    resolve_full_8bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_8bpp_scaled_cs_metallib,
            sizeof(resolve_full_8bpp_scaled_cs_metallib),
            "resolve_full_8bpp_scaled");
    resolve_full_16bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_16bpp_scaled_cs_metallib,
            sizeof(resolve_full_16bpp_scaled_cs_metallib),
            "resolve_full_16bpp_scaled");
    resolve_full_32bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_32bpp_scaled_cs_metallib,
            sizeof(resolve_full_32bpp_scaled_cs_metallib),
            "resolve_full_32bpp_scaled");
    resolve_full_64bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_64bpp_scaled_cs_metallib,
            sizeof(resolve_full_64bpp_scaled_cs_metallib),
            "resolve_full_64bpp_scaled");
    resolve_full_128bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_128bpp_scaled_cs_metallib,
            sizeof(resolve_full_128bpp_scaled_cs_metallib),
            "resolve_full_128bpp_scaled");
    resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_32bpp_1x2xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_32bpp_1x2xmsaa_scaled_cs_metallib),
            "resolve_fast_32bpp_1x2xmsaa_scaled");
    resolve_fast_32bpp_4xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_32bpp_4xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_32bpp_4xmsaa_scaled_cs_metallib),
            "resolve_fast_32bpp_4xmsaa_scaled");
    resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_64bpp_1x2xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_64bpp_1x2xmsaa_scaled_cs_metallib),
            "resolve_fast_64bpp_1x2xmsaa_scaled");
    resolve_fast_64bpp_4xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_64bpp_4xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_64bpp_4xmsaa_scaled_cs_metallib),
            "resolve_fast_64bpp_4xmsaa_scaled");
    if (!resolve_full_8bpp_scaled_pipeline_ ||
        !resolve_full_16bpp_scaled_pipeline_ ||
        !resolve_full_32bpp_scaled_pipeline_ ||
        !resolve_full_64bpp_scaled_pipeline_ ||
        !resolve_full_128bpp_scaled_pipeline_ ||
        !resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ ||
        !resolve_fast_32bpp_4xmsaa_scaled_pipeline_ ||
        !resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ ||
        !resolve_fast_64bpp_4xmsaa_scaled_pipeline_) {
      XELOGE("Metal: failed to initialize scaled resolve compute pipelines");
      return false;
    }
  }

  host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)] =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, host_depth_store_1xmsaa_cs_metallib,
          sizeof(host_depth_store_1xmsaa_cs_metallib),
          "host_depth_store_1xmsaa");
  host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)] =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, host_depth_store_2xmsaa_cs_metallib,
          sizeof(host_depth_store_2xmsaa_cs_metallib),
          "host_depth_store_2xmsaa");
  host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)] =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, host_depth_store_4xmsaa_cs_metallib,
          sizeof(host_depth_store_4xmsaa_cs_metallib),
          "host_depth_store_4xmsaa");

  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    if (!host_depth_store_pipelines_[i]) {
      XELOGE("Metal: failed to initialize host depth store pipelines");
      return false;
    }
  }

  // EDRAM dump compute shader for 32-bpp color, 1x MSAA.
  {
    static const char kEdramDumpColor32bpp1xMsaaShader[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;  // bit 0
constant uint kDumpFlagDepthRound = 2;  // bit 1
constant uint kDumpFlagGammaAsLinear = 4;  // bit 2: source is linear, needs PWL gamma encode

// PWL gamma encode: linear -> gamma (for gamma RTs stored as linear RGBA16Unorm)
inline float XeLinearToPWLGamma(float value) {
  float clamped = clamp(value, 0.0f, 1.0f);
  float scale, offset;
  if (clamped >= (128.0f / 1023.0f)) {
    if (clamped >= (512.0f / 1023.0f)) { scale = 1023.0f / 8.0f; offset = 128.0f / 255.0f; }
    else { scale = 1023.0f / 4.0f; offset = 64.0f / 255.0f; }
  } else {
    if (clamped >= (64.0f / 1023.0f)) { scale = 1023.0f / 2.0f; offset = 32.0f / 255.0f; }
    else { scale = 1023.0f; offset = 0.0f; }
  }
  return trunc(clamped * scale) * (1.0f / 255.0f) + offset;
}
inline float3 XeLinearToPWLGamma3(float3 v) {
  return float3(XeLinearToPWLGamma(v.r), XeLinearToPWLGamma(v.g), XeLinearToPWLGamma(v.b));
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

inline uint XePackSnorm16(float value) {
  float clamped = clamp(value, -1.0f, 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint(packed) & 0xFFFFu;
}

uint XePreClampedFloat32To7e3(float value) {
  uint f32 = as_type<uint>(value);
  uint biased_f32;
  if (f32 < 0x3E800000u) {
    uint f32_exp = f32 >> 23u;
    uint shift = 125u - f32_exp;
    shift = min(shift, 24u);
    uint mantissa = (f32 & 0x7FFFFFu) | 0x800000u;
    biased_f32 = mantissa >> shift;
  } else {
    biased_f32 = f32 + 0xC2000000u;
  }
  uint round_bit = (biased_f32 >> 16u) & 1u;
  uint f10 = biased_f32 + 0x7FFFu + round_bit;
  return (f10 >> 16u) & 0x3FFu;
}

uint XeUnclampedFloat32To7e3(float value) {
  float clamped = min(max(value, 0.0f), 31.875f);
  return XePreClampedFloat32To7e3(clamped);
}

uint XePackColor32bpp(uint format, float4 color) {
  switch (format) {
    case kDumpFormatColorRGBA8: {
      uint r = XePackUnorm(color.r, 255.0f);
      uint g = XePackUnorm(color.g, 255.0f);
      uint b = XePackUnorm(color.b, 255.0f);
      uint a = XePackUnorm(color.a, 255.0f);
      return r | (g << 8u) | (b << 16u) | (a << 24u);
    }
    case kDumpFormatColorRGB10A2Unorm: {
      uint r = XePackUnorm(color.r, 1023.0f);
      uint g = XePackUnorm(color.g, 1023.0f);
      uint b = XePackUnorm(color.b, 1023.0f);
      uint a = XePackUnorm(color.a, 3.0f);
      return r | (g << 10u) | (b << 20u) | (a << 30u);
    }
    case kDumpFormatColorRGB10A2Float: {
      uint r = XeUnclampedFloat32To7e3(color.r);
      uint g = XeUnclampedFloat32To7e3(color.g);
      uint b = XeUnclampedFloat32To7e3(color.b);
      uint a = XePackUnorm(color.a, 3.0f);
      return (r & 0x3FFu) | ((g & 0x3FFu) << 10u) |
             ((b & 0x3FFu) << 20u) | ((a & 0x3u) << 30u);
    }
    case kDumpFormatColorRG16Snorm: {
      uint r = XePackSnorm16(color.r);
      uint g = XePackSnorm16(color.g);
      return r | (g << 16u);
    }
    case kDumpFormatColorRG16Float:
      return as_type<uint>(half2(color.rg));
    case kDumpFormatColorR32Float:
      return as_type<uint>(color.r);
    default: {
      uint r = XePackUnorm(color.r, 255.0f);
      uint g = XePackUnorm(color.g, 255.0f);
      uint b = XePackUnorm(color.b, 255.0f);
      uint a = XePackUnorm(color.a, 255.0f);
      return r | (g << 8u) | (b << 16u) | (a << 24u);
    }
  }
}

kernel void edram_dump_color_32bpp_1xmsaa(
    texture2d<float, access::read> source [[texture(0)]],
    device uint* edram [[buffer(0)]],
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
  uint sample_index = sample_in_tile.y * tile_size.x + sample_in_tile.x;
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);
  uint2 source_coord = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                             source_tile_y * tile_size.y + sample_in_tile.y);

  float4 color = source.read(source_coord);

  // If source is a linear RGBA16Unorm gamma RT, convert to PWL gamma encoding
  if (constants.flags & kDumpFlagGammaAsLinear) {
    color.rgb = XeLinearToPWLGamma3(color.rgb);
  }

  uint packed = XePackColor32bpp(constants.format, color);

  edram[edram_index] = packed;
}
)METAL";

    NS::String* source = NS::String::string(kEdramDumpColor32bpp1xMsaaShader,
                                            NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(source, nullptr, &error);
    if (!lib) {
      XELOGW(
          "Metal: failed to compile edram_dump_color_32bpp_1xmsaa shader: {}",
          error ? error->localizedDescription()->utf8String() : "unknown");
    } else {
      NS::String* fn_name = NS::String::string("edram_dump_color_32bpp_1xmsaa",
                                               NS::UTF8StringEncoding);
      MTL::Function* fn = lib->newFunction(fn_name);
      if (!fn) {
        XELOGW("Metal: edram_dump_color_32bpp_1xmsaa missing entrypoint");
        lib->release();
      } else {
        edram_dump_color_32bpp_1xmsaa_pipeline_ =
            device_->newComputePipelineState(fn, &error);
        fn->release();
        lib->release();
        if (!edram_dump_color_32bpp_1xmsaa_pipeline_) {
          XELOGW(
              "Metal: failed to create edram_dump_color_32bpp_1xmsaa pipeline: "
              "{}",
              error ? error->localizedDescription()->utf8String() : "unknown");
        }
      }
    }
  }

  // EDRAM dump compute shader for 32-bpp color, 2x MSAA.
  {
    static const char kEdramDumpColor32bpp2xMsaaShader[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;  // bit 0
constant uint kDumpFlagDepthRound = 2;  // bit 1
constant uint kDumpFlagGammaAsLinear = 4;  // bit 2: source is linear, needs PWL gamma encode

// PWL gamma encode: linear -> gamma (for gamma RTs stored as linear RGBA16Unorm)
inline float XeLinearToPWLGamma(float value) {
  float clamped = clamp(value, 0.0f, 1.0f);
  float scale, offset;
  if (clamped >= (128.0f / 1023.0f)) {
    if (clamped >= (512.0f / 1023.0f)) { scale = 1023.0f / 8.0f; offset = 128.0f / 255.0f; }
    else { scale = 1023.0f / 4.0f; offset = 64.0f / 255.0f; }
  } else {
    if (clamped >= (64.0f / 1023.0f)) { scale = 1023.0f / 2.0f; offset = 32.0f / 255.0f; }
    else { scale = 1023.0f; offset = 0.0f; }
  }
  return trunc(clamped * scale) * (1.0f / 255.0f) + offset;
}
inline float3 XeLinearToPWLGamma3(float3 v) {
  return float3(XeLinearToPWLGamma(v.r), XeLinearToPWLGamma(v.g), XeLinearToPWLGamma(v.b));
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

inline uint XePackSnorm16(float value) {
  float clamped = clamp(value, -1.0f, 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint(packed) & 0xFFFFu;
}

uint XePreClampedFloat32To7e3(float value) {
  uint f32 = as_type<uint>(value);
  uint biased_f32;
  if (f32 < 0x3E800000u) {
    uint f32_exp = f32 >> 23u;
    uint shift = 125u - f32_exp;
    shift = min(shift, 24u);
    uint mantissa = (f32 & 0x7FFFFFu) | 0x800000u;
    biased_f32 = mantissa >> shift;
  } else {
    biased_f32 = f32 + 0xC2000000u;
  }
  uint round_bit = (biased_f32 >> 16u) & 1u;
  uint f10 = biased_f32 + 0x7FFFu + round_bit;
  return (f10 >> 16u) & 0x3FFu;
}

uint XeUnclampedFloat32To7e3(float value) {
  float clamped = min(max(value, 0.0f), 31.875f);
  return XePreClampedFloat32To7e3(clamped);
}

uint XePackColor32bpp(uint format, float4 color) {
  switch (format) {
    case kDumpFormatColorRGBA8: {
      uint r = XePackUnorm(color.r, 255.0f);
      uint g = XePackUnorm(color.g, 255.0f);
      uint b = XePackUnorm(color.b, 255.0f);
      uint a = XePackUnorm(color.a, 255.0f);
      return r | (g << 8u) | (b << 16u) | (a << 24u);
    }
    case kDumpFormatColorRGB10A2Unorm: {
      uint r = XePackUnorm(color.r, 1023.0f);
      uint g = XePackUnorm(color.g, 1023.0f);
      uint b = XePackUnorm(color.b, 1023.0f);
      uint a = XePackUnorm(color.a, 3.0f);
      return r | (g << 10u) | (b << 20u) | (a << 30u);
    }
    case kDumpFormatColorRGB10A2Float: {
      uint r = XeUnclampedFloat32To7e3(color.r);
      uint g = XeUnclampedFloat32To7e3(color.g);
      uint b = XeUnclampedFloat32To7e3(color.b);
      uint a = XePackUnorm(color.a, 3.0f);
      return (r & 0x3FFu) | ((g & 0x3FFu) << 10u) |
             ((b & 0x3FFu) << 20u) | ((a & 0x3u) << 30u);
    }
    case kDumpFormatColorRG16Snorm: {
      uint r = XePackSnorm16(color.r);
      uint g = XePackSnorm16(color.g);
      return r | (g << 16u);
    }
    case kDumpFormatColorRG16Float:
      return as_type<uint>(half2(color.rg));
    case kDumpFormatColorR32Float:
      return as_type<uint>(color.r);
    default: {
      uint r = XePackUnorm(color.r, 255.0f);
      uint g = XePackUnorm(color.g, 255.0f);
      uint b = XePackUnorm(color.b, 255.0f);
      uint a = XePackUnorm(color.a, 255.0f);
      return r | (g << 8u) | (b << 16u) | (a << 24u);
    }
  }
}

kernel void edram_dump_color_32bpp_2xmsaa(
    texture2d_ms<float, access::read> source [[texture(0)]],
    device uint* edram [[buffer(0)]],
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
  uint sample_index = sample_in_tile.y * tile_size.x + sample_in_tile.x;
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);
  uint2 source_sample = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                              source_tile_y * tile_size.y + sample_in_tile.y);

  uint sample_id = source_sample.y & 1u;
  uint2 pixel_coord = uint2(source_sample.x, source_sample.y >> 1);

  float4 color = source.read(pixel_coord, sample_id);

  // If source is a linear RGBA16Unorm gamma RT, convert to PWL gamma encoding
  if (constants.flags & kDumpFlagGammaAsLinear) {
    color.rgb = XeLinearToPWLGamma3(color.rgb);
  }

  uint packed = XePackColor32bpp(constants.format, color);

  edram[edram_index] = packed;
}
)METAL";

    NS::String* source = NS::String::string(kEdramDumpColor32bpp2xMsaaShader,
                                            NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(source, nullptr, &error);
    if (!lib) {
      XELOGW(
          "Metal: failed to compile edram_dump_color_32bpp_2xmsaa shader: {}",
          error ? error->localizedDescription()->utf8String() : "unknown");
    } else {
      NS::String* fn_name = NS::String::string("edram_dump_color_32bpp_2xmsaa",
                                               NS::UTF8StringEncoding);
      MTL::Function* fn = lib->newFunction(fn_name);
      if (!fn) {
        XELOGW("Metal: edram_dump_color_32bpp_2xmsaa missing entrypoint");
        lib->release();
      } else {
        edram_dump_color_32bpp_2xmsaa_pipeline_ =
            device_->newComputePipelineState(fn, &error);
        fn->release();
        lib->release();
        if (!edram_dump_color_32bpp_2xmsaa_pipeline_) {
          XELOGW(
              "Metal: failed to create edram_dump_color_32bpp_2xmsaa pipeline: "
              "{}",
              error ? error->localizedDescription()->utf8String() : "unknown");
        }
      }
    }
  }

  // EDRAM dump compute shader for 32-bpp color, 4x MSAA.
  {
    static const char kEdramDumpColor32bpp4xMsaaShader[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;  // bit 0
constant uint kDumpFlagDepthRound = 2;  // bit 1
constant uint kDumpFlagGammaAsLinear = 4;  // bit 2: source is linear, needs PWL gamma encode

// PWL gamma encode: linear -> gamma (for gamma RTs stored as linear RGBA16Unorm)
inline float XeLinearToPWLGamma(float value) {
  float clamped = clamp(value, 0.0f, 1.0f);
  float scale, offset;
  if (clamped >= (128.0f / 1023.0f)) {
    if (clamped >= (512.0f / 1023.0f)) { scale = 1023.0f / 8.0f; offset = 128.0f / 255.0f; }
    else { scale = 1023.0f / 4.0f; offset = 64.0f / 255.0f; }
  } else {
    if (clamped >= (64.0f / 1023.0f)) { scale = 1023.0f / 2.0f; offset = 32.0f / 255.0f; }
    else { scale = 1023.0f; offset = 0.0f; }
  }
  return trunc(clamped * scale) * (1.0f / 255.0f) + offset;
}
inline float3 XeLinearToPWLGamma3(float3 v) {
  return float3(XeLinearToPWLGamma(v.r), XeLinearToPWLGamma(v.g), XeLinearToPWLGamma(v.b));
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

inline uint XePackSnorm16(float value) {
  float clamped = clamp(value, -1.0f, 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint(packed) & 0xFFFFu;
}

uint XePreClampedFloat32To7e3(float value) {
  uint f32 = as_type<uint>(value);
  uint biased_f32;
  if (f32 < 0x3E800000u) {
    uint f32_exp = f32 >> 23u;
    uint shift = 125u - f32_exp;
    shift = min(shift, 24u);
    uint mantissa = (f32 & 0x7FFFFFu) | 0x800000u;
    biased_f32 = mantissa >> shift;
  } else {
    biased_f32 = f32 + 0xC2000000u;
  }
  uint round_bit = (biased_f32 >> 16u) & 1u;
  uint f10 = biased_f32 + 0x7FFFu + round_bit;
  return (f10 >> 16u) & 0x3FFu;
}

uint XeUnclampedFloat32To7e3(float value) {
  float clamped = min(max(value, 0.0f), 31.875f);
  return XePreClampedFloat32To7e3(clamped);
}

uint XePackColor32bpp(uint format, float4 color) {
  switch (format) {
    case kDumpFormatColorRGBA8: {
      uint r = XePackUnorm(color.r, 255.0f);
      uint g = XePackUnorm(color.g, 255.0f);
      uint b = XePackUnorm(color.b, 255.0f);
      uint a = XePackUnorm(color.a, 255.0f);
      return r | (g << 8u) | (b << 16u) | (a << 24u);
    }
    case kDumpFormatColorRGB10A2Unorm: {
      uint r = XePackUnorm(color.r, 1023.0f);
      uint g = XePackUnorm(color.g, 1023.0f);
      uint b = XePackUnorm(color.b, 1023.0f);
      uint a = XePackUnorm(color.a, 3.0f);
      return r | (g << 10u) | (b << 20u) | (a << 30u);
    }
    case kDumpFormatColorRGB10A2Float: {
      uint r = XeUnclampedFloat32To7e3(color.r);
      uint g = XeUnclampedFloat32To7e3(color.g);
      uint b = XeUnclampedFloat32To7e3(color.b);
      uint a = XePackUnorm(color.a, 3.0f);
      return (r & 0x3FFu) | ((g & 0x3FFu) << 10u) |
             ((b & 0x3FFu) << 20u) | ((a & 0x3u) << 30u);
    }
    case kDumpFormatColorRG16Snorm: {
      uint r = XePackSnorm16(color.r);
      uint g = XePackSnorm16(color.g);
      return r | (g << 16u);
    }
    case kDumpFormatColorRG16Float:
      return as_type<uint>(half2(color.rg));
    case kDumpFormatColorR32Float:
      return as_type<uint>(color.r);
    default: {
      uint r = XePackUnorm(color.r, 255.0f);
      uint g = XePackUnorm(color.g, 255.0f);
      uint b = XePackUnorm(color.b, 255.0f);
      uint a = XePackUnorm(color.a, 255.0f);
      return r | (g << 8u) | (b << 16u) | (a << 24u);
    }
  }
}

kernel void edram_dump_color_32bpp_4xmsaa(
    texture2d_ms<float, access::read> source [[texture(0)]],
    device uint* edram [[buffer(0)]],
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
  uint sample_index = sample_in_tile.y * tile_size.x + sample_in_tile.x;
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);
  uint2 source_sample = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                              source_tile_y * tile_size.y + sample_in_tile.y);

  uint sample_x = source_sample.x & 1u;
  uint sample_y = source_sample.y & 1u;
  uint sample_id = sample_x | (sample_y << 1u);
  uint2 pixel_coord = uint2(source_sample.x >> 1, source_sample.y >> 1);

  float4 color = source.read(pixel_coord, sample_id);

  // If source is a linear RGBA16Unorm gamma RT, convert to PWL gamma encoding
  if (constants.flags & kDumpFlagGammaAsLinear) {
    color.rgb = XeLinearToPWLGamma3(color.rgb);
  }

  uint packed = XePackColor32bpp(constants.format, color);

  edram[edram_index] = packed;
}
)METAL";

    NS::String* source = NS::String::string(kEdramDumpColor32bpp4xMsaaShader,
                                            NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(source, nullptr, &error);
    if (!lib) {
      XELOGW(
          "Metal: failed to compile edram_dump_color_32bpp_4xmsaa shader: {}",
          error ? error->localizedDescription()->utf8String() : "unknown");
    } else {
      NS::String* fn_name = NS::String::string("edram_dump_color_32bpp_4xmsaa",
                                               NS::UTF8StringEncoding);
      MTL::Function* fn = lib->newFunction(fn_name);
      if (!fn) {
        XELOGW("Metal: edram_dump_color_32bpp_4xmsaa missing entrypoint");
        lib->release();
      } else {
        edram_dump_color_32bpp_4xmsaa_pipeline_ =
            device_->newComputePipelineState(fn, &error);
        fn->release();
        lib->release();
        if (!edram_dump_color_32bpp_4xmsaa_pipeline_) {
          XELOGW(
              "Metal: failed to create edram_dump_color_32bpp_4xmsaa pipeline: "
              "{}",
              error ? error->localizedDescription()->utf8String() : "unknown");
        }
      }
    }
  }

  // EDRAM dump compute shader for 32-bpp depth, 4x MSAA.
  {
    static const char kEdramDumpDepth32bpp4xMsaaShader[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;
constant uint kDumpFlagDepthRound = 2;

inline uint XeRoundToNearestEven(float value) {
  float floor_value = floor(value);
  float frac = value - floor_value;
  uint result = uint(floor_value);
  if (frac > 0.5f || (frac == 0.5f && (result & 1u))) {
    result += 1u;
  }
  return result;
}

uint XeFloat32To20e4(float value, bool round_to_nearest_even) {
  uint f32 = as_type<uint>(value);
  f32 = min((f32 <= 0x7FFFFFFFu) ? f32 : 0u, 0x3FFFFFF8u);
  uint denormalized =
      ((f32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32 >> 23u), 24u);
  uint f24 = (f32 < 0x38800000u) ? denormalized : (f32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24 += 3u + ((f24 >> 3u) & 1u);
  }
  return (f24 >> 3u) & 0xFFFFFFu;
}

kernel void edram_dump_depth_32bpp_4xmsaa(
    texture2d_ms<float, access::read> source [[texture(0)]],
    texture2d_ms<uint, access::read> stencil [[texture(1)]],
    device uint* edram [[buffer(0)]],
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);
  uint2 edram_sample_in_tile = sample_in_tile;
  uint tile_width_half = tile_size.x >> 1u;
  edram_sample_in_tile.x =
      (edram_sample_in_tile.x < tile_width_half)
          ? (edram_sample_in_tile.x + tile_width_half)
          : (edram_sample_in_tile.x - tile_width_half);

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
  uint sample_index =
      edram_sample_in_tile.y * tile_size.x + edram_sample_in_tile.x;
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);
  uint2 source_sample = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                              source_tile_y * tile_size.y + sample_in_tile.y);

  uint sample_x = source_sample.x & 1u;
  uint sample_y = source_sample.y & 1u;
  uint sample_id = sample_x | (sample_y << 1u);
  uint2 pixel_coord = uint2(source_sample.x >> 1, source_sample.y >> 1);

  float depth = source.read(pixel_coord, sample_id).r;

  uint depth24;
  if (constants.format == kDumpFormatDepthD24FS8) {
    bool round_depth = (constants.flags & kDumpFlagDepthRound) != 0u;
    depth24 = XeFloat32To20e4(depth * 2.0f, round_depth);
  } else {
    float depth_f = clamp(depth, 0.0f, 1.0f) * 16777215.0f;
    depth24 = XeRoundToNearestEven(depth_f);
  }

  uint stencil_value = 0u;
  if ((constants.flags & kDumpFlagHasStencil) != 0u) {
    stencil_value = stencil.read(pixel_coord, sample_id).x & 0xFFu;
  }

  uint packed = (depth24 << 8u) | stencil_value;

  edram[edram_index] = packed;
}
)METAL";

    NS::String* source = NS::String::string(kEdramDumpDepth32bpp4xMsaaShader,
                                            NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(source, nullptr, &error);
    if (!lib) {
      XELOGW(
          "Metal: failed to compile edram_dump_depth_32bpp_4xmsaa shader: {}",
          error ? error->localizedDescription()->utf8String() : "unknown");
    } else {
      NS::String* fn_name = NS::String::string("edram_dump_depth_32bpp_4xmsaa",
                                               NS::UTF8StringEncoding);
      MTL::Function* fn = lib->newFunction(fn_name);
      if (!fn) {
        XELOGW("Metal: edram_dump_depth_32bpp_4xmsaa missing entrypoint");
        lib->release();
      } else {
        edram_dump_depth_32bpp_4xmsaa_pipeline_ =
            device_->newComputePipelineState(fn, &error);
        fn->release();
        lib->release();
        if (!edram_dump_depth_32bpp_4xmsaa_pipeline_) {
          XELOGW(
              "Metal: failed to create edram_dump_depth_32bpp_4xmsaa pipeline: "
              "{}",
              error ? error->localizedDescription()->utf8String() : "unknown");
        }
      }
    }
  }

  // EDRAM dump compute shader for 32-bpp depth, 2x MSAA.
  {
    static const char kEdramDumpDepth32bpp2xMsaaShader[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;
constant uint kDumpFlagDepthRound = 2;

inline uint XeRoundToNearestEven(float value) {
  float floor_value = floor(value);
  float frac = value - floor_value;
  uint result = uint(floor_value);
  if (frac > 0.5f || (frac == 0.5f && (result & 1u))) {
    result += 1u;
  }
  return result;
}

uint XeFloat32To20e4(float value, bool round_to_nearest_even) {
  uint f32 = as_type<uint>(value);
  f32 = min((f32 <= 0x7FFFFFFFu) ? f32 : 0u, 0x3FFFFFF8u);
  uint denormalized =
      ((f32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32 >> 23u), 24u);
  uint f24 = (f32 < 0x38800000u) ? denormalized : (f32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24 += 3u + ((f24 >> 3u) & 1u);
  }
  return (f24 >> 3u) & 0xFFFFFFu;
}

kernel void edram_dump_depth_32bpp_2xmsaa(
    texture2d_ms<float, access::read> source [[texture(0)]],
    texture2d_ms<uint, access::read> stencil [[texture(1)]],
    device uint* edram [[buffer(0)]],
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);
  uint2 edram_sample_in_tile = sample_in_tile;
  uint tile_width_half = tile_size.x >> 1u;
  edram_sample_in_tile.x =
      (edram_sample_in_tile.x < tile_width_half)
          ? (edram_sample_in_tile.x + tile_width_half)
          : (edram_sample_in_tile.x - tile_width_half);

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
  uint sample_index =
      edram_sample_in_tile.y * tile_size.x + edram_sample_in_tile.x;
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);
  uint2 source_sample = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                              source_tile_y * tile_size.y + sample_in_tile.y);

  uint sample_id = source_sample.y & 1u;
  uint2 pixel_coord = uint2(source_sample.x, source_sample.y >> 1);

  float depth = source.read(pixel_coord, sample_id).r;

  uint depth24;
  if (constants.format == kDumpFormatDepthD24FS8) {
    bool round_depth = (constants.flags & kDumpFlagDepthRound) != 0u;
    depth24 = XeFloat32To20e4(depth * 2.0f, round_depth);
  } else {
    float depth_f = clamp(depth, 0.0f, 1.0f) * 16777215.0f;
    depth24 = XeRoundToNearestEven(depth_f);
  }

  uint stencil_value = 0u;
  if ((constants.flags & kDumpFlagHasStencil) != 0u) {
    stencil_value = stencil.read(pixel_coord, sample_id).x & 0xFFu;
  }

  uint packed = (depth24 << 8u) | stencil_value;

  edram[edram_index] = packed;
}
)METAL";

    NS::String* source = NS::String::string(kEdramDumpDepth32bpp2xMsaaShader,
                                            NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(source, nullptr, &error);
    if (!lib) {
      XELOGW(
          "Metal: failed to compile edram_dump_depth_32bpp_2xmsaa shader: {}",
          error ? error->localizedDescription()->utf8String() : "unknown");
    } else {
      NS::String* fn_name = NS::String::string("edram_dump_depth_32bpp_2xmsaa",
                                               NS::UTF8StringEncoding);
      MTL::Function* fn = lib->newFunction(fn_name);
      if (!fn) {
        XELOGW("Metal: edram_dump_depth_32bpp_2xmsaa missing entrypoint");
        lib->release();
      } else {
        edram_dump_depth_32bpp_2xmsaa_pipeline_ =
            device_->newComputePipelineState(fn, &error);
        fn->release();
        lib->release();
        if (!edram_dump_depth_32bpp_2xmsaa_pipeline_) {
          XELOGW(
              "Metal: failed to create edram_dump_depth_32bpp_2xmsaa pipeline: "
              "{}",
              error ? error->localizedDescription()->utf8String() : "unknown");
        }
      }
    }
  }

  // EDRAM dump compute shader for 32-bpp depth, 1x MSAA.
  {
    static const char kEdramDumpDepth32bpp1xMsaaShader[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;
constant uint kDumpFlagDepthRound = 2;

inline uint XeRoundToNearestEven(float value) {
  float floor_value = floor(value);
  float frac = value - floor_value;
  uint result = uint(floor_value);
  if (frac > 0.5f || (frac == 0.5f && (result & 1u))) {
    result += 1u;
  }
  return result;
}

uint XeFloat32To20e4(float value, bool round_to_nearest_even) {
  uint f32 = as_type<uint>(value);
  f32 = min((f32 <= 0x7FFFFFFFu) ? f32 : 0u, 0x3FFFFFF8u);
  uint denormalized =
      ((f32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32 >> 23u), 24u);
  uint f24 = (f32 < 0x38800000u) ? denormalized : (f32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24 += 3u + ((f24 >> 3u) & 1u);
  }
  return (f24 >> 3u) & 0xFFFFFFu;
}

kernel void edram_dump_depth_32bpp_1xmsaa(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<uint, access::read> stencil [[texture(1)]],
    device uint* edram [[buffer(0)]],
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);
  uint2 edram_sample_in_tile = sample_in_tile;
  uint tile_width_half = tile_size.x >> 1u;
  edram_sample_in_tile.x =
      (edram_sample_in_tile.x < tile_width_half)
          ? (edram_sample_in_tile.x + tile_width_half)
          : (edram_sample_in_tile.x - tile_width_half);

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
  uint sample_index =
      edram_sample_in_tile.y * tile_size.x + edram_sample_in_tile.x;
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);
  uint2 source_coord = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                             source_tile_y * tile_size.y + sample_in_tile.y);

  float depth = source.read(source_coord).r;

  uint depth24;
  if (constants.format == kDumpFormatDepthD24FS8) {
    bool round_depth = (constants.flags & kDumpFlagDepthRound) != 0u;
    depth24 = XeFloat32To20e4(depth * 2.0f, round_depth);
  } else {
    float depth_f = clamp(depth, 0.0f, 1.0f) * 16777215.0f;
    depth24 = XeRoundToNearestEven(depth_f);
  }

  uint stencil_value = 0u;
  if ((constants.flags & kDumpFlagHasStencil) != 0u) {
    stencil_value = stencil.read(source_coord).x & 0xFFu;
  }

  uint packed = (depth24 << 8u) | stencil_value;

  edram[edram_index] = packed;
}
)METAL";

    NS::String* source = NS::String::string(kEdramDumpDepth32bpp1xMsaaShader,
                                            NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(source, nullptr, &error);
    if (!lib) {
      XELOGW(
          "Metal: failed to compile edram_dump_depth_32bpp_1xmsaa shader: {}",
          error ? error->localizedDescription()->utf8String() : "unknown");
    } else {
      NS::String* fn_name = NS::String::string("edram_dump_depth_32bpp_1xmsaa",
                                               NS::UTF8StringEncoding);
      MTL::Function* fn = lib->newFunction(fn_name);
      if (!fn) {
        XELOGW("Metal: edram_dump_depth_32bpp_1xmsaa missing entrypoint");
        lib->release();
      } else {
        edram_dump_depth_32bpp_1xmsaa_pipeline_ =
            device_->newComputePipelineState(fn, &error);
        fn->release();
        lib->release();
        if (!edram_dump_depth_32bpp_1xmsaa_pipeline_) {
          XELOGW(
              "Metal: failed to create edram_dump_depth_32bpp_1xmsaa pipeline: "
              "{}",
              error ? error->localizedDescription()->utf8String() : "unknown");
        }
      }
    }
  }

  // EDRAM dump compute shader for 64-bpp color, 1x MSAA.
  // 64bpp tiles are half the horizontal width (40 samples per tile, not 80).
  {
    static const char kEdramDumpColor64bpp1xMsaaShader[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;
constant uint kDumpFlagDepthRound = 2;

inline uint XePackSnorm16(float value) {
  float clamped = clamp(value, -1.0f, 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint(packed) & 0xFFFFu;
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

uint2 XePackColor64bpp(uint format, float4 color) {
  switch (format) {
    case kDumpFormatColorRGBA16Snorm: {
      uint r = XePackSnorm16(color.r);
      uint g = XePackSnorm16(color.g);
      uint b = XePackSnorm16(color.b);
      uint a = XePackSnorm16(color.a);
      uint rg = r | (g << 16u);
      uint ba = b | (a << 16u);
      return uint2(rg, ba);
    }
    case kDumpFormatColorRGBA16Float: {
      uint rg = as_type<uint>(half2(color.rg));
      uint ba = as_type<uint>(half2(color.ba));
      return uint2(rg, ba);
    }
    case kDumpFormatColorRGBA16Unorm: {
      uint r = XePackUnorm(color.r, 65535.0f);
      uint g = XePackUnorm(color.g, 65535.0f);
      uint b = XePackUnorm(color.b, 65535.0f);
      uint a = XePackUnorm(color.a, 65535.0f);
      uint rg = r | (g << 16u);
      uint ba = b | (a << 16u);
      return uint2(rg, ba);
    }
    case kDumpFormatColorRG32Float: {
      uint r = as_type<uint>(color.r);
      uint g = as_type<uint>(color.g);
      return uint2(r, g);
    }
    default: {
      uint rg = as_type<uint>(half2(color.rg));
      uint ba = as_type<uint>(half2(color.ba));
      return uint2(rg, ba);
    }
  }
}

kernel void edram_dump_color_64bpp_1xmsaa(
    texture2d<float, access::read> source [[texture(0)]],
    device uint2* edram [[buffer(0)]],
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  // 64bpp: 40 samples wide per tile instead of 80.
  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
  uint sample_index = sample_in_tile.y * tile_size.x + sample_in_tile.x;
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);
  uint2 source_coord = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                             source_tile_y * tile_size.y + sample_in_tile.y);

  float4 color = source.read(source_coord);

  edram[edram_index] = XePackColor64bpp(constants.format, color);
}
)METAL";

    NS::String* source = NS::String::string(kEdramDumpColor64bpp1xMsaaShader,
                                            NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(source, nullptr, &error);
    if (!lib) {
      XELOGW(
          "Metal: failed to compile edram_dump_color_64bpp_1xmsaa shader: {}",
          error ? error->localizedDescription()->utf8String() : "unknown");
    } else {
      NS::String* fn_name = NS::String::string("edram_dump_color_64bpp_1xmsaa",
                                               NS::UTF8StringEncoding);
      MTL::Function* fn = lib->newFunction(fn_name);
      if (!fn) {
        XELOGW("Metal: edram_dump_color_64bpp_1xmsaa missing entrypoint");
        lib->release();
      } else {
        edram_dump_color_64bpp_1xmsaa_pipeline_ =
            device_->newComputePipelineState(fn, &error);
        fn->release();
        lib->release();
        if (!edram_dump_color_64bpp_1xmsaa_pipeline_) {
          XELOGW(
              "Metal: failed to create edram_dump_color_64bpp_1xmsaa pipeline: "
              "{}",
              error ? error->localizedDescription()->utf8String() : "unknown");
        }
      }
    }
  }

  // EDRAM dump compute shader for 64-bpp color, 2x MSAA.
  {
    static const char kEdramDumpColor64bpp2xMsaaShader[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;
constant uint kDumpFlagDepthRound = 2;

inline uint XePackSnorm16(float value) {
  float clamped = clamp(value, -1.0f, 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint(packed) & 0xFFFFu;
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

uint2 XePackColor64bpp(uint format, float4 color) {
  switch (format) {
    case kDumpFormatColorRGBA16Snorm: {
      uint r = XePackSnorm16(color.r);
      uint g = XePackSnorm16(color.g);
      uint b = XePackSnorm16(color.b);
      uint a = XePackSnorm16(color.a);
      uint rg = r | (g << 16u);
      uint ba = b | (a << 16u);
      return uint2(rg, ba);
    }
    case kDumpFormatColorRGBA16Float: {
      uint rg = as_type<uint>(half2(color.rg));
      uint ba = as_type<uint>(half2(color.ba));
      return uint2(rg, ba);
    }
    case kDumpFormatColorRGBA16Unorm: {
      uint r = XePackUnorm(color.r, 65535.0f);
      uint g = XePackUnorm(color.g, 65535.0f);
      uint b = XePackUnorm(color.b, 65535.0f);
      uint a = XePackUnorm(color.a, 65535.0f);
      uint rg = r | (g << 16u);
      uint ba = b | (a << 16u);
      return uint2(rg, ba);
    }
    case kDumpFormatColorRG32Float: {
      uint r = as_type<uint>(color.r);
      uint g = as_type<uint>(color.g);
      return uint2(r, g);
    }
    default: {
      uint rg = as_type<uint>(half2(color.rg));
      uint ba = as_type<uint>(half2(color.ba));
      return uint2(rg, ba);
    }
  }
}

kernel void edram_dump_color_64bpp_2xmsaa(
    texture2d_ms<float, access::read> source [[texture(0)]],
    device uint2* edram [[buffer(0)]],
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  // 64bpp: 40 samples wide per tile instead of 80.
  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
  uint sample_index = sample_in_tile.y * tile_size.x + sample_in_tile.x;
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);
  uint2 source_sample = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                              source_tile_y * tile_size.y + sample_in_tile.y);

  uint sample_id = source_sample.y & 1u;
  uint2 pixel_coord = uint2(source_sample.x, source_sample.y >> 1);

  float4 color = source.read(pixel_coord, sample_id);

  edram[edram_index] = XePackColor64bpp(constants.format, color);
}
)METAL";

    NS::String* source = NS::String::string(kEdramDumpColor64bpp2xMsaaShader,
                                            NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(source, nullptr, &error);
    if (!lib) {
      XELOGW(
          "Metal: failed to compile edram_dump_color_64bpp_2xmsaa shader: {}",
          error ? error->localizedDescription()->utf8String() : "unknown");
    } else {
      NS::String* fn_name = NS::String::string("edram_dump_color_64bpp_2xmsaa",
                                               NS::UTF8StringEncoding);
      MTL::Function* fn = lib->newFunction(fn_name);
      if (!fn) {
        XELOGW("Metal: edram_dump_color_64bpp_2xmsaa missing entrypoint");
        lib->release();
      } else {
        edram_dump_color_64bpp_2xmsaa_pipeline_ =
            device_->newComputePipelineState(fn, &error);
        fn->release();
        lib->release();
        if (!edram_dump_color_64bpp_2xmsaa_pipeline_) {
          XELOGW(
              "Metal: failed to create edram_dump_color_64bpp_2xmsaa pipeline: "
              "{}",
              error ? error->localizedDescription()->utf8String() : "unknown");
        }
      }
    }
  }

  // EDRAM dump compute shader for 64-bpp color, 4x MSAA.
  {
    static const char kEdramDumpColor64bpp4xMsaaShader[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;
constant uint kDumpFlagDepthRound = 2;

inline uint XePackSnorm16(float value) {
  float clamped = clamp(value, -1.0f, 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint(packed) & 0xFFFFu;
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

uint2 XePackColor64bpp(uint format, float4 color) {
  switch (format) {
    case kDumpFormatColorRGBA16Snorm: {
      uint r = XePackSnorm16(color.r);
      uint g = XePackSnorm16(color.g);
      uint b = XePackSnorm16(color.b);
      uint a = XePackSnorm16(color.a);
      uint rg = r | (g << 16u);
      uint ba = b | (a << 16u);
      return uint2(rg, ba);
    }
    case kDumpFormatColorRGBA16Float: {
      uint rg = as_type<uint>(half2(color.rg));
      uint ba = as_type<uint>(half2(color.ba));
      return uint2(rg, ba);
    }
    case kDumpFormatColorRGBA16Unorm: {
      uint r = XePackUnorm(color.r, 65535.0f);
      uint g = XePackUnorm(color.g, 65535.0f);
      uint b = XePackUnorm(color.b, 65535.0f);
      uint a = XePackUnorm(color.a, 65535.0f);
      uint rg = r | (g << 16u);
      uint ba = b | (a << 16u);
      return uint2(rg, ba);
    }
    case kDumpFormatColorRG32Float: {
      uint r = as_type<uint>(color.r);
      uint g = as_type<uint>(color.g);
      return uint2(r, g);
    }
    default: {
      uint rg = as_type<uint>(half2(color.rg));
      uint ba = as_type<uint>(half2(color.ba));
      return uint2(rg, ba);
    }
  }
}

kernel void edram_dump_color_64bpp_4xmsaa(
    texture2d_ms<float, access::read> source [[texture(0)]],
    device uint2* edram [[buffer(0)]],
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  // 64bpp: 40 samples wide per tile instead of 80.
  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
  uint sample_index = sample_in_tile.y * tile_size.x + sample_in_tile.x;
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);
  uint2 source_sample = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                              source_tile_y * tile_size.y + sample_in_tile.y);

  uint sample_x = source_sample.x & 1u;
  uint sample_y = source_sample.y & 1u;
  uint sample_id = sample_x | (sample_y << 1u);
  uint2 pixel_coord = uint2(source_sample.x >> 1, source_sample.y >> 1);

  float4 color = source.read(pixel_coord, sample_id);

  edram[edram_index] = XePackColor64bpp(constants.format, color);
}
)METAL";

    NS::String* source = NS::String::string(kEdramDumpColor64bpp4xMsaaShader,
                                            NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(source, nullptr, &error);
    if (!lib) {
      XELOGW(
          "Metal: failed to compile edram_dump_color_64bpp_4xmsaa shader: {}",
          error ? error->localizedDescription()->utf8String() : "unknown");
    } else {
      NS::String* fn_name = NS::String::string("edram_dump_color_64bpp_4xmsaa",
                                               NS::UTF8StringEncoding);
      MTL::Function* fn = lib->newFunction(fn_name);
      if (!fn) {
        XELOGW("Metal: edram_dump_color_64bpp_4xmsaa missing entrypoint");
        lib->release();
      } else {
        edram_dump_color_64bpp_4xmsaa_pipeline_ =
            device_->newComputePipelineState(fn, &error);
        fn->release();
        lib->release();
        if (!edram_dump_color_64bpp_4xmsaa_pipeline_) {
          XELOGW(
              "Metal: failed to create edram_dump_color_64bpp_4xmsaa pipeline: "
              "{}",
              error ? error->localizedDescription()->utf8String() : "unknown");
        }
      }
    }
  }

  return true;
}

void MetalRenderTargetCache::ShutdownEdramComputeShaders() {
  if (edram_load_pipeline_) {
    edram_load_pipeline_->release();
    edram_load_pipeline_ = nullptr;
  }
  if (edram_store_pipeline_) {
    edram_store_pipeline_->release();
    edram_store_pipeline_ = nullptr;
  }
  // Release 32bpp color dump pipelines
  if (edram_dump_color_32bpp_1xmsaa_pipeline_) {
    edram_dump_color_32bpp_1xmsaa_pipeline_->release();
    edram_dump_color_32bpp_1xmsaa_pipeline_ = nullptr;
  }
  if (edram_dump_color_32bpp_2xmsaa_pipeline_) {
    edram_dump_color_32bpp_2xmsaa_pipeline_->release();
    edram_dump_color_32bpp_2xmsaa_pipeline_ = nullptr;
  }
  if (edram_dump_color_32bpp_4xmsaa_pipeline_) {
    edram_dump_color_32bpp_4xmsaa_pipeline_->release();
    edram_dump_color_32bpp_4xmsaa_pipeline_ = nullptr;
  }
  // Release 64bpp color dump pipelines
  if (edram_dump_color_64bpp_1xmsaa_pipeline_) {
    edram_dump_color_64bpp_1xmsaa_pipeline_->release();
    edram_dump_color_64bpp_1xmsaa_pipeline_ = nullptr;
  }
  if (edram_dump_color_64bpp_2xmsaa_pipeline_) {
    edram_dump_color_64bpp_2xmsaa_pipeline_->release();
    edram_dump_color_64bpp_2xmsaa_pipeline_ = nullptr;
  }
  if (edram_dump_color_64bpp_4xmsaa_pipeline_) {
    edram_dump_color_64bpp_4xmsaa_pipeline_->release();
    edram_dump_color_64bpp_4xmsaa_pipeline_ = nullptr;
  }
  // Release 32bpp depth dump pipelines
  if (edram_dump_depth_32bpp_1xmsaa_pipeline_) {
    edram_dump_depth_32bpp_1xmsaa_pipeline_->release();
    edram_dump_depth_32bpp_1xmsaa_pipeline_ = nullptr;
  }
  if (edram_dump_depth_32bpp_2xmsaa_pipeline_) {
    edram_dump_depth_32bpp_2xmsaa_pipeline_->release();
    edram_dump_depth_32bpp_2xmsaa_pipeline_ = nullptr;
  }
  if (edram_dump_depth_32bpp_4xmsaa_pipeline_) {
    edram_dump_depth_32bpp_4xmsaa_pipeline_->release();
    edram_dump_depth_32bpp_4xmsaa_pipeline_ = nullptr;
  }
  // Release resolve pipelines
  if (resolve_full_8bpp_pipeline_) {
    resolve_full_8bpp_pipeline_->release();
    resolve_full_8bpp_pipeline_ = nullptr;
  }
  if (resolve_full_16bpp_pipeline_) {
    resolve_full_16bpp_pipeline_->release();
    resolve_full_16bpp_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_pipeline_) {
    resolve_full_32bpp_pipeline_->release();
    resolve_full_32bpp_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_pipeline_) {
    resolve_full_64bpp_pipeline_->release();
    resolve_full_64bpp_pipeline_ = nullptr;
  }
  if (resolve_full_128bpp_pipeline_) {
    resolve_full_128bpp_pipeline_->release();
    resolve_full_128bpp_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_1x2xmsaa_pipeline_) {
    resolve_fast_32bpp_1x2xmsaa_pipeline_->release();
    resolve_fast_32bpp_1x2xmsaa_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_4xmsaa_pipeline_) {
    resolve_fast_32bpp_4xmsaa_pipeline_->release();
    resolve_fast_32bpp_4xmsaa_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_1x2xmsaa_pipeline_) {
    resolve_fast_64bpp_1x2xmsaa_pipeline_->release();
    resolve_fast_64bpp_1x2xmsaa_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_4xmsaa_pipeline_) {
    resolve_fast_64bpp_4xmsaa_pipeline_->release();
    resolve_fast_64bpp_4xmsaa_pipeline_ = nullptr;
  }
  if (resolve_full_8bpp_scaled_pipeline_) {
    resolve_full_8bpp_scaled_pipeline_->release();
    resolve_full_8bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_16bpp_scaled_pipeline_) {
    resolve_full_16bpp_scaled_pipeline_->release();
    resolve_full_16bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_scaled_pipeline_) {
    resolve_full_32bpp_scaled_pipeline_->release();
    resolve_full_32bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_scaled_pipeline_) {
    resolve_full_64bpp_scaled_pipeline_->release();
    resolve_full_64bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_128bpp_scaled_pipeline_) {
    resolve_full_128bpp_scaled_pipeline_->release();
    resolve_full_128bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_) {
    resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_->release();
    resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_4xmsaa_scaled_pipeline_) {
    resolve_fast_32bpp_4xmsaa_scaled_pipeline_->release();
    resolve_fast_32bpp_4xmsaa_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_) {
    resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_->release();
    resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_4xmsaa_scaled_pipeline_) {
    resolve_fast_64bpp_4xmsaa_scaled_pipeline_->release();
    resolve_fast_64bpp_4xmsaa_scaled_pipeline_ = nullptr;
  }
  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    if (host_depth_store_pipelines_[i]) {
      host_depth_store_pipelines_[i]->release();
      host_depth_store_pipelines_[i] = nullptr;
    }
  }
}

void MetalRenderTargetCache::ClearCache() {
  // Clear current bindings
  for (uint32_t i = 0; i < 4; ++i) {
    current_color_targets_[i] = nullptr;
  }
  current_depth_target_ = nullptr;
  render_pass_descriptor_dirty_ = true;

  // Clear the tracking of which render targets have been cleared
  cleared_render_targets_this_frame_.clear();
  dummy_color_targets_.clear();
  dummy_color_target_ = nullptr;
  render_target_map_.clear();

  // Call base implementation
  RenderTargetCache::ClearCache();
}

void MetalRenderTargetCache::BeginFrame() {
  ++frame_id_;

  // Clear the tracking of which render targets have been cleared this frame
  cleared_render_targets_this_frame_.clear();

  // Call base implementation
  RenderTargetCache::BeginFrame();

  if (::cvars::metal_memory_log_rate > 0 &&
      (frame_id_ % uint64_t(::cvars::metal_memory_log_rate)) == 0) {
    XELOGI(
        "Metal mem: frame={} rt={} map={} dummy={} pipelines={} "
        "tile_pipelines={} inst_buf_sizes=[{}, {}, {}]",
        frame_id_, render_target_map_.size(), render_target_map_.size(),
        dummy_color_targets_.size(), transfer_pipelines_.size(),
        transfer_tile_pipelines_.size(),
        transfer_tile_instance_buffer_sizes_[0],
        transfer_tile_instance_buffer_sizes_[1],
        transfer_tile_instance_buffer_sizes_[2]);
  }
}

bool MetalRenderTargetCache::Update(
    bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, const Shader& vertex_shader) {
  // Use the base class logic to update the current render target setup.
  if (!RenderTargetCache::Update(is_rasterization_done,
                                 normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    XELOGE("MetalRenderTargetCache::Update - Base class Update failed");
    return false;
  }

  if (::cvars::metal_memory_log_rate > 0) {
    static uint64_t memory_log_counter = 0;
    if ((++memory_log_counter % uint64_t(::cvars::metal_memory_log_rate)) ==
        0) {
      XELOGI(
          "Metal mem: frame={} rt={} map={} dummy={} pipelines={} "
          "tile_pipelines={} inst_buf_sizes=[{}, {}, {}]",
          frame_id_, render_target_map_.size(), render_target_map_.size(),
          dummy_color_targets_.size(), transfer_pipelines_.size(),
          transfer_tile_pipelines_.size(),
          transfer_tile_instance_buffer_sizes_[0],
          transfer_tile_instance_buffer_sizes_[1],
          transfer_tile_instance_buffer_sizes_[2]);
    }
  }

  // After base class update, retrieve the actual render targets that were
  // selected This is the KEY to connecting base class management with
  // Metal-specific rendering
  RenderTarget* const* accumulated_targets =
      last_update_accumulated_render_targets();

  // Check if render targets actually changed
  bool targets_changed = false;

  // Check depth target
  MetalRenderTarget* new_depth_target =
      accumulated_targets[0]
          ? static_cast<MetalRenderTarget*>(accumulated_targets[0])
          : nullptr;
  if (new_depth_target != current_depth_target_) {
    targets_changed = true;
    current_depth_target_ = new_depth_target;
    if (current_depth_target_) {
      XELOGD(
          "MetalRenderTargetCache::Update - Depth target changed: key={:08X}",
          current_depth_target_->key().key);
    }
  }

  // Check color targets
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MetalRenderTarget* new_color_target =
        accumulated_targets[i + 1]
            ? static_cast<MetalRenderTarget*>(accumulated_targets[i + 1])
            : nullptr;
    if (new_color_target != current_color_targets_[i]) {
      targets_changed = true;
      current_color_targets_[i] = new_color_target;
      if (current_color_targets_[i]) {
        XELOGD(
            "MetalRenderTargetCache::Update - Color target {} changed: "
            "key={:08X}",
            i, current_color_targets_[i]->key().key);
      }
    }
  }

  // Perform ownership transfers - this is critical for correct rendering when
  // EDRAM regions are aliased between different RT configurations.
  // The base class Update() populates last_update_transfers() with the needed
  // transfers based on EDRAM tile overlaps.
  PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                   accumulated_targets, last_update_transfers(),
                                   nullptr, nullptr, nullptr);

  // Only mark render pass descriptor as dirty if targets actually changed
  if (targets_changed) {
    render_pass_descriptor_dirty_ = true;
  }

  return true;
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetWidth() const {
  // Metal maximum texture dimension
  return 16384;
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetHeight() const {
  // Metal maximum texture dimension
  return 16384;
}

bool MetalRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

RenderTargetCache::RenderTarget* MetalRenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  // Calculate dimensions
  uint32_t width = key.GetWidth();
  uint32_t height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples);

  // Apply resolution scaling
  width *= draw_resolution_scale_x();
  height *= draw_resolution_scale_y();

  // Create Metal render target
  auto* render_target = new MetalRenderTarget(key);

  // Create the texture based on format
  MTL::Texture* texture = nullptr;
  uint32_t samples = 1 << uint32_t(key.msaa_samples);

  if (key.is_depth) {
    texture = CreateDepthTexture(width, height, key.GetDepthFormat(), samples);
  } else {
    texture = CreateColorTexture(width, height, key.GetColorFormat(), samples);
  }

  if (!texture) {
    delete render_target;
    return nullptr;
  }

  render_target->SetTexture(texture);
  if (!key.is_depth) {
    MTL::PixelFormat resource_format =
        GetColorResourcePixelFormat(key.GetColorFormat());
    MTL::PixelFormat draw_format =
        GetColorDrawPixelFormat(key.GetColorFormat());
    MTL::PixelFormat transfer_format =
        GetColorOwnershipTransferPixelFormat(key.GetColorFormat(), nullptr);
    if (draw_format != resource_format) {
      MTL::Texture* draw_view = texture->newTextureView(draw_format);
      RecordRenderTargetViewCreated();
      render_target->SetDrawTexture(draw_view);
    }
    if (transfer_format != resource_format) {
      MTL::Texture* transfer_view = texture->newTextureView(transfer_format);
      RecordRenderTargetViewCreated();
      render_target->SetTransferTexture(transfer_view);
    }
    if (render_target->msaa_texture()) {
      if (draw_format != render_target->msaa_texture()->pixelFormat()) {
        MTL::Texture* msaa_draw_view =
            render_target->msaa_texture()->newTextureView(draw_format);
        RecordRenderTargetViewCreated();
        render_target->SetMsaaDrawTexture(msaa_draw_view);
      }
      if (transfer_format != render_target->msaa_texture()->pixelFormat()) {
        MTL::Texture* msaa_transfer_view =
            render_target->msaa_texture()->newTextureView(transfer_format);
        RecordRenderTargetViewCreated();
        render_target->SetMsaaTransferTexture(msaa_transfer_view);
      }
    }
  }

  // NOTE: Unlike the previous implementation, we do NOT load EDRAM data here.
  // This matches D3D12's approach where:
  // 1. CreateRenderTarget creates an empty texture
  // 2. Data transfer happens via ownership transfers in
  // PerformTransfersAndResolveClears
  // 3. The EDRAM buffer is only used as scratch space for resolves
  //
  // The ownership transfer system (called from Update()) handles copying data
  // between render target textures when EDRAM regions are aliased between
  // different RT configurations.

  // Store in our map for later retrieval
  render_target_map_[key.key] = render_target;

  return render_target;
}

bool MetalRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  // Metal uses different depth encoding than Xbox 360
  // D24S8 on Xbox 360 vs D32Float_S8 on Metal
  return format == xenos::DepthRenderTargetFormat::kD24S8 ||
         format == xenos::DepthRenderTargetFormat::kD24FS8;
}

void MetalRenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  if (!snapshot) {
    return;
  }

  if (IsDrawResolutionScaled()) {
    return;
  }

  RenderTarget* full_edram_rt =
      PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
          xenos::ColorRenderTargetFormat::k_32_FLOAT);
  if (!full_edram_rt) {
    return;
  }

  MetalRenderTarget* metal_rt = static_cast<MetalRenderTarget*>(full_edram_rt);
  MTL::Texture* texture = metal_rt->texture();
  if (!texture) {
    return;
  }

  constexpr uint32_t kPitchTilesAt32bpp = 16;
  constexpr uint32_t kWidth =
      kPitchTilesAt32bpp * xenos::kEdramTileWidthSamples;
  constexpr uint32_t kTileRows = xenos::kEdramTileCount / kPitchTilesAt32bpp;
  constexpr uint32_t kHeight = kTileRows * xenos::kEdramTileHeightSamples;

  size_t staging_size = size_t(kWidth) * size_t(kHeight) * sizeof(uint32_t);
  MTL::Buffer* staging =
      device_->newBuffer(staging_size, MTL::ResourceStorageModeShared);
  if (!staging) {
    return;
  }

  auto* dst_base = static_cast<uint8_t*>(staging->contents());
  const uint8_t* src = static_cast<const uint8_t*>(snapshot);
  uint32_t bytes_per_row = kWidth * sizeof(uint32_t);

  for (uint32_t y_tile = 0; y_tile < kTileRows; ++y_tile) {
    for (uint32_t x_tile = 0; x_tile < kPitchTilesAt32bpp; ++x_tile) {
      uint32_t tile_index = y_tile * kPitchTilesAt32bpp + x_tile;
      const uint8_t* tile_src =
          src + tile_index * xenos::kEdramTileWidthSamples *
                    xenos::kEdramTileHeightSamples * sizeof(uint32_t);

      for (uint32_t sample_row = 0; sample_row < xenos::kEdramTileHeightSamples;
           ++sample_row) {
        uint32_t dst_y = y_tile * xenos::kEdramTileHeightSamples + sample_row;
        uint32_t dst_x = x_tile * xenos::kEdramTileWidthSamples;

        uint8_t* dst_row =
            dst_base + dst_y * bytes_per_row + dst_x * sizeof(uint32_t);
        const uint8_t* src_row = tile_src + sample_row *
                                                xenos::kEdramTileWidthSamples *
                                                sizeof(uint32_t);

        std::memcpy(dst_row, src_row,
                    xenos::kEdramTileWidthSamples * sizeof(uint32_t));
      }
    }
  }

  ScopedAutoreleasePool autorelease_pool;
  MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
  if (!queue) {
    staging->release();
    return;
  }

  MTL::CommandBuffer* cmd = queue->commandBuffer();
  if (!cmd) {
    staging->release();
    return;
  }

  MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
  if (!blit) {
    // cmd is autoreleased from commandBuffer() - do not release
    staging->release();
    return;
  }

  blit->copyFromBuffer(staging, 0, bytes_per_row, 0,
                       MTL::Size::Make(kWidth, kHeight, 1), texture, 0, 0,
                       MTL::Origin::Make(0, 0, 0));
  blit->endEncoding();
  cmd->commit();
  cmd->waitUntilCompleted();
  // cmd is autoreleased from commandBuffer() - do not release
  staging->release();
  if (metal_rt->needs_initial_clear()) {
    metal_rt->SetNeedsInitialClear(false);
    render_pass_descriptor_dirty_ = true;
  }

  // Seed edram_buffer_ with the restored full-EDRAM render target contents
  // so subsequent DumpRenderTargets and resolve passes see the same initial
  // EDRAM state as D3D12/Vulkan.
  DumpRenderTargets(0, kPitchTilesAt32bpp, kTileRows, kPitchTilesAt32bpp);
}

MTL::Texture* MetalRenderTargetCache::CreateColorTexture(
    uint32_t width, uint32_t height, xenos::ColorRenderTargetFormat format,
    uint32_t samples, bool transient_render_target_only,
    bool allow_unpooled_fallback) {
  MTL::PixelFormat resource_format = GetColorResourcePixelFormat(format);
  MTL::PixelFormat draw_format = GetColorDrawPixelFormat(format);
  MTL::PixelFormat transfer_format =
      GetColorOwnershipTransferPixelFormat(format, nullptr);
  bool needs_pixel_format_view =
      draw_format != resource_format || transfer_format != resource_format;

  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);  // Default height if not specified
  desc->setPixelFormat(resource_format);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  MTL::TextureUsage usage = MTL::TextureUsageRenderTarget;
  if (!transient_render_target_only) {
    usage |= MTL::TextureUsageShaderRead;
  }
  if (needs_pixel_format_view) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);

  MTL::Texture* texture = nullptr;
  bool can_use_memoryless = false;
#if XE_PLATFORM_IOS
  can_use_memoryless = transient_render_target_only && !needs_pixel_format_view;
#endif
  if (can_use_memoryless) {
    // Dummy fallback color targets are transient (load/store don't care) and
    // never sampled - memoryless is optimal on iOS TBDR.
    desc->setStorageMode(MTL::StorageModeMemoryless);
    texture = device_->newTexture(desc);
  }
  if (!texture) {
    desc->setStorageMode(MTL::StorageModePrivate);
    if (render_target_heap_pool_ && !can_use_memoryless) {
      texture = render_target_heap_pool_->CreateTexture(desc);
    }
    if (!texture && (!render_target_heap_pool_ || allow_unpooled_fallback)) {
      texture = device_->newTexture(desc);
    }
  }
  desc->release();
  // Initial clear is handled on first bind via load actions; avoid
  // synchronous clears here to keep the host RT path fast.
  return texture;
}

MTL::Texture* MetalRenderTargetCache::CreateDepthTexture(
    uint32_t width, uint32_t height, xenos::DepthRenderTargetFormat format,
    uint32_t samples) {
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);  // Default height if not specified
  MTL::PixelFormat pixel_format = GetDepthPixelFormat(format);
  desc->setPixelFormat(pixel_format);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  MTL::TextureUsage usage =
      MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
  if (pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
      pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);
  desc->setStorageMode(MTL::StorageModePrivate);

  MTL::Texture* texture = nullptr;
  if (render_target_heap_pool_) {
    texture = render_target_heap_pool_->CreateTexture(desc);
  }
  if (!texture) {
    texture = device_->newTexture(desc);
  }
  desc->release();
  // Initial clear is handled on first bind via load actions; avoid
  // synchronous clears here to keep the host RT path fast.
  return texture;
}

MTL::PixelFormat MetalRenderTargetCache::GetColorResourcePixelFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return MTL::PixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      // Updated to suport unorm16 for gamma render targets.
      return gamma_render_target_as_unorm16_ ? MTL::PixelFormatRGBA16Unorm
                                             : MTL::PixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return MTL::PixelFormatRGB10A2Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      // Match D3D12 behavior: store as RGBA16F and pack to float10 on dump.
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return MTL::PixelFormatRG16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return MTL::PixelFormatRGBA16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    default:
      XELOGE("MetalRenderTargetCache: Unsupported color format {}",
             static_cast<uint32_t>(format));
      return MTL::PixelFormatRGBA8Unorm;
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetColorDrawPixelFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return MTL::PixelFormatRG16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return MTL::PixelFormatRGBA16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    default:
      return GetColorResourcePixelFormat(format);
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetColorOwnershipTransferPixelFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Uint;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Uint;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Uint;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Uint;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      // Ownership transfers must use a linear resource view to avoid
      // implicit sRGB conversion for gamma render targets.
      return GetColorResourcePixelFormat(format);
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetDepthPixelFormat(
    xenos::DepthRenderTargetFormat format) const {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
    case xenos::DepthRenderTargetFormat::kD24FS8:
      // Metal doesn't have D24S8, use D32Float_S8
      return MTL::PixelFormatDepth32Float_Stencil8;
    default:
      XELOGE("MetalRenderTargetCache: Unsupported depth format {}",
             static_cast<uint32_t>(format));
      return MTL::PixelFormatDepth32Float_Stencil8;
  }
}

MTL::Texture* MetalRenderTargetCache::GetStencilTextureView(
    MetalRenderTarget* render_target) {
  if (!render_target) {
    return nullptr;
  }
  if (render_target->stencil_view()) {
    return render_target->stencil_view();
  }
  RenderTargetKey key = render_target->key();
  if (!key.is_depth) {
    return nullptr;
  }
  MTL::Texture* depth_texture = render_target->texture();
  if (!depth_texture) {
    return nullptr;
  }
  MTL::Texture* view =
      depth_texture->newTextureView(MTL::PixelFormatX32_Stencil8);
  if (view) {
    render_target->SetStencilView(view);
  }
  return view;
}

MTL::RenderPassDescriptor* MetalRenderTargetCache::GetRenderPassDescriptor(
    uint32_t expected_sample_count) {
  if (!render_pass_descriptor_dirty_ && cached_render_pass_descriptor_ &&
      cached_render_pass_descriptor_sample_count_ == expected_sample_count) {
    return cached_render_pass_descriptor_;
  }
  if (cached_render_pass_descriptor_sample_count_ != expected_sample_count) {
    render_pass_descriptor_dirty_ = true;
  }

  // Release old descriptor
  if (cached_render_pass_descriptor_) {
    cached_render_pass_descriptor_->release();
    cached_render_pass_descriptor_ = nullptr;
  }

  // Create new descriptor
  cached_render_pass_descriptor_ =
      MTL::RenderPassDescriptor::renderPassDescriptor();
  if (!cached_render_pass_descriptor_) {
    XELOGE("MetalRenderTargetCache: Failed to create render pass descriptor");
    return nullptr;
  }
  cached_render_pass_descriptor_->retain();
  cached_render_pass_descriptor_sample_count_ = expected_sample_count;

  bool has_any_render_target = false;
  bool has_any_color_target = false;
  bool needs_descriptor_refresh = false;
  uint32_t coverage_width = 0;
  uint32_t coverage_height = 0;
  uint32_t coverage_samples = std::max(1u, expected_sample_count);

  // Bind the actual render targets retrieved from base class in Update()

  // Bind depth target if present
  if (current_depth_target_ && current_depth_target_->texture()) {
    auto* depth_attachment = cached_render_pass_descriptor_->depthAttachment();
    depth_attachment->setTexture(current_depth_target_->draw_texture());

    // Clear on first bind to avoid synchronous clears at creation.
    uint32_t depth_key = current_depth_target_->key().key;
    bool depth_needs_clear = current_depth_target_->needs_initial_clear();
    if (depth_needs_clear) {
      depth_attachment->setLoadAction(MTL::LoadActionClear);
      depth_attachment->setClearDepth(1.0);
      current_depth_target_->SetNeedsInitialClear(false);
      needs_descriptor_refresh = true;
    } else {
      depth_attachment->setLoadAction(MTL::LoadActionLoad);
    }
    depth_attachment->setStoreAction(MTL::StoreActionStore);

    // If the depth texture includes stencil, bind the same texture to the
    // stencil attachment too (Metal requires explicit stencil attachment
    // binding to match pipeline state).
    MTL::PixelFormat depth_pixel_format =
        current_depth_target_->draw_texture()->pixelFormat();
    if (depth_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatX32_Stencil8) {
      auto* stencil_attachment =
          cached_render_pass_descriptor_->stencilAttachment();
      stencil_attachment->setTexture(current_depth_target_->draw_texture());
      if (depth_needs_clear) {
        stencil_attachment->setLoadAction(MTL::LoadActionClear);
        stencil_attachment->setClearStencil(0);
      } else {
        stencil_attachment->setLoadAction(MTL::LoadActionLoad);
      }
      stencil_attachment->setStoreAction(MTL::StoreActionStore);
    }

    has_any_render_target = true;

    // Track this as a real render target for capture
    last_real_depth_target_ = current_depth_target_;

    if (!coverage_width && current_depth_target_->draw_texture()) {
      coverage_width =
          static_cast<uint32_t>(current_depth_target_->draw_texture()->width());
      coverage_height = static_cast<uint32_t>(
          current_depth_target_->draw_texture()->height());
      if (current_depth_target_->draw_texture()->sampleCount() > 0) {
        coverage_samples = std::max<uint32_t>(
            coverage_samples,
            static_cast<uint32_t>(
                current_depth_target_->draw_texture()->sampleCount()));
      }
    }
  }

  // Bind color targets
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_color_targets_[i] && current_color_targets_[i]->texture()) {
      auto* color_attachment =
          cached_render_pass_descriptor_->colorAttachments()->object(i);
      color_attachment->setTexture(current_color_targets_[i]->draw_texture());

      // Clear on first bind to avoid synchronous clears at creation.
      bool color_needs_clear = current_color_targets_[i]->needs_initial_clear();
      if (color_needs_clear) {
        color_attachment->setLoadAction(MTL::LoadActionClear);
        color_attachment->setClearColor(
            MTL::ClearColor::Make(0.0, 0.0, 0.0, 0.0));
        current_color_targets_[i]->SetNeedsInitialClear(false);
        needs_descriptor_refresh = true;
      } else {
        color_attachment->setLoadAction(MTL::LoadActionLoad);
      }
      color_attachment->setStoreAction(MTL::StoreActionStore);

      has_any_render_target = true;
      has_any_color_target = true;

      // Track this as a real render target for capture
      last_real_color_targets_[i] = current_color_targets_[i];

      if (!coverage_width) {
        coverage_width = static_cast<uint32_t>(
            current_color_targets_[i]->draw_texture()->width());
        coverage_height = static_cast<uint32_t>(
            current_color_targets_[i]->draw_texture()->height());
        if (current_color_targets_[i]->draw_texture()->sampleCount() > 0) {
          coverage_samples = std::max<uint32_t>(
              coverage_samples,
              static_cast<uint32_t>(
                  current_color_targets_[i]->draw_texture()->sampleCount()));
        }
      }
    }
  }

  // If no color render targets are bound, attach a dummy color target so Metal
  // has at least one color attachment. This mirrors the D3D12/Vulkan behavior
  // where an RTV is always bound when drawing, and also keeps pipeline state
  // validation happy for depth-only passes.
  if (!has_any_color_target) {
    xenos::ColorRenderTargetFormat fmt =
        xenos::ColorRenderTargetFormat::k_8_8_8_8;
    uint32_t samples = std::max(1u, expected_sample_count);

    uint32_t width = 1280;
    uint32_t height = 720;
    if (current_depth_target_ && current_depth_target_->texture()) {
      width = static_cast<uint32_t>(current_depth_target_->texture()->width());
      height =
          static_cast<uint32_t>(current_depth_target_->texture()->height());
      if (current_depth_target_->texture()->sampleCount() > 0) {
        samples = std::max<uint32_t>(
            samples, static_cast<uint32_t>(
                         current_depth_target_->texture()->sampleCount()));
      }
    } else if (last_real_color_targets_[0] &&
               last_real_color_targets_[0]->texture()) {
      width = static_cast<uint32_t>(
          last_real_color_targets_[0]->texture()->width());
      height = static_cast<uint32_t>(
          last_real_color_targets_[0]->texture()->height());
    } else if (last_real_depth_target_ && last_real_depth_target_->texture()) {
      width =
          static_cast<uint32_t>(last_real_depth_target_->texture()->width());
      height =
          static_cast<uint32_t>(last_real_depth_target_->texture()->height());
      if (last_real_depth_target_->texture()->sampleCount() > 0) {
        samples = std::max<uint32_t>(
            samples, static_cast<uint32_t>(
                         last_real_depth_target_->texture()->sampleCount()));
      }
    }

    uint32_t dummy_sample_count =
        samples >= 4u ? 4u : (samples == 2u ? 2u : 1u);
    // Cache dummy color targets by shape/format only so depth-only passes with
    // changing EDRAM bases can reuse the same transient attachment.
    uint64_t dummy_key = uint64_t(width & 0xFFFFu) |
                         (uint64_t(height & 0xFFFFu) << 16) |
                         (uint64_t(dummy_sample_count & 0xFFu) << 32) |
                         (uint64_t(uint32_t(fmt) & 0xFFFFu) << 40);
    auto evict_oldest_dummy_target = [&](uint64_t keep_key) -> bool {
      uint64_t oldest_key = 0;
      uint64_t oldest_frame = frame_id_;
      bool found = false;
      for (const auto& it : dummy_color_targets_) {
        if (it.first == keep_key) {
          continue;
        }
        if (!found || it.second.last_used_frame < oldest_frame) {
          oldest_frame = it.second.last_used_frame;
          oldest_key = it.first;
          found = true;
        }
      }
      if (found) {
        dummy_color_targets_.erase(oldest_key);
      }
      return found;
    };

    auto& entry = dummy_color_targets_[dummy_key];
    if (!entry.target || !entry.target->texture()) {
      RenderTargetKey dummy_rt_key;
      dummy_rt_key.key = 0;
      dummy_rt_key.is_depth = 0;
      dummy_rt_key.resource_format = uint32_t(fmt);
      dummy_rt_key.msaa_samples =
          dummy_sample_count >= 4u   ? xenos::MsaaSamples::k4X
          : dummy_sample_count == 2u ? xenos::MsaaSamples::k2X
                                     : xenos::MsaaSamples::k1X;
      entry.target = std::make_unique<MetalRenderTarget>(dummy_rt_key);
      entry.last_cleared_frame = frame_id_ - 1;
      // Prefer memoryless transient attachments on iOS (inside
      // CreateColorTexture), otherwise keep dummy allocations in heap budget.
      MTL::Texture* tex =
          CreateColorTexture(width, height, fmt, dummy_sample_count,
                             /*transient_render_target_only=*/true,
                             /*allow_unpooled_fallback=*/false);
      while (!tex && render_target_heap_pool_ &&
             dummy_color_targets_.size() > 1 &&
             evict_oldest_dummy_target(dummy_key)) {
        tex = CreateColorTexture(width, height, fmt, dummy_sample_count,
                                 /*transient_render_target_only=*/true,
                                 /*allow_unpooled_fallback=*/false);
      }
      if (!tex) {
        static uint64_t last_unpooled_fallback_log_frame = 0;
        if (render_target_heap_pool_ &&
            last_unpooled_fallback_log_frame != frame_id_) {
          XELOGW(
              "Metal RT dummy target: heap allocation failed for {}x{} {}x; "
              "falling back to unpooled texture",
              width, height, dummy_sample_count);
          last_unpooled_fallback_log_frame = frame_id_;
        }
        tex = CreateColorTexture(width, height, fmt, dummy_sample_count,
                                 /*transient_render_target_only=*/true,
                                 /*allow_unpooled_fallback=*/true);
      }
      entry.target->SetTexture(tex);
      if (tex) {
        MTL::PixelFormat resource_format = GetColorResourcePixelFormat(fmt);
        MTL::PixelFormat draw_format = GetColorDrawPixelFormat(fmt);
        MTL::PixelFormat transfer_format =
            GetColorOwnershipTransferPixelFormat(fmt, nullptr);
        if (draw_format != resource_format) {
          entry.target->SetDrawTexture(tex->newTextureView(draw_format));
          RecordRenderTargetViewCreated();
        }
        if (transfer_format != resource_format) {
          entry.target->SetTransferTexture(
              tex->newTextureView(transfer_format));
          RecordRenderTargetViewCreated();
        }
      }
    }

    entry.last_used_frame = frame_id_;
    dummy_color_target_ = entry.target.get();

    // Keep this cache small - dummy targets are transient fallback attachments.
#if XE_PLATFORM_IOS
    constexpr size_t kMaxDummyColorTargets = 4;
#else
    constexpr size_t kMaxDummyColorTargets = 8;
#endif
    while (dummy_color_targets_.size() > kMaxDummyColorTargets) {
      if (!evict_oldest_dummy_target(dummy_key)) {
        break;
      }
    }

    auto* color_attachment =
        cached_render_pass_descriptor_->colorAttachments()->object(0);
    color_attachment->setTexture(dummy_color_target_->draw_texture());
    color_attachment->setLoadAction(MTL::LoadActionDontCare);
    color_attachment->setStoreAction(MTL::StoreActionDontCare);

    has_any_render_target = true;
    if (!coverage_width && dummy_color_target_->draw_texture()) {
      coverage_width =
          static_cast<uint32_t>(dummy_color_target_->draw_texture()->width());
      coverage_height =
          static_cast<uint32_t>(dummy_color_target_->draw_texture()->height());
      if (dummy_color_target_->draw_texture()->sampleCount() > 0) {
        coverage_samples = std::max<uint32_t>(
            coverage_samples,
            static_cast<uint32_t>(
                dummy_color_target_->draw_texture()->sampleCount()));
      }
    }
  }

  render_pass_descriptor_dirty_ = needs_descriptor_refresh;
  return cached_render_pass_descriptor_;
}

MTL::Texture* MetalRenderTargetCache::GetColorTarget(uint32_t index) const {
  if (index >= 4 || !current_color_targets_[index]) {
    return nullptr;
  }
  return current_color_targets_[index]->texture();
}

MTL::Texture* MetalRenderTargetCache::GetDepthTarget() const {
  if (!current_depth_target_) {
    return nullptr;
  }
  return current_depth_target_->texture();
}

MTL::Texture* MetalRenderTargetCache::GetDummyColorTarget() const {
  if (dummy_color_target_ && dummy_color_target_->texture()) {
    return dummy_color_target_->texture();
  }
  return nullptr;
}

void MetalRenderTargetCache::RecordRenderTargetViewCreated() {
  render_target_views_created_.fetch_add(1, std::memory_order_relaxed);
}

MetalRenderTargetCache::MetalRenderTarget*
MetalRenderTargetCache::GetColorRenderTarget(uint32_t index) const {
  if (index >= 4) {
    return nullptr;
  }
  return current_color_targets_[index];
}

MTL::Texture* MetalRenderTargetCache::GetColorTargetForDraw(
    uint32_t index) const {
  if (index >= 4 || !current_color_targets_[index]) {
    return nullptr;
  }
  return current_color_targets_[index]->draw_texture();
}

MTL::Texture* MetalRenderTargetCache::GetDepthTargetForDraw() const {
  if (!current_depth_target_) {
    return nullptr;
  }
  return current_depth_target_->draw_texture();
}

MTL::Texture* MetalRenderTargetCache::GetDummyColorTargetForDraw() const {
  if (dummy_color_target_ && dummy_color_target_->draw_texture()) {
    return dummy_color_target_->draw_texture();
  }
  return nullptr;
}

MTL::Texture* MetalRenderTargetCache::GetLastRealColorTarget(
    uint32_t index) const {
  if (index >= 4 || !last_real_color_targets_[index]) {
    return nullptr;
  }
  return last_real_color_targets_[index]->texture();
}

MTL::Texture* MetalRenderTargetCache::GetLastRealDepthTarget() const {
  if (!last_real_depth_target_) {
    return nullptr;
  }
  return last_real_depth_target_->texture();
}

MTL::Texture* MetalRenderTargetCache::GetRenderTargetTexture(
    RenderTargetKey key) const {
  auto it = render_target_map_.find(key.key);
  if (it == render_target_map_.end()) {
    return nullptr;
  }
  MetalRenderTarget* target = it->second;
  return target ? target->texture() : nullptr;
}

MTL::Texture* MetalRenderTargetCache::GetColorRenderTargetTexture(
    uint32_t pitch, xenos::MsaaSamples samples, uint32_t base,
    xenos::ColorRenderTargetFormat format) const {
  if (!pitch) {
    return nullptr;
  }
  RenderTargetKey key;
  key.base_tiles = base;
  uint32_t msaa_samples_x_log2 = uint32_t(samples >= xenos::MsaaSamples::k4X);
  key.pitch_tiles_at_32bpp =
      ((pitch << msaa_samples_x_log2) + (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  key.msaa_samples = samples;
  key.is_depth = 0;
  xenos::ColorRenderTargetFormat resource_format =
      (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
       !gamma_render_target_as_unorm16_)
          ? xenos::ColorRenderTargetFormat::k_8_8_8_8
          : xenos::GetStorageColorFormat(format);
  key.resource_format = uint32_t(resource_format);
  return GetRenderTargetTexture(key);
}

void MetalRenderTargetCache::StoreTiledData(MTL::CommandBuffer* command_buffer,
                                            MTL::Texture* texture,
                                            uint32_t edram_base,
                                            uint32_t pitch_tiles,
                                            uint32_t height_tiles,
                                            bool is_depth) {
  MTL::Texture* source_texture = texture;
  MTL::Texture* temp_texture = nullptr;

  // Check if this is a depth/stencil texture
  bool is_depth_stencil_format =
      texture->pixelFormat() == MTL::PixelFormatDepth32Float_Stencil8 ||
      texture->pixelFormat() == MTL::PixelFormatDepth32Float ||
      texture->pixelFormat() == MTL::PixelFormatDepth16Unorm ||
      texture->pixelFormat() == MTL::PixelFormatDepth24Unorm_Stencil8 ||
      texture->pixelFormat() == MTL::PixelFormatX32_Stencil8;

  if (is_depth_stencil_format) {
    // Depth/stencil textures can't be sampled directly from a compute shader,
    // so storing them back to EDRAM would need a depth-read intermediate path.
    // Depth buffers are typically write-only during rendering, so skip.
    return;
  }

  // If texture is multisample, create a temporary non-multisample texture and
  // resolve to it first
  if (texture->textureType() == MTL::TextureType2DMultisample) {
    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(texture->width());
    desc->setHeight(texture->height());
    desc->setPixelFormat(texture->pixelFormat());
    desc->setTextureType(MTL::TextureType2D);  // Regular 2D texture
    desc->setSampleCount(1);                   // Non-multisample
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModePrivate);

    if (render_target_heap_pool_) {
      temp_texture = render_target_heap_pool_->CreateTexture(desc);
    }
    if (!temp_texture) {
      temp_texture = device_->newTexture(desc);
    }
    desc->release();
    if (!temp_texture) {
      XELOGE(
          "MetalRenderTargetCache::StoreTiledData - Failed to create "
          "temporary "
          "texture");
      return;
    }

    // Resolve multisample texture to temporary texture
    MTL::RenderPassDescriptor* resolve_desc =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    if (resolve_desc) {
      auto* color_attachment = resolve_desc->colorAttachments()->object(0);
      color_attachment->setTexture(texture);              // Multisample source
      color_attachment->setResolveTexture(temp_texture);  // Resolved output
      color_attachment->setLoadAction(MTL::LoadActionLoad);
      color_attachment->setStoreAction(MTL::StoreActionMultisampleResolve);

      MTL::RenderCommandEncoder* render_encoder =
          command_buffer->renderCommandEncoder(resolve_desc);
      if (render_encoder) {
        render_encoder->endEncoding();
        // render_encoder is autoreleased - do not release
      }
    }

    source_texture = temp_texture;
  }

  // Create compute encoder
  MTL::ComputeCommandEncoder* encoder = command_buffer->computeCommandEncoder();
  if (!encoder) {
    if (temp_texture) {
      temp_texture->release();
    }
    return;
  }

  // Set compute pipeline
  encoder->setComputePipelineState(edram_store_pipeline_);

  // Bind input texture (either original or resolved)
  encoder->setTexture(source_texture, 0);

  // Bind EDRAM buffer
  encoder->setBuffer(edram_buffer_, 0, 0);
  encoder->useResource(source_texture, MTL::ResourceUsageRead);
  encoder->useResource(edram_buffer_, MTL::ResourceUsageWrite);

  // Create parameter buffers
  uint32_t params[2] = {edram_base, pitch_tiles};
  MTL::Buffer* param_buffer = device_->newBuffer(
      &params, sizeof(params), MTL::ResourceStorageModeShared);
  encoder->setBuffer(param_buffer, 0, 1);
  encoder->setBuffer(param_buffer, sizeof(uint32_t), 2);

  // Calculate thread group sizes
  MTL::Size threads_per_threadgroup = MTL::Size::Make(8, 8, 1);
  MTL::Size threadgroups = MTL::Size::Make(
      (source_texture->width() + 7) / 8, (source_texture->height() + 7) / 8, 1);

  // Dispatch compute
  encoder->dispatchThreadgroups(threadgroups, threads_per_threadgroup);
  encoder->endEncoding();
  // encoder is autoreleased - do not release

  if (temp_texture) {
    temp_texture->release();
  }

  param_buffer->release();
}

void MetalRenderTargetCache::DumpRenderTargets(
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, MTL::CommandBuffer* command_buffer) {
  XELOGGPU(
      "MetalRenderTargetCache::DumpRenderTargets: base={} row_length_used={} "
      "rows={} pitch={}",
      dump_base, dump_row_length_used, dump_rows, dump_pitch);

  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, rectangles);

  XELOGGPU("MetalRenderTargetCache::DumpRenderTargets: {} rectangles to dump",
           rectangles.size());
  if (rectangles.empty()) {
    XELOGW(
        "MetalRenderTargetCache::DumpRenderTargets: no rectangles for base={} "
        "row_length_used={} rows={} pitch={}",
        dump_base, dump_row_length_used, dump_rows, dump_pitch);
    return;
  }

  if (!edram_buffer_) {
    XELOGW(
        "MetalRenderTargetCache::DumpRenderTargets: EDRAM buffer not "
        "initialized, skipping GPU dump");
    return;
  }

  struct EdramDumpConstants {
    uint32_t dispatch_first_tile;
    uint32_t source_base_tiles;
    uint32_t dest_pitch_tiles;
    uint32_t source_pitch_tiles;
    uint32_t resolution_scale_x;
    uint32_t resolution_scale_y;
    uint32_t tile_size_x;
    uint32_t tile_size_y;
    float tile_size_inv_x;
    float tile_size_inv_y;
    float source_pitch_tiles_inv;
    uint32_t format;
    uint32_t flags;
    uint32_t padding;
  };

  MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
  if (!queue) {
    XELOGE("MetalRenderTargetCache::DumpRenderTargets: no command queue");
    return;
  }

  ScopedAutoreleasePool autorelease_pool;
  bool owns_command_buffer = false;
  MTL::CommandBuffer* cmd = command_buffer;
  if (!cmd) {
    cmd = queue->commandBuffer();
    if (!cmd) {
      XELOGE("MetalRenderTargetCache::DumpRenderTargets: no command buffer");
      return;
    }
    owns_command_buffer = true;
  }

  MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
  if (!encoder) {
    XELOGE("MetalRenderTargetCache::DumpRenderTargets: no compute encoder");
    // cmd is autoreleased from commandBuffer() - do not release
    return;
  }

  encoder->setBuffer(edram_buffer_, 0, 0);
  encoder->useResource(edram_buffer_, MTL::ResourceUsageWrite);

  uint32_t scale_x = draw_resolution_scale_x();
  uint32_t scale_y = draw_resolution_scale_y();

  for (const ResolveCopyDumpRectangle& rect : rectangles) {
    auto* rt = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!rt) {
      continue;
    }

    RenderTargetKey key = rt->key();
    MTL::Texture* tex = rt->texture();
    if (!tex) {
      continue;
    }
    if (key.is_depth) {
      MTL::PixelFormat expected_format =
          GetDepthPixelFormat(key.GetDepthFormat());
      assert_true(tex->pixelFormat() == expected_format,
                  "Dump depth must bind resource pixel format");
    } else {
      MTL::PixelFormat expected_format =
          GetColorResourcePixelFormat(key.GetColorFormat());
      assert_true(tex->pixelFormat() == expected_format,
                  "Dump color must bind resource pixel format");
    }

    uint32_t dump_format = GetMetalEdramDumpFormat(key);
    uint32_t dump_flags = 0;
    MTL::Texture* stencil_tex = nullptr;
    if (key.is_depth) {
      if (!::cvars::depth_float24_convert_in_pixel_shader &&
          ::cvars::depth_float24_round) {
        dump_flags |= kMetalEdramDumpFlagDepthRound;
      }
      stencil_tex = GetStencilTextureView(rt);
      if (stencil_tex) {
        dump_flags |= kMetalEdramDumpFlagHasStencil;
      }
    }

    // Choose the appropriate dump pipeline based on:
    // - 32bpp vs 64bpp (key.Is64bpp())
    // - color vs depth (key.is_depth)
    // - MSAA sample count (key.msaa_samples)
    // This mirrors D3D12's dump pipeline selection: use key.Is64bpp() directly,
    // NOT IsKey64bpp() which includes gamma-as-unorm16. The EDRAM buffer is
    // always 32bpp for gamma formats; only the host texture storage is 64bpp.
    MTL::ComputePipelineState* dump_pipeline = nullptr;
    bool is_64bpp = key.Is64bpp();

    // If this is a gamma RT stored as linear RGBA16Unorm, we need to encode
    // to PWL gamma when dumping. Set the flag for the dump shader.
    if (!key.is_depth &&
        key.GetColorFormat() ==
            xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
        gamma_render_target_as_unorm16_) {
      dump_flags |= kMetalEdramDumpFlagGammaAsLinear;
    }

    if (!key.is_depth) {
      // Color render target
      if (is_64bpp) {
        // 64bpp color
        switch (key.msaa_samples) {
          case xenos::MsaaSamples::k1X:
            dump_pipeline = edram_dump_color_64bpp_1xmsaa_pipeline_;
            break;
          case xenos::MsaaSamples::k2X:
            dump_pipeline = edram_dump_color_64bpp_2xmsaa_pipeline_;
            break;
          case xenos::MsaaSamples::k4X:
            dump_pipeline = edram_dump_color_64bpp_4xmsaa_pipeline_;
            break;
          default:
            break;
        }
      } else {
        // 32bpp color
        switch (key.msaa_samples) {
          case xenos::MsaaSamples::k1X:
            dump_pipeline = edram_dump_color_32bpp_1xmsaa_pipeline_;
            break;
          case xenos::MsaaSamples::k2X:
            dump_pipeline = edram_dump_color_32bpp_2xmsaa_pipeline_;
            break;
          case xenos::MsaaSamples::k4X:
            dump_pipeline = edram_dump_color_32bpp_4xmsaa_pipeline_;
            break;
          default:
            break;
        }
      }
    } else {
      // Depth render target (always 32bpp for D24S8/D24FS8)
      switch (key.msaa_samples) {
        case xenos::MsaaSamples::k1X:
          dump_pipeline = edram_dump_depth_32bpp_1xmsaa_pipeline_;
          break;
        case xenos::MsaaSamples::k2X:
          dump_pipeline = edram_dump_depth_32bpp_2xmsaa_pipeline_;
          break;
        case xenos::MsaaSamples::k4X:
          dump_pipeline = edram_dump_depth_32bpp_4xmsaa_pipeline_;
          break;
        default:
          break;
      }
    }

    if (!dump_pipeline) {
      XELOGGPU(
          "MetalRenderTargetCache::DumpRenderTargets: no dump pipeline for "
          "key=0x{:08X} (is_depth={}, is_64bpp={}, msaa={})",
          key.key, key.is_depth ? 1 : 0, is_64bpp ? 1 : 0,
          static_cast<uint32_t>(key.msaa_samples));
      continue;
    }

    XELOGGPU(
        "MetalRenderTargetCache::DumpRenderTargets: dump RT key=0x{:08X} "
        "(is_depth={}, is_64bpp={}, msaa={}) tex={}x{} pipeline={:p}",
        key.key, key.is_depth ? 1 : 0, is_64bpp ? 1 : 0,
        static_cast<uint32_t>(key.msaa_samples), tex->width(), tex->height(),
        static_cast<void*>(dump_pipeline));

    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rect.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    if (!dispatch_count) {
      continue;
    }

    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];

      EdramDumpConstants constants;
      constants.dispatch_first_tile = dump_base + dispatch.offset;
      constants.source_base_tiles = key.base_tiles;
      constants.dest_pitch_tiles = dump_pitch;
      constants.source_pitch_tiles = key.GetPitchTiles();
      constants.resolution_scale_x = scale_x;
      constants.resolution_scale_y = scale_y;
      uint32_t tile_size_x = (is_64bpp ? 40u : 80u) * scale_x;
      uint32_t tile_size_y = 16u * scale_y;
      constants.tile_size_x = tile_size_x;
      constants.tile_size_y = tile_size_y;
      constants.tile_size_inv_x =
          tile_size_x ? (1.0f / float(tile_size_x)) : 0.0f;
      constants.tile_size_inv_y =
          tile_size_y ? (1.0f / float(tile_size_y)) : 0.0f;
      constants.source_pitch_tiles_inv =
          constants.source_pitch_tiles
              ? (1.0f / float(constants.source_pitch_tiles))
              : 0.0f;
      constants.format = dump_format;
      constants.flags = dump_flags;
      constants.padding = 0;

      encoder->setComputePipelineState(dump_pipeline);
      encoder->setTexture(tex, 0);
      if (stencil_tex) {
        encoder->setTexture(stencil_tex, 1);
      }
      encoder->useResource(tex, MTL::ResourceUsageRead);
      if (stencil_tex) {
        encoder->useResource(stencil_tex, MTL::ResourceUsageRead);
      }
      encoder->setBytes(&constants, sizeof(constants), 1);

      // Thread group dispatch:
      // - 40x16 threads per group (same as D3D12/Vulkan)
      // - For 32bpp: two groups per tile along X (80 samples / 40 threads)
      // - For 64bpp: one group per tile along X (40 samples / 40 threads)
      uint32_t groups_x = dispatch.width_tiles * scale_x;
      if (!is_64bpp) {
        groups_x <<= 1;  // Double for 32bpp
      }
      uint32_t groups_y = dispatch.height_tiles * scale_y;

      MTL::Size threads_per_group = MTL::Size::Make(40, 16, 1);
      MTL::Size threadgroups = MTL::Size::Make(groups_x, groups_y, 1);
      encoder->dispatchThreadgroups(threadgroups, threads_per_group);
    }
  }

  encoder->endEncoding();
  if (owns_command_buffer) {
    cmd->commit();
    cmd->waitUntilCompleted();
  }
  // cmd is autoreleased from commandBuffer() - do not release
}

MTL::Library* MetalRenderTargetCache::GetOrCreateEdramLoadLibrary(bool msaa) {
  MTL::Library*& library =
      msaa ? edram_load_library_msaa_ : edram_load_library_;
  if (library) {
    return library;
  }

  static const char kEdramLoadShaderSource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramLoadConstants {
  uint base_tiles;
  uint pitch_tiles;
  uint format;
  uint format_is_64bpp;
  uint msaa_samples;
  uint sample_id;
  uint resolution_scale_x;
  uint resolution_scale_y;
};

struct VSOut {
  float4 position [[position]];
};

vertex VSOut edram_load_vs(uint vid [[vertex_id]]) {
  float2 pt = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(pt * 2.0f - 1.0f, 0.0f, 1.0f);
  return out;
}

constant uint kXenosMsaaSamples1X = 0u;
constant uint kXenosMsaaSamples2X = 1u;
constant uint kXenosMsaaSamples4X = 2u;
constant uint kEdramTileCount = 2048u;

uint XeEdramOffsetInts(uint2 pixel_index, uint base_tiles, bool wrap,
                       uint pitch_tiles, uint msaa_samples, bool is_depth,
                       uint format_ints_log2, uint pixel_sample_index,
                       uint2 resolution_scale) {
  uint msaa_samples_x_log2 = (msaa_samples >= kXenosMsaaSamples4X) ? 1u : 0u;
  uint msaa_samples_y_log2 = (msaa_samples >= kXenosMsaaSamples2X) ? 1u : 0u;
  uint2 rt_sample_index =
      pixel_index << uint2(msaa_samples_x_log2, msaa_samples_y_log2);
  rt_sample_index +=
      (uint2(pixel_sample_index) >> uint2(1u, 0u)) & 1u;
  uint2 tile_size_at_32bpp = uint2(80u, 16u) * resolution_scale;
  uint2 tile_size_samples =
      tile_size_at_32bpp >> uint2(format_ints_log2, 0u);
  uint2 tile_offset_xy = rt_sample_index / tile_size_samples;
  base_tiles += tile_offset_xy.y * pitch_tiles + tile_offset_xy.x;
  rt_sample_index -= tile_offset_xy * tile_size_samples;
  if (is_depth) {
    uint tile_width_half = tile_size_samples.x >> 1u;
    rt_sample_index.x =
        uint(int(rt_sample_index.x) +
             ((rt_sample_index.x >= tile_width_half)
                  ? -int(tile_width_half)
                  : int(tile_width_half)));
  }
  uint address =
      base_tiles * (tile_size_at_32bpp.x * tile_size_at_32bpp.y) +
      ((rt_sample_index.y * tile_size_samples.x + rt_sample_index.x) <<
       format_ints_log2);
  if (wrap) {
    address %= tile_size_at_32bpp.x * tile_size_at_32bpp.y * kEdramTileCount;
  }
  return address;
}

float XeFloat7e3To32(uint f10) {
  f10 &= 0x3FFu;
  if (f10 == 0u) {
    return 0.0f;
  }
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint mantissa_lzcnt = clz(mantissa) - 24u;
    exponent = uint(int(1) - int(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7Fu;
  }
  uint f32 = ((exponent + 124u) << 23u) | (mantissa << 16u);
  return as_type<float>(f32);
}

float4 XeUnpackR8G8B8A8UNorm(uint packed) {
  float4 value = float4(packed & 0xFFu, (packed >> 8u) & 0xFFu,
                        (packed >> 16u) & 0xFFu, packed >> 24u);
  return value * (1.0f / 255.0f);
}

float4 XeUnpackR10G10B10A2UNorm(uint packed) {
  float4 value = float4(packed & 0x3FFu, (packed >> 10u) & 0x3FFu,
                        (packed >> 20u) & 0x3FFu, (packed >> 30u) & 0x3u);
  return value * float4(1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 1023.0f,
                        1.0f / 3.0f);
}

float4 XeUnpackR10G10B10A2Float(uint packed) {
  float r = XeFloat7e3To32(packed & 0x3FFu);
  float g = XeFloat7e3To32((packed >> 10u) & 0x3FFu);
  float b = XeFloat7e3To32((packed >> 20u) & 0x3FFu);
  float a = float((packed >> 30u) & 0x3u) * (1.0f / 3.0f);
  return float4(r, g, b, a);
}

float2 XeUnpackR16G16Edram(uint packed) {
  int r = int(packed << 16u) >> 16u;
  int g = int(packed) >> 16u;
  float2 value = float2(float(r), float(g)) * (32.0f / 32767.0f);
  return max(value, float2(-1.0f));
}

float4 XeUnpackR16G16B16A16Edram(uint2 packed) {
  int r = int(packed.x << 16u) >> 16u;
  int g = int(packed.x) >> 16u;
  int b = int(packed.y << 16u) >> 16u;
  int a = int(packed.y) >> 16u;
  float4 value = float4(float(r), float(g), float(b), float(a)) *
                 (32.0f / 32767.0f);
  return max(value, float4(-1.0f));
}

float2 XeUnpackHalf2(uint packed) {
  return float2(as_type<half2>(packed));
}

float4 XeUnpackColor32bpp(uint format, uint packed) {
  switch (format) {
    case 0u:  // kXenosColorRenderTargetFormat_8_8_8_8
    case 1u:  // kXenosColorRenderTargetFormat_8_8_8_8_GAMMA
      return XeUnpackR8G8B8A8UNorm(packed);
    case 2u:  // kXenosColorRenderTargetFormat_2_10_10_10
    case 10u: // kXenosColorRenderTargetFormat_2_10_10_10_AS_10_10_10_10
      return XeUnpackR10G10B10A2UNorm(packed);
    case 3u:  // kXenosColorRenderTargetFormat_2_10_10_10_FLOAT
    case 12u: // kXenosColorRenderTargetFormat_2_10_10_10_FLOAT_AS_16_16_16_16
      return XeUnpackR10G10B10A2Float(packed);
    case 4u: {  // kXenosColorRenderTargetFormat_16_16
      float2 rg = XeUnpackR16G16Edram(packed);
      return float4(rg, 0.0f, 1.0f);
    }
    case 6u: {  // kXenosColorRenderTargetFormat_16_16_FLOAT
      float2 rg = XeUnpackHalf2(packed);
      return float4(rg, 0.0f, 1.0f);
    }
    case 14u:  // kXenosColorRenderTargetFormat_32_FLOAT
      return float4(as_type<float>(packed), 0.0f, 0.0f, 1.0f);
    default:
      return float4(0.0f);
  }
}

float4 XeUnpackColor64bpp(uint format, uint2 packed) {
  switch (format) {
    case 5u:  // kXenosColorRenderTargetFormat_16_16_16_16
      return XeUnpackR16G16B16A16Edram(packed);
    case 7u: {  // kXenosColorRenderTargetFormat_16_16_16_16_FLOAT
      float2 rg = XeUnpackHalf2(packed.x);
      float2 ba = XeUnpackHalf2(packed.y);
      return float4(rg, ba);
    }
    case 15u:  // kXenosColorRenderTargetFormat_32_32_FLOAT
      return float4(as_type<float>(packed.x), as_type<float>(packed.y),
                    0.0f, 0.0f);
    default:
      return float4(0.0f);
  }
}

struct EdramLoadOut {
  float4 color [[color(0)]];
#if XE_EDRAM_LOAD_MSAA
  uint sample_mask [[sample_mask]];
#endif
};

fragment EdramLoadOut edram_load_ps(
    VSOut in [[stage_in]],
    constant EdramLoadConstants& constants [[buffer(0)]],
    device const uint* edram [[buffer(1)]]) {
  uint2 pixel = uint2(in.position.xy);
  uint format_ints_log2 = constants.format_is_64bpp;
  uint address = XeEdramOffsetInts(
      pixel, constants.base_tiles, true, constants.pitch_tiles,
      constants.msaa_samples, false, format_ints_log2, constants.sample_id,
      uint2(constants.resolution_scale_x, constants.resolution_scale_y));
  float4 color;
  if (constants.format_is_64bpp != 0u) {
    uint2 packed = uint2(edram[address], edram[address + 1u]);
    color = XeUnpackColor64bpp(constants.format, packed);
  } else {
    color = XeUnpackColor32bpp(constants.format, edram[address]);
  }
  EdramLoadOut out;
  out.color = color;
#if XE_EDRAM_LOAD_MSAA
  out.sample_mask = 1u << (constants.sample_id & 0x1Fu);
#endif
  return out;
}
)METAL";

  std::string source;
  source.reserve(sizeof(kEdramLoadShaderSource) + 32);
  source.append(msaa ? "#define XE_EDRAM_LOAD_MSAA 1\n"
                     : "#define XE_EDRAM_LOAD_MSAA 0\n");
  source.append(kEdramLoadShaderSource);

  NS::Error* error = nullptr;
  auto source_str = NS::String::string(source.c_str(), NS::UTF8StringEncoding);
  library = device_->newLibrary(source_str, nullptr, &error);
  if (!library) {
    XELOGE("Metal: failed to compile edram load shader: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
  }
  return library;
}

MTL::RenderPipelineState* MetalRenderTargetCache::GetOrCreateEdramLoadPipeline(
    MTL::PixelFormat dest_format, uint32_t sample_count) {
  uint64_t key = uint64_t(dest_format) | (uint64_t(sample_count) << 32);
  auto it = edram_load_pipelines_.find(key);
  if (it != edram_load_pipelines_.end()) {
    return it->second;
  }

  bool msaa = sample_count > 1;
  MTL::Library* lib = GetOrCreateEdramLoadLibrary(msaa);
  if (!lib) {
    return nullptr;
  }

  NS::String* vs_name =
      NS::String::string("edram_load_vs", NS::UTF8StringEncoding);
  NS::String* ps_name =
      NS::String::string("edram_load_ps", NS::UTF8StringEncoding);
  MTL::Function* vs = lib->newFunction(vs_name);
  MTL::Function* ps = lib->newFunction(ps_name);
  if (!vs || !ps) {
    if (vs) {
      vs->release();
    }
    if (ps) {
      ps->release();
    }
    XELOGE("Metal: edram load missing shader entrypoints");
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs);
  desc->setFragmentFunction(ps);
  desc->colorAttachments()->object(0)->setPixelFormat(dest_format);
  desc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
  desc->setSampleCount(sample_count);

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);
  desc->release();
  vs->release();
  ps->release();

  if (!pipeline) {
    XELOGE("Metal: failed to create edram load pipeline: {}",
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  edram_load_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool MetalRenderTargetCache::Resolve(Memory& memory, uint32_t& written_address,
                                     uint32_t& written_length,
                                     MTL::CommandBuffer* command_buffer) {
  written_address = 0;
  written_length = 0;
  const RegisterFile& regs = register_file();
  draw_util::ResolveInfo resolve_info;

  // Fixed16 formats may be truncated to -1..1 when backed by SNORM.
  bool fixed_rg16_trunc = IsFixedRG16TruncatedToMinus1To1();
  bool fixed_rgba16_trunc = IsFixedRGBA16TruncatedToMinus1To1();

  if (!trace_writer_) {
    XELOGE("MetalRenderTargetCache::Resolve: trace_writer_ is null");
    return false;
  }

  if (!draw_util::GetResolveInfo(regs, memory, *trace_writer_,
                                 draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), fixed_rg16_trunc,
                                 fixed_rgba16_trunc, resolve_info)) {
    XELOGE("MetalRenderTargetCache::Resolve: GetResolveInfo failed");
    return false;
  }

  // Nothing to do.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  bool is_depth = resolve_info.IsCopyingDepth();

  if (!resolve_info.copy_dest_extent_length) {
    return true;
  }

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  const auto& coord = resolve_info.coordinate_info;
  uint32_t resolve_width = coord.width_div_8 * 8;
  uint32_t resolve_height = resolve_info.height_div_8 * 8;

  // Compute the EDRAM tile span for this resolve.
  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows,
                                    dump_pitch);
  // Match D3D12/Vulkan: dump host RT ownership into EDRAM, then resolve
  // from EDRAM to shared memory. Resolve-time blend fallback is not correct
  // because blending state is per-draw, not per-resolve.
  DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                    command_buffer);

  uint32_t dest_base = resolve_info.copy_dest_base;
  uint32_t dest_local_start = resolve_info.copy_dest_extent_start - dest_base;
  uint32_t dest_local_end =
      dest_local_start + resolve_info.copy_dest_extent_length;

  command_processor_.SetSwapDestSwap(
      dest_base, resolve_info.copy_dest_info.copy_dest_swap);

  // Color resolves are 8888; depth resolves may use different destination
  // formats, so only apply the 4-byte-per-pixel assumption to color.
  uint32_t bytes_per_pixel = 4;

  // Try GPU compute resolve first (RT -> EDRAM -> shared memory), matching
  // D3D12/Vulkan behavior for the supported cases.
  if (edram_buffer_) {
    draw_util::ResolveCopyShaderConstants copy_constants;
    uint32_t group_count_x = 0, group_count_y = 0;
    draw_util::ResolveCopyShaderIndex copy_shader = resolve_info.GetCopyShader(
        draw_resolution_scale_x(), draw_resolution_scale_y(), copy_constants,
        group_count_x, group_count_y);

    // Select the appropriate Metal pipeline for this shader.
    MTL::ComputePipelineState* pipeline = nullptr;
    if (draw_resolution_scaled) {
      switch (copy_shader) {
        case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
          pipeline = resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
          pipeline = resolve_fast_32bpp_4xmsaa_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
          pipeline = resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
          pipeline = resolve_fast_64bpp_4xmsaa_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull8bpp:
          pipeline = resolve_full_8bpp_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull16bpp:
          pipeline = resolve_full_16bpp_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull32bpp:
          pipeline = resolve_full_32bpp_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull64bpp:
          pipeline = resolve_full_64bpp_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull128bpp:
          pipeline = resolve_full_128bpp_scaled_pipeline_;
          break;
        default:
          pipeline = nullptr;
          break;
      }
    } else {
      switch (copy_shader) {
        case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
          pipeline = resolve_fast_32bpp_1x2xmsaa_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
          pipeline = resolve_fast_32bpp_4xmsaa_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
          pipeline = resolve_fast_64bpp_1x2xmsaa_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
          pipeline = resolve_fast_64bpp_4xmsaa_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull8bpp:
          pipeline = resolve_full_8bpp_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull16bpp:
          pipeline = resolve_full_16bpp_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull32bpp:
          pipeline = resolve_full_32bpp_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull64bpp:
          pipeline = resolve_full_64bpp_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull128bpp:
          pipeline = resolve_full_128bpp_pipeline_;
          break;
        default:
          pipeline = nullptr;
          break;
      }
    }
    if (draw_resolution_scaled && !pipeline) {
      static uint32_t missing_scaled_pipeline_log_count = 0;
      if (missing_scaled_pipeline_log_count < 8) {
        ++missing_scaled_pipeline_log_count;
        XELOGW("MetalResolve: scaled resolve pipeline missing for shader {}",
               int(copy_shader));
      }
    }

    if (pipeline && group_count_x && group_count_y) {
      uint32_t dest_pitch_pixels =
          copy_constants.dest_relative.dest_coordinate_info.pitch_aligned_div_32
          << 5;
      if (dest_pitch_pixels < resolve_width) {
        uint32_t new_pitch_pixels = (resolve_width + 31) & ~31u;
        XELOGW(
            "MetalResolve: overriding dest pitch {} -> {} "
            "(resolve_width={})",
            dest_pitch_pixels, new_pitch_pixels, resolve_width);
        copy_constants.dest_relative.dest_coordinate_info.pitch_aligned_div_32 =
            new_pitch_pixels >> 5;
      }
      auto* shared = command_processor_.shared_memory();
      auto* texture_cache = command_processor_.texture_cache();
      MTL::Buffer* dest_buffer = nullptr;
      size_t dest_buffer_offset = 0;
      size_t dest_buffer_length = 0;
      const uint8_t* shared_bytes = nullptr;
      uint32_t scaled_range_length = 0;
      if (draw_resolution_scaled) {
        auto* metal_texture_cache =
            texture_cache ? static_cast<MetalTextureCache*>(texture_cache)
                          : nullptr;
        if (!metal_texture_cache) {
          XELOGE("MetalResolve: missing MetalTextureCache for scaled resolve");
          return false;
        }
        uint32_t range_length = resolve_info.copy_dest_extent_start -
                                resolve_info.copy_dest_base +
                                resolve_info.copy_dest_extent_length;
        scaled_range_length = range_length;
        if (!metal_texture_cache->EnsureScaledResolveMemoryCommitted(
                resolve_info.copy_dest_extent_start,
                resolve_info.copy_dest_extent_length) ||
            !metal_texture_cache->MakeScaledResolveRangeCurrent(
                resolve_info.copy_dest_base, range_length) ||
            !metal_texture_cache->GetCurrentScaledResolveBuffer(
                dest_buffer, dest_buffer_offset, dest_buffer_length)) {
          XELOGE("MetalResolve: failed to select scaled resolve buffer");
          return false;
        }
        (void)dest_buffer_length;
      } else {
        dest_buffer = shared ? shared->GetBuffer() : nullptr;
        if (!dest_buffer) {
          XELOGE("MetalResolve: missing shared memory buffer");
          return false;
        }
        // Request the destination shared memory range before the GPU write,
        // mirroring D3D12/Vulkan behavior. This ensures pages are committed and
        // any CPU data is uploaded before the GPU overwrites it.
        if (!shared->RequestRange(resolve_info.copy_dest_extent_start,
                                  resolve_info.copy_dest_extent_length)) {
          XELOGE(
              "MetalRenderTargetCache::Resolve: RequestRange failed for "
              "0x{:08X} len {}",
              resolve_info.copy_dest_extent_start,
              resolve_info.copy_dest_extent_length);
          return false;
        }

        shared_bytes = static_cast<const uint8_t*>(dest_buffer->contents());
      }
      if (draw_resolution_scaled) {
      }

      MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();

      if (!queue) {
        XELOGE(
            "MetalRenderTargetCache::Resolve: no command queue for GPU path");
      } else {
        ScopedAutoreleasePool autorelease_pool;
        bool owns_command_buffer = false;
        MTL::CommandBuffer* cmd = command_buffer;
        if (!cmd) {
          cmd = queue->commandBuffer();
          if (!cmd) {
            XELOGE(
                "MetalRenderTargetCache::Resolve: failed to get command "
                "buffer for GPU path");
            cmd = nullptr;
          }
          owns_command_buffer = true;
        }
        if (cmd) {
          MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
          if (!encoder) {
            XELOGE(
                "MetalRenderTargetCache::Resolve: failed to get compute "
                "encoder for GPU path");
            // cmd is autoreleased from commandBuffer() - do not release
          } else {
            encoder->setComputePipelineState(pipeline);

            // Buffer 0: push constants
            if (draw_resolution_scaled) {
              encoder->setBytes(&copy_constants.dest_relative,
                                sizeof(copy_constants.dest_relative), 0);
            } else {
              encoder->setBytes(&copy_constants, sizeof(copy_constants), 0);
            }

            // Buffer 1: destination memory (shared or scaled resolve).
            encoder->setBuffer(dest_buffer, dest_buffer_offset, 1);

            // Buffer 2: EDRAM source buffer.
            encoder->setBuffer(edram_buffer_, 0, 2);
            encoder->useResource(dest_buffer, MTL::ResourceUsageWrite);
            encoder->useResource(edram_buffer_, MTL::ResourceUsageRead);

            encoder->dispatchThreadgroups(
                MTL::Size::Make(group_count_x, group_count_y, 1),
                MTL::Size::Make(8, 8, 1));

            encoder->endEncoding();
            if (owns_command_buffer) {
              cmd->commit();
              cmd->waitUntilCompleted();
            }
            // cmd is autoreleased from commandBuffer() - do not release

            written_address = resolve_info.copy_dest_extent_start;
            written_length = resolve_info.copy_dest_extent_length;

            // Mark the shared memory range as GPU-written resolve data so
            // texture caches and trace dumping can see it without an extra
            // CPU copy. This mirrors D3D12/Vulkan behavior.
            if (!draw_resolution_scaled) {
              if (auto* shared_after = command_processor_.shared_memory()) {
                shared_after->RangeWrittenByGpu(written_address,
                                                written_length);
              }
            }

            // Mark the range as resolved in the texture cache so that any
            // textures overlapping this range will be reloaded from the
            // updated shared memory. This matches D3D12/Vulkan behavior.
            if (auto* tex_cache = command_processor_.texture_cache()) {
              tex_cache->MarkRangeAsResolved(written_address, written_length);
            }

            bool clear_depth = resolve_info.IsClearingDepth();
            bool clear_color = resolve_info.IsClearingColor();
            if (clear_depth || clear_color) {
              Transfer::Rectangle clear_rectangle;
              RenderTarget* clear_targets[2] = {};
              std::vector<Transfer> clear_transfers[2];
              if (PrepareHostRenderTargetsResolveClear(
                      resolve_info, clear_rectangle, clear_targets[0],
                      clear_transfers[0], clear_targets[1],
                      clear_transfers[1])) {
                uint64_t clear_values[2];
                clear_values[0] = resolve_info.rb_depth_clear;
                clear_values[1] =
                    resolve_info.rb_color_clear |
                    (uint64_t(resolve_info.rb_color_clear_lo) << 32);
                PerformTransfersAndResolveClears(
                    2, clear_targets, clear_transfers, clear_values,
                    &clear_rectangle, command_buffer);
              }
            }
            return true;
          }
        }
      }
    }
  }

  XELOGE(
      "MetalRenderTargetCache::Resolve: no valid GPU resolve shader / pipeline "
      "for this configuration");
  return false;
}

void MetalRenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle,
    MTL::CommandBuffer* command_buffer) {
  if (!render_targets || !render_target_transfers) {
    return;
  }

  bool resolve_clear_needed =
      render_target_resolve_clear_values && resolve_clear_rectangle;
  bool any_work = false;
  bool host_depth_store_needed = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    if (resolve_clear_needed) {
      any_work = true;
    }
    const std::vector<Transfer>& transfers = render_target_transfers[i];
    if (transfers.empty()) {
      continue;
    }
    any_work = true;
    if (!dest_rt->key().is_depth) {
      continue;
    }
    for (const Transfer& transfer : transfers) {
      if (transfer.host_depth_source == dest_rt) {
        host_depth_store_needed = true;
        break;
      }
    }
  }
  if (!any_work) {
    return;
  }

  MTL::CommandBuffer* cmd = command_buffer;
  if (!cmd) {
    cmd = command_processor_.EnsureCommandBuffer();
  }
  if (!cmd) {
    XELOGE(
        "MetalRenderTargetCache::PerformTransfersAndResolveClears: no command "
        "buffer");
    return;
  }

  command_processor_.EndRenderEncoder();

  uint32_t scale_x = draw_resolution_scale_x();
  uint32_t scale_y = draw_resolution_scale_y();
  uint32_t tile_width_samples =
      xenos::kEdramTileWidthSamples * draw_resolution_scale_x();
  uint32_t tile_height_samples =
      xenos::kEdramTileHeightSamples * draw_resolution_scale_y();
  uint32_t depth_round = (!::cvars::depth_float24_convert_in_pixel_shader &&
                          ::cvars::depth_float24_round)
                             ? 1u
                             : 0u;

  // Host depth store pass (dest depth where host depth source == dest).
  bool host_depth_store_dispatched = false;
  if (host_depth_store_needed) {
    for (uint32_t i = 0; i < render_target_count; ++i) {
      RenderTarget* dest_rt = render_targets[i];
      if (!dest_rt) {
        continue;
      }
      RenderTargetKey dest_key = dest_rt->key();
      if (!dest_key.is_depth) {
        continue;
      }
      const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
      for (const Transfer& transfer : depth_transfers) {
        if (transfer.host_depth_source != dest_rt) {
          continue;
        }
        auto* dest_metal_rt = static_cast<MetalRenderTarget*>(dest_rt);
        MTL::Texture* depth_texture = dest_metal_rt->texture();
        if (!depth_texture || !edram_buffer_) {
          continue;
        }
        size_t pipeline_index = size_t(dest_key.msaa_samples);
        if (pipeline_index >= xe::countof(host_depth_store_pipelines_) ||
            !host_depth_store_pipelines_[pipeline_index]) {
          XELOGE(
              "MetalRenderTargetCache::PerformTransfersAndResolveClears: "
              "missing host depth store pipeline for msaa={}",
              uint32_t(dest_key.msaa_samples));
          continue;
        }
        Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
        uint32_t rectangle_count = transfer.GetRectangles(
            dest_key.base_tiles, dest_key.pitch_tiles_at_32bpp,
            dest_key.msaa_samples, false, rectangles, resolve_clear_rectangle);
        if (!rectangle_count) {
          continue;
        }
        HostDepthStoreRenderTargetConstant render_target_constant =
            GetHostDepthStoreRenderTargetConstant(dest_key.pitch_tiles_at_32bpp,
                                                  msaa_2x_supported_);
        MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
        if (!encoder) {
          XELOGE(
              "MetalRenderTargetCache::PerformTransfersAndResolveClears: "
              "failed to create host depth store encoder");
          continue;
        }
        encoder->setComputePipelineState(
            host_depth_store_pipelines_[pipeline_index]);
        encoder->setBuffer(edram_buffer_, 0, 1);
        encoder->setTexture(depth_texture, 0);
        encoder->useResource(edram_buffer_, MTL::ResourceUsageWrite);
        encoder->useResource(depth_texture, MTL::ResourceUsageRead);
        for (uint32_t rect_index = 0; rect_index < rectangle_count;
             ++rect_index) {
          uint32_t group_count_x = 0;
          uint32_t group_count_y = 0;
          HostDepthStoreRectangleConstant rectangle_constant;
          GetHostDepthStoreRectangleInfo(
              rectangles[rect_index], dest_key.msaa_samples, rectangle_constant,
              group_count_x, group_count_y);
          if (!group_count_x || !group_count_y) {
            continue;
          }
          HostDepthStoreConstants constants = {};
          constants.rectangle = rectangle_constant;
          constants.render_target = render_target_constant;
          encoder->setBytes(&constants, sizeof(constants), 0);
          encoder->dispatchThreadgroups(
              MTL::Size::Make(group_count_x, group_count_y, 1),
              MTL::Size::Make(8, 8, 1));
          host_depth_store_dispatched = true;
        }
        encoder->endEncoding();
      }
      break;
    }
  }

  bool any_transfers_done = false;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& transfers = render_target_transfers[i];
    if (transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto* dest_metal_rt = static_cast<MetalRenderTarget*>(dest_rt);
    if (dest_metal_rt->needs_initial_clear()) {
      dest_metal_rt->SetNeedsInitialClear(false);
      render_pass_descriptor_dirty_ = true;
    }
    RenderTargetKey dest_key = dest_metal_rt->key();
    bool dest_is_depth = dest_key.is_depth;

    bool dest_is_uint = false;
    MTL::PixelFormat dest_pixel_format =
        dest_is_depth ? GetDepthPixelFormat(dest_key.GetDepthFormat())
                      : GetColorOwnershipTransferPixelFormat(
                            dest_key.GetColorFormat(), &dest_is_uint);

    MTL::Texture* dest_texture = dest_is_depth
                                     ? dest_metal_rt->texture()
                                     : dest_metal_rt->transfer_texture();
    if (!dest_texture) {
      XELOGW(
          "MetalRenderTargetCache::PerformTransfersAndResolveClears: "
          "Destination RT {} has no texture",
          i);
      continue;
    }
    if (dest_is_depth) {
      assert_true(dest_texture->pixelFormat() == dest_pixel_format,
                  "Transfer depth must use resource pixel format");
    } else {
      assert_true(dest_texture->pixelFormat() == dest_pixel_format,
                  "Transfer color must use ownership pixel format");
    }

    uint32_t dest_sample_count = MsaaSamplesToCount(dest_key.msaa_samples);
    bool transfer_use_sample_id_default =
        dest_sample_count > 1 && ::cvars::metal_transfer_msaa_sample_id;
    uint32_t dest_width = uint32_t(dest_texture->width());
    uint32_t dest_height = uint32_t(dest_texture->height());

    auto get_scaled_rect = [&](const Transfer::Rectangle& rect,
                               uint32_t& scaled_x, uint32_t& scaled_y,
                               uint32_t& scaled_width,
                               uint32_t& scaled_height) -> bool {
      uint32_t rect_x = rect.x_pixels * scale_x;
      uint32_t rect_y = rect.y_pixels * scale_y;
      uint32_t rect_width = rect.width_pixels * scale_x;
      uint32_t rect_height = rect.height_pixels * scale_y;
      if (rect_x >= dest_width || rect_y >= dest_height) {
        return false;
      }
      rect_width = std::min(rect_width, dest_width - rect_x);
      rect_height = std::min(rect_height, dest_height - rect_y);
      if (!rect_width || !rect_height) {
        return false;
      }
      scaled_x = rect_x;
      scaled_y = rect_y;
      scaled_width = rect_width;
      scaled_height = rect_height;
      return true;
    };

    auto set_rect_viewport = [&](MTL::RenderCommandEncoder* encoder,
                                 const Transfer::Rectangle& rect) -> bool {
      uint32_t scaled_x = 0;
      uint32_t scaled_y = 0;
      uint32_t scaled_width = 0;
      uint32_t scaled_height = 0;
      if (!get_scaled_rect(rect, scaled_x, scaled_y, scaled_width,
                           scaled_height)) {
        return false;
      }
      MTL::Viewport vp;
      vp.originX = double(scaled_x);
      vp.originY = double(scaled_y);
      vp.width = double(scaled_width);
      vp.height = double(scaled_height);
      vp.znear = 0.0;
      vp.zfar = 1.0;
      encoder->setViewport(vp);
      MTL::ScissorRect scissor;
      scissor.x = scaled_x;
      scissor.y = scaled_y;
      scissor.width = scaled_width;
      scissor.height = scaled_height;
      encoder->setScissorRect(scissor);
      return true;
    };

    struct TransferTileBatch {
      MTL::Buffer* buffer = nullptr;
      size_t buffer_offset = 0;
      uint32_t instance_count = 0;
      MTL::ScissorRect scissor = {};
    };

    struct TransferTileBatchBuildInfo {
      TransferTileBatch batch;
      uint32_t tile_x_start = 0;
      uint32_t tile_x_end = 0;
      uint32_t tile_y_start = 0;
      uint32_t tile_y_end = 0;
    };
    bool transfer_tile_instance_budget_hit = false;
    bool transfer_tile_instance_adaptive_cutoff_hit = false;
    size_t transfer_tile_instance_adaptive_candidate_bytes = 0;
    size_t transfer_tile_instance_adaptive_limit_bytes = 0;
    uint32_t transfer_tile_instance_adaptive_rect_count = 0;
    bool transfer_tile_instance_predictive_cutoff_hit = false;
    size_t transfer_tile_instance_predictive_used_bytes = 0;
    size_t transfer_tile_instance_predictive_candidate_bytes = 0;
    size_t transfer_tile_instance_predictive_threshold_bytes = 0;

    auto allocate_instance_buffer = [&](size_t size, MTL::Buffer*& buffer,
                                        size_t& offset) -> bool {
      if (!device_) {
        return false;
      }
      uint32_t buffer_index =
          uint32_t(frame_id_ % kTransferInstanceBufferCount);
      if (transfer_tile_instance_buffer_frame_id_ != frame_id_) {
        transfer_tile_instance_buffer_frame_id_ = frame_id_;
        transfer_tile_instance_buffer_offset_ = 0;
        auto& retired_buffers =
            transfer_tile_instance_retired_buffers_[buffer_index];
        for (auto* retired_buffer : retired_buffers) {
          if (retired_buffer) {
            retired_buffer->release();
          }
        }
        retired_buffers.clear();
      }
      constexpr size_t kAlignment = 256;
      size_t aligned_offset =
          xe::align(transfer_tile_instance_buffer_offset_, size_t(kAlignment));
      if (size > kTransferTileInstanceBufferMaxBytes ||
          aligned_offset > kTransferTileInstanceBufferMaxBytes ||
          aligned_offset > (kTransferTileInstanceBufferMaxBytes - size)) {
        transfer_tile_instance_budget_hit = true;
        return false;
      }
      size_t required = aligned_offset + size;
      if (transfer_tile_instance_buffers_[buffer_index] &&
          transfer_tile_instance_buffer_sizes_[buffer_index] < required &&
          aligned_offset != 0) {
        transfer_tile_instance_budget_hit = true;
        return false;
      }
      if (!transfer_tile_instance_buffers_[buffer_index] ||
          transfer_tile_instance_buffer_sizes_[buffer_index] < required) {
        size_t new_size = xe::round_up<size_t>(required, 65536);
        if (new_size > kTransferTileInstanceBufferMaxBytes) {
          new_size = kTransferTileInstanceBufferMaxBytes;
        }
        if (new_size < required) {
          transfer_tile_instance_budget_hit = true;
          return false;
        }
        if (transfer_tile_instance_buffers_[buffer_index]) {
          transfer_tile_instance_retired_buffers_[buffer_index].push_back(
              transfer_tile_instance_buffers_[buffer_index]);
          transfer_tile_instance_buffers_[buffer_index] = nullptr;
        }
        MTL::ResourceOptions options = MTL::ResourceStorageModeShared |
                                       MTL::ResourceCPUCacheModeWriteCombined;
        MTL::Buffer* new_buffer = device_->newBuffer(new_size, options);
        if (!new_buffer) {
          transfer_tile_instance_buffer_sizes_[buffer_index] = 0;
          return false;
        }
        transfer_tile_instance_buffers_[buffer_index] = new_buffer;
        transfer_tile_instance_buffer_sizes_[buffer_index] = new_size;
      }
      buffer = transfer_tile_instance_buffers_[buffer_index];
      if (!buffer) {
        return false;
      }
      offset = aligned_offset;
      transfer_tile_instance_buffer_offset_ = aligned_offset + size;
      return true;
    };

    auto build_tile_batches =
        [&](const Transfer::Rectangle* rectangles, uint32_t rectangle_count,
            const TransferShaderConstants& constants, bool uses_host_depth,
            bool host_depth_is_copy,
            std::vector<TransferTileBatch>& out_batches) -> bool {
      out_batches.clear();
      if (!constants.dest_tile_width_pixels ||
          !constants.dest_tile_height_pixels) {
        return false;
      }
      if (!cmd) {
        return false;
      }
      uint32_t max_tile_x =
          (dest_width + constants.dest_tile_width_pixels - 1) /
          constants.dest_tile_width_pixels;
      uint32_t max_tile_y =
          (dest_height + constants.dest_tile_height_pixels - 1) /
          constants.dest_tile_height_pixels;
      if (!max_tile_x || !max_tile_y) {
        return false;
      }
      --max_tile_x;
      --max_tile_y;
      std::vector<TransferTileBatchBuildInfo> build_infos;
      size_t total_instance_bytes = 0;
      uint64_t total_covered_pixels = 0;
      constexpr size_t kAlignment = 256;
      for (uint32_t rect_index = 0; rect_index < rectangle_count;
           ++rect_index) {
        uint32_t scaled_x = 0;
        uint32_t scaled_y = 0;
        uint32_t scaled_width = 0;
        uint32_t scaled_height = 0;
        if (!get_scaled_rect(rectangles[rect_index], scaled_x, scaled_y,
                             scaled_width, scaled_height)) {
          continue;
        }
        uint32_t tile_x_start = scaled_x / constants.dest_tile_width_pixels;
        uint32_t tile_y_start = scaled_y / constants.dest_tile_height_pixels;
        uint32_t tile_x_end =
            (scaled_x + scaled_width - 1) / constants.dest_tile_width_pixels;
        uint32_t tile_y_end =
            (scaled_y + scaled_height - 1) / constants.dest_tile_height_pixels;
        tile_x_end = std::min(tile_x_end, max_tile_x);
        tile_y_end = std::min(tile_y_end, max_tile_y);
        if (tile_x_start > tile_x_end || tile_y_start > tile_y_end) {
          continue;
        }
        uint32_t tiles_x = tile_x_end - tile_x_start + 1;
        uint32_t tiles_y = tile_y_end - tile_y_start + 1;
        uint32_t tile_count = tiles_x * tiles_y;
        if (!tile_count) {
          continue;
        }
        TransferTileBatchBuildInfo info;
        info.tile_x_start = tile_x_start;
        info.tile_x_end = tile_x_end;
        info.tile_y_start = tile_y_start;
        info.tile_y_end = tile_y_end;
        info.batch.instance_count = tile_count;
        info.batch.scissor.x = scaled_x;
        info.batch.scissor.y = scaled_y;
        info.batch.scissor.width = scaled_width;
        info.batch.scissor.height = scaled_height;
        total_covered_pixels +=
            uint64_t(scaled_width) * uint64_t(scaled_height);
        total_instance_bytes = xe::align(total_instance_bytes, kAlignment);
        info.batch.buffer_offset = total_instance_bytes;
        total_instance_bytes +=
            size_t(tile_count) * sizeof(TransferTileInstance);
        build_infos.push_back(info);
      }

      if (build_infos.empty() || !total_instance_bytes) {
        return false;
      }

      size_t adaptive_soft_limit_bytes = kTransferTileInstanceSoftBaseBytes;
      if (dest_width && dest_height) {
        uint64_t dest_pixels = uint64_t(dest_width) * uint64_t(dest_height);
        if (total_covered_pixels *
                kTransferTileInstanceLowCoverageRatioDivisor <=
            dest_pixels) {
          adaptive_soft_limit_bytes = kTransferTileInstanceSoftLowCoverageBytes;
        } else if (total_covered_pixels *
                       kTransferTileInstanceMediumCoverageRatioDivisor <=
                   dest_pixels) {
          adaptive_soft_limit_bytes =
              (kTransferTileInstanceSoftBaseBytes +
               kTransferTileInstanceSoftLowCoverageBytes) /
              2;
        }
      }
      if (build_infos.size() <= kTransferTileInstanceSmallRectPenaltyCount &&
          adaptive_soft_limit_bytes > (kAlignment * 64)) {
        size_t penalized_soft_limit_bytes =
            adaptive_soft_limit_bytes *
            kTransferTileInstanceSmallRectPenaltyNumerator /
            kTransferTileInstanceSmallRectPenaltyDenominator;
        adaptive_soft_limit_bytes =
            std::max(penalized_soft_limit_bytes, kAlignment * 64);
      }
      adaptive_soft_limit_bytes = std::min(adaptive_soft_limit_bytes,
                                           kTransferTileInstanceBufferMaxBytes);
      if (total_instance_bytes > adaptive_soft_limit_bytes) {
        transfer_tile_instance_adaptive_cutoff_hit = true;
        transfer_tile_instance_adaptive_candidate_bytes = total_instance_bytes;
        transfer_tile_instance_adaptive_limit_bytes = adaptive_soft_limit_bytes;
        transfer_tile_instance_adaptive_rect_count =
            uint32_t(build_infos.size());
        return false;
      }

      size_t current_frame_instance_offset = 0;
      if (transfer_tile_instance_buffer_frame_id_ == frame_id_) {
        current_frame_instance_offset =
            xe::align(transfer_tile_instance_buffer_offset_, kAlignment);
      }
      size_t near_cap_threshold_by_percent =
          kTransferTileInstanceBufferMaxBytes *
          kTransferTileInstanceNearCapUsagePercent / 100;
      size_t near_cap_threshold_by_reserve = 0;
      if (kTransferTileInstanceBufferMaxBytes >
          kTransferTileInstanceNearCapReserveBytes) {
        near_cap_threshold_by_reserve =
            kTransferTileInstanceBufferMaxBytes -
            kTransferTileInstanceNearCapReserveBytes;
      }
      size_t near_cap_threshold_bytes = std::min(near_cap_threshold_by_percent,
                                                 near_cap_threshold_by_reserve);
      size_t projected_instance_bytes = current_frame_instance_offset;
      if (projected_instance_bytes >
              kTransferTileInstanceBufferMaxBytes - total_instance_bytes ||
          projected_instance_bytes + total_instance_bytes >
              near_cap_threshold_bytes) {
        transfer_tile_instance_predictive_cutoff_hit = true;
        transfer_tile_instance_predictive_used_bytes =
            current_frame_instance_offset;
        transfer_tile_instance_predictive_candidate_bytes =
            total_instance_bytes;
        transfer_tile_instance_predictive_threshold_bytes =
            near_cap_threshold_bytes;
        return false;
      }

      MTL::Buffer* buffer = nullptr;
      size_t buffer_base_offset = 0;
      if (!allocate_instance_buffer(total_instance_bytes, buffer,
                                    buffer_base_offset)) {
        return false;
      }

      uint8_t* base_ptr =
          reinterpret_cast<uint8_t*>(buffer->contents()) + buffer_base_offset;
      uint32_t source_pitch_tiles = constants.address.source_pitch;
      uint32_t source_tile_width_pixels =
          tile_width_samples >>
          ((constants.source_is_64bpp != 0u) +
           (constants.source_msaa_samples >= 4u ? 1u : 0u));
      uint32_t source_tile_height_pixels =
          tile_height_samples >>
          (constants.source_msaa_samples >= 2u ? 1u : 0u);
      uint32_t host_tile_width_pixels =
          tile_width_samples >>
          (constants.host_depth_source_msaa_samples >= 4u ? 1u : 0u);
      uint32_t host_tile_height_pixels =
          tile_height_samples >>
          (constants.host_depth_source_msaa_samples >= 2u ? 1u : 0u);

      for (const auto& info : build_infos) {
        uint8_t* batch_ptr = base_ptr + info.batch.buffer_offset;
        auto* instances = reinterpret_cast<TransferTileInstance*>(batch_ptr);
        uint32_t instance_index = 0;
        for (uint32_t tile_y = info.tile_y_start; tile_y <= info.tile_y_end;
             ++tile_y) {
          uint32_t row_base = tile_y * constants.address.dest_pitch;
          float origin_y = float(tile_y * constants.dest_tile_height_pixels);
          for (uint32_t tile_x = info.tile_x_start; tile_x <= info.tile_x_end;
               ++tile_x) {
            TransferTileInstance& instance = instances[instance_index++];
            instance.origin_x =
                float(tile_x * constants.dest_tile_width_pixels);
            instance.origin_y = origin_y;
            instance.tile_index = row_base + tile_x;
            uint32_t dest_tile_index = instance.tile_index;
            uint32_t source_tile_index =
                uint32_t(int32_t(dest_tile_index) +
                         constants.address.source_to_dest) &
                (xenos::kEdramTileCount - 1u);
            uint32_t source_tile_index_y = 0u;
            uint32_t source_tile_index_x = 0u;
            if (source_pitch_tiles) {
              source_tile_index_y = source_tile_index / source_pitch_tiles;
              source_tile_index_x =
                  source_tile_index - source_tile_index_y * source_pitch_tiles;
            }
            instance.source_base_x =
                source_tile_index_x * source_tile_width_pixels;
            instance.source_base_y =
                source_tile_index_y * source_tile_height_pixels;
            instance.host_base_x = 0;
            instance.host_base_y = 0;
            if (uses_host_depth && !host_depth_is_copy) {
              uint32_t host_pitch_tiles =
                  constants.host_depth_address.source_pitch;
              uint32_t host_tile_index =
                  uint32_t(int32_t(dest_tile_index) +
                           constants.host_depth_address.source_to_dest) &
                  (xenos::kEdramTileCount - 1u);
              uint32_t host_tile_index_y = 0u;
              uint32_t host_tile_index_x = 0u;
              if (host_pitch_tiles) {
                host_tile_index_y = host_tile_index / host_pitch_tiles;
                host_tile_index_x =
                    host_tile_index - host_tile_index_y * host_pitch_tiles;
              }
              instance.host_base_x = host_tile_index_x * host_tile_width_pixels;
              instance.host_base_y =
                  host_tile_index_y * host_tile_height_pixels;
            }
          }
        }
        TransferTileBatch batch = info.batch;
        batch.buffer = buffer;
        batch.buffer_offset = buffer_base_offset + info.batch.buffer_offset;
        out_batches.push_back(batch);
      }

      if (out_batches.empty()) {
        return false;
      }
      return true;
    };

    auto build_rect_instance_stream =
        [&](const Transfer::Rectangle* rectangles, uint32_t rectangle_count,
            MTL::Buffer*& out_buffer, size_t& out_buffer_offset,
            uint32_t& out_instance_count) -> bool {
      out_buffer = nullptr;
      out_buffer_offset = 0;
      out_instance_count = 0;
      if (!rectangles || !rectangle_count) {
        return false;
      }
      size_t buffer_size =
          size_t(rectangle_count) * sizeof(TransferRectInstance);
      if (!buffer_size) {
        return false;
      }
      MTL::Buffer* buffer = nullptr;
      size_t buffer_offset = 0;
      if (!allocate_instance_buffer(buffer_size, buffer, buffer_offset)) {
        return false;
      }
      if (!buffer) {
        return false;
      }
      auto* instances = reinterpret_cast<TransferRectInstance*>(
          reinterpret_cast<uint8_t*>(buffer->contents()) + buffer_offset);
      if (!instances) {
        return false;
      }
      uint32_t instance_count = 0;
      for (uint32_t rect_index = 0; rect_index < rectangle_count;
           ++rect_index) {
        uint32_t scaled_x = 0;
        uint32_t scaled_y = 0;
        uint32_t scaled_width = 0;
        uint32_t scaled_height = 0;
        if (!get_scaled_rect(rectangles[rect_index], scaled_x, scaled_y,
                             scaled_width, scaled_height)) {
          continue;
        }
        if (!scaled_width || !scaled_height) {
          continue;
        }
        TransferRectInstance& instance = instances[instance_count++];
        instance.origin_x = float(scaled_x);
        instance.origin_y = float(scaled_y);
        instance.size_x = float(scaled_width);
        instance.size_y = float(scaled_height);
      }
      if (!instance_count) {
        return false;
      }
      out_buffer = buffer;
      out_buffer_offset = buffer_offset;
      out_instance_count = instance_count;
      return true;
    };

    std::vector<Transfer> filtered_transfers;
    bool used_blit = false;
    MTL::BlitCommandEncoder* blit_encoder = nullptr;
    auto ensure_blit_encoder = [&]() -> MTL::BlitCommandEncoder* {
      if (!blit_encoder) {
        blit_encoder = cmd->blitCommandEncoder();
      }
      return blit_encoder;
    };

    // Fast path: when source/dest share compatible EDRAM layout and format,
    // use a blit instead of shader-based transfers.
    if (!transfers.empty()) {
      auto try_blit_transfer = [&](const Transfer& transfer) -> bool {
        auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
        if (!source_rt || transfer.host_depth_source) {
          return false;
        }

        RenderTargetKey source_key = source_rt->key();
        if (dest_is_depth != source_key.is_depth) {
          return false;
        }
        if (source_key.resource_format != dest_key.resource_format ||
            source_key.msaa_samples != dest_key.msaa_samples ||
            source_key.pitch_tiles_at_32bpp != dest_key.pitch_tiles_at_32bpp) {
          return false;
        }

        bool base_tiles_match = source_key.base_tiles == dest_key.base_tiles;
        if (dest_is_depth && !base_tiles_match) {
          return false;
        }

        MTL::Texture* source_texture = dest_is_depth
                                           ? source_rt->texture()
                                           : source_rt->transfer_texture();
        if (!source_texture) {
          return false;
        }
        if (!dest_is_depth) {
          MTL::PixelFormat expected_format =
              GetColorOwnershipTransferPixelFormat(source_key.GetColorFormat(),
                                                   nullptr);
          assert_true(source_texture->pixelFormat() == expected_format,
                      "Transfer source must use ownership pixel format");
        }
        if (source_texture->pixelFormat() != dest_texture->pixelFormat() ||
            source_texture->sampleCount() != dest_texture->sampleCount() ||
            source_texture->sampleCount() != 1 ||
            source_texture->width() != dest_width ||
            source_texture->height() != dest_height) {
          return false;
        }

        Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
        uint32_t rectangle_count = transfer.GetRectangles(
            dest_key.base_tiles, dest_key.pitch_tiles_at_32bpp,
            dest_key.msaa_samples, IsKey64bpp(dest_key), rectangles,
            resolve_clear_rectangle);
        if (!rectangle_count) {
          return false;
        }

        MTL::BlitCommandEncoder* blit = ensure_blit_encoder();
        if (!blit) {
          return false;
        }

        if (base_tiles_match || dest_is_depth) {
          for (uint32_t rect_index = 0; rect_index < rectangle_count;
               ++rect_index) {
            uint32_t scaled_x = 0;
            uint32_t scaled_y = 0;
            uint32_t scaled_width = 0;
            uint32_t scaled_height = 0;
            if (!get_scaled_rect(rectangles[rect_index], scaled_x, scaled_y,
                                 scaled_width, scaled_height)) {
              continue;
            }
            MTL::Origin origin = MTL::Origin::Make(scaled_x, scaled_y, 0);
            MTL::Size size = MTL::Size::Make(scaled_width, scaled_height, 1);
            blit->copyFromTexture(source_texture, 0, 0, origin, size,
                                  dest_texture, 0, 0, origin);
          }
        } else {
          // Base-tile offset blit (color only, non-MSAA, tile-aligned).
          uint32_t pitch_tiles = dest_key.pitch_tiles_at_32bpp;
          if (!pitch_tiles) {
            return false;
          }
          uint32_t tile_width_pixels =
              tile_width_samples >>
              ((IsKey64bpp(dest_key) ? 1u : 0u) +
               uint32_t(dest_key.msaa_samples >= xenos::MsaaSamples::k4X));
          uint32_t tile_height_pixels =
              tile_height_samples >>
              uint32_t(dest_key.msaa_samples >= xenos::MsaaSamples::k2X);
          if (!tile_width_pixels || !tile_height_pixels) {
            return false;
          }
          uint32_t delta_tiles = (dest_key.base_tiles - source_key.base_tiles) &
                                 (xenos::kEdramTileCount - 1u);
          uint32_t delta_rows = delta_tiles / pitch_tiles;
          uint32_t delta_x = delta_tiles % pitch_tiles;
          uint32_t total_rows =
              (xenos::kEdramTileCount + pitch_tiles - 1u) / pitch_tiles;

          struct ScaledRect {
            uint32_t x;
            uint32_t y;
            uint32_t width;
            uint32_t height;
          };
          std::vector<ScaledRect> scaled_rects;
          scaled_rects.reserve(rectangle_count);
          for (uint32_t rect_index = 0; rect_index < rectangle_count;
               ++rect_index) {
            uint32_t scaled_x = 0;
            uint32_t scaled_y = 0;
            uint32_t scaled_width = 0;
            uint32_t scaled_height = 0;
            if (!get_scaled_rect(rectangles[rect_index], scaled_x, scaled_y,
                                 scaled_width, scaled_height)) {
              continue;
            }
            if ((scaled_x % tile_width_pixels) ||
                (scaled_y % tile_height_pixels) ||
                (scaled_width % tile_width_pixels) ||
                (scaled_height % tile_height_pixels)) {
              return false;
            }
            if (!scaled_width || !scaled_height) {
              continue;
            }
            scaled_rects.push_back(
                {scaled_x, scaled_y, scaled_width, scaled_height});
          }
          if (scaled_rects.empty()) {
            return false;
          }

          for (const auto& rect : scaled_rects) {
            uint32_t tile_x = rect.x / tile_width_pixels;
            uint32_t tile_y = rect.y / tile_height_pixels;
            uint32_t tiles_w = rect.width / tile_width_pixels;
            uint32_t tiles_h = rect.height / tile_height_pixels;
            if (!tiles_w || !tiles_h) {
              continue;
            }

            uint32_t source_tile_x_base = tile_x + delta_x;
            uint32_t source_tile_x = source_tile_x_base % pitch_tiles;
            uint32_t source_tile_y =
                tile_y + delta_rows + (source_tile_x_base / pitch_tiles);
            if (source_tile_y >= total_rows) {
              source_tile_y %= total_rows;
            }

            uint32_t rows_before_wrap =
                std::min(tiles_h, total_rows - source_tile_y);
            uint32_t rows_after_wrap = tiles_h - rows_before_wrap;

            uint32_t tiles_before_wrap_x =
                (source_tile_x + tiles_w <= pitch_tiles)
                    ? tiles_w
                    : (pitch_tiles - source_tile_x);
            uint32_t tiles_after_wrap_x = tiles_w - tiles_before_wrap_x;

            for (uint32_t wrap_y = 0; wrap_y <= (rows_after_wrap ? 1u : 0u);
                 ++wrap_y) {
              uint32_t y_offset_tiles = wrap_y ? rows_before_wrap : 0u;
              uint32_t rows = wrap_y ? rows_after_wrap : rows_before_wrap;
              if (!rows) {
                continue;
              }
              uint32_t dest_y_pixels =
                  rect.y + y_offset_tiles * tile_height_pixels;
              uint32_t source_y_tiles = wrap_y ? 0u : source_tile_y;
              uint32_t source_y_pixels = source_y_tiles * tile_height_pixels;
              uint32_t height_pixels = rows * tile_height_pixels;

              // X segment 0.
              if (tiles_before_wrap_x) {
                uint32_t dest_x_pixels = rect.x;
                uint32_t source_x_pixels = source_tile_x * tile_width_pixels;
                uint32_t width_pixels = tiles_before_wrap_x * tile_width_pixels;
                MTL::Origin src_origin =
                    MTL::Origin::Make(source_x_pixels, source_y_pixels, 0);
                MTL::Origin dst_origin =
                    MTL::Origin::Make(dest_x_pixels, dest_y_pixels, 0);
                MTL::Size size =
                    MTL::Size::Make(width_pixels, height_pixels, 1);
                blit->copyFromTexture(source_texture, 0, 0, src_origin, size,
                                      dest_texture, 0, 0, dst_origin);
              }

              // X segment 1 (wrap).
              if (tiles_after_wrap_x) {
                uint32_t dest_x_pixels =
                    rect.x + tiles_before_wrap_x * tile_width_pixels;
                uint32_t source_x_pixels = 0;
                uint32_t width_pixels = tiles_after_wrap_x * tile_width_pixels;
                MTL::Origin src_origin =
                    MTL::Origin::Make(source_x_pixels, source_y_pixels, 0);
                MTL::Origin dst_origin =
                    MTL::Origin::Make(dest_x_pixels, dest_y_pixels, 0);
                MTL::Size size =
                    MTL::Size::Make(width_pixels, height_pixels, 1);
                blit->copyFromTexture(source_texture, 0, 0, src_origin, size,
                                      dest_texture, 0, 0, dst_origin);
              }
            }
          }
        }

        used_blit = true;
        any_transfers_done = true;
        return true;
      };

      for (const Transfer& transfer : transfers) {
        if (!try_blit_transfer(transfer)) {
          filtered_transfers.push_back(transfer);
        }
      }
    }

    if (blit_encoder) {
      blit_encoder->endEncoding();
    }

    const bool disable_transfer_shaders = false;
    const std::vector<Transfer>& transfers_for_shaders =
        used_blit ? filtered_transfers : transfers;

    auto is_full_target_rectangle =
        [&](const Transfer::Rectangle& rect) -> bool {
      uint32_t scaled_x = 0;
      uint32_t scaled_y = 0;
      uint32_t scaled_width = 0;
      uint32_t scaled_height = 0;
      if (!get_scaled_rect(rect, scaled_x, scaled_y, scaled_width,
                           scaled_height)) {
        return false;
      }
      return !scaled_x && !scaled_y && scaled_width == dest_width &&
             scaled_height == dest_height;
    };

    auto transfers_fully_overwrite_target = [&]() -> bool {
      if (transfers_for_shaders.empty()) {
        return false;
      }
      for (const Transfer& transfer : transfers_for_shaders) {
        Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
        uint32_t rectangle_count = transfer.GetRectangles(
            dest_key.base_tiles, dest_key.GetPitchTiles(),
            dest_key.msaa_samples, IsKey64bpp(dest_key), rectangles,
            resolve_clear_rectangle);
        if (rectangle_count != 1 || !is_full_target_rectangle(rectangles[0])) {
          return false;
        }
      }
      return true;
    };

    bool resolve_clear_fully_overwrites_target = false;
    if (resolve_clear_needed && resolve_clear_rectangle) {
      resolve_clear_fully_overwrites_target =
          is_full_target_rectangle(*resolve_clear_rectangle);
    }

    bool resolve_clear_via_load_action = false;
    MTL::ClearColor resolve_clear_color = MTL::ClearColor(0.0, 0.0, 0.0, 0.0);
    double resolve_clear_depth = 1.0;
    uint32_t resolve_clear_stencil = 0;
    if (resolve_clear_needed && resolve_clear_fully_overwrites_target) {
      const uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_is_depth) {
        uint32_t depth_guest_clear_value =
            (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        switch (dest_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            resolve_clear_depth = xenos::UNorm24To32(depth_guest_clear_value);
            resolve_clear_via_load_action = true;
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            resolve_clear_depth =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            resolve_clear_via_load_action = true;
            break;
        }
        resolve_clear_stencil = uint32_t(clear_value) & 0xFF;
      } else {
        TransferClearColorFloatConstants float_constants = {};
        bool clear_via_drawing = false;
        switch (dest_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            if (gamma_render_target_as_unorm16_) {
              for (uint32_t j = 0; j < 3; ++j) {
                float_constants.color[j] =
                    xenos::PWLGammaToLinear(float_constants.color[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::
              k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            for (uint32_t j = 0; j < 2; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            float_constants.color[1] = float(uint32_t(clear_value >> 32));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value) ||
                uint64_t(float_constants.color[1]) !=
                    uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }

        bool clear_is_uint = false;
        GetColorOwnershipTransferPixelFormat(dest_key.GetColorFormat(),
                                             &clear_is_uint);
        if (!clear_is_uint && !clear_via_drawing) {
          resolve_clear_color = MTL::ClearColor(
              float_constants.color[0], float_constants.color[1],
              float_constants.color[2], float_constants.color[3]);
          resolve_clear_via_load_action = true;
        }
      }
    }

    // Prefer DontCare on transfer-pass loads only when destination contents are
    // provably fully overwritten by this pass.
    bool transfer_pass_load_dontcare = false;
    if (resolve_clear_fully_overwrites_target &&
        !resolve_clear_via_load_action) {
      transfer_pass_load_dontcare = true;
    }
    if (!transfer_pass_load_dontcare && !resolve_clear_needed) {
      transfer_pass_load_dontcare = transfers_fully_overwrite_target();
    }
    MTL::LoadAction transfer_load_action = MTL::LoadActionLoad;
    if (resolve_clear_via_load_action) {
      transfer_load_action = MTL::LoadActionClear;
    } else if (transfer_pass_load_dontcare) {
      transfer_load_action = MTL::LoadActionDontCare;
    }

    MTL::RenderCommandEncoder* transfer_encoder = nullptr;
    auto ensure_transfer_encoder = [&]() -> MTL::RenderCommandEncoder* {
      if (transfer_encoder) {
        return transfer_encoder;
      }
      MTL::RenderPassDescriptor* rp =
          MTL::RenderPassDescriptor::renderPassDescriptor();
      if (dest_is_depth) {
        auto* da = rp->depthAttachment();
        da->setTexture(dest_texture);
        da->setLoadAction(transfer_load_action);
        da->setStoreAction(MTL::StoreActionStore);
        if (transfer_load_action == MTL::LoadActionClear) {
          da->setClearDepth(resolve_clear_depth);
        }
        if (dest_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
            dest_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
          auto* sa = rp->stencilAttachment();
          sa->setTexture(dest_texture);
          sa->setLoadAction(transfer_load_action);
          sa->setStoreAction(MTL::StoreActionStore);
          if (transfer_load_action == MTL::LoadActionClear) {
            sa->setClearStencil(resolve_clear_stencil);
          }
        }
      } else {
        auto* ca = rp->colorAttachments()->object(0);
        ca->setTexture(dest_texture);
        ca->setLoadAction(transfer_load_action);
        ca->setStoreAction(MTL::StoreActionStore);
        if (transfer_load_action == MTL::LoadActionClear) {
          ca->setClearColor(resolve_clear_color);
        }
      }
      transfer_encoder = cmd->renderCommandEncoder(rp);
      return transfer_encoder;
    };

    if (!transfers_for_shaders.empty() && disable_transfer_shaders) {
      static uint32_t transfer_shader_skip_log_count = 0;
      if (transfer_shader_skip_log_count < 8) {
        ++transfer_shader_skip_log_count;
        XELOGW(
            "MetalRenderTargetCache::PerformTransfersAndResolveClears: "
            "transfer shaders disabled; skipping {} transfers for RT {}",
            transfers_for_shaders.size(), i);
      }
    } else if (!transfers_for_shaders.empty()) {
      bool need_stencil_bit_draws = dest_is_depth;
      bool stencil_clear_needed = need_stencil_bit_draws;

      transfer_invocations_.clear();
      transfer_invocations_.reserve(transfers_for_shaders.size() *
                                    (need_stencil_bit_draws ? 2 : 1));

      for (const Transfer& transfer : transfers_for_shaders) {
        if (transfer.source) {
          auto* source = static_cast<MetalRenderTarget*>(transfer.source);
          source->SetTemporarySortIndex(UINT32_MAX);
        }
        if (transfer.host_depth_source) {
          auto* host_depth =
              static_cast<MetalRenderTarget*>(transfer.host_depth_source);
          host_depth->SetTemporarySortIndex(UINT32_MAX);
        }
      }

      uint32_t rt_sort_index = 0;
      auto ensure_sort_index = [&](MetalRenderTarget* rt) {
        if (rt && rt->temporary_sort_index() == UINT32_MAX) {
          rt->SetTemporarySortIndex(rt_sort_index++);
        }
      };

      for (uint32_t pass = 0; pass <= uint32_t(need_stencil_bit_draws);
           ++pass) {
        for (const Transfer& transfer : transfers_for_shaders) {
          if (!transfer.source) {
            continue;
          }
          auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
          auto* host_depth_rt =
              pass
                  ? nullptr
                  : static_cast<MetalRenderTarget*>(transfer.host_depth_source);
          ensure_sort_index(source_rt);
          ensure_sort_index(host_depth_rt);

          RenderTargetKey source_key = source_rt->key();
          TransferShaderKey shader_key = {};
          shader_key.source_msaa_samples = source_key.msaa_samples;
          shader_key.dest_msaa_samples = dest_key.msaa_samples;
          shader_key.source_resource_format = source_key.resource_format;
          shader_key.dest_resource_format = dest_key.resource_format;

          if (pass) {
            shader_key.mode = source_key.is_depth
                                  ? TransferMode::kDepthToStencilBit
                                  : TransferMode::kColorToStencilBit;
            shader_key.host_depth_source_msaa_samples = xenos::MsaaSamples::k1X;
            shader_key.host_depth_source_is_copy = 0;
          } else {
            if (dest_is_depth) {
              if (host_depth_rt) {
                bool host_depth_is_copy = host_depth_rt == dest_metal_rt;
                shader_key.mode = source_key.is_depth
                                      ? TransferMode::kDepthAndHostDepthToDepth
                                      : TransferMode::kColorAndHostDepthToDepth;
                shader_key.host_depth_source_is_copy =
                    host_depth_is_copy ? 1 : 0;
                shader_key.host_depth_source_msaa_samples =
                    host_depth_is_copy ? xenos::MsaaSamples::k1X
                                       : host_depth_rt->key().msaa_samples;
              } else {
                shader_key.mode = source_key.is_depth
                                      ? TransferMode::kDepthToDepth
                                      : TransferMode::kColorToDepth;
                shader_key.host_depth_source_msaa_samples =
                    xenos::MsaaSamples::k1X;
                shader_key.host_depth_source_is_copy = 0;
              }
            } else {
              shader_key.mode = source_key.is_depth
                                    ? TransferMode::kDepthToColor
                                    : TransferMode::kColorToColor;
              shader_key.host_depth_source_msaa_samples =
                  xenos::MsaaSamples::k1X;
              shader_key.host_depth_source_is_copy = 0;
            }
          }

          const TransferModeInfo& mode_info =
              kTransferModeInfos[size_t(shader_key.mode)];
          bool transfer_use_sample_id = transfer_use_sample_id_default;
          if (transfer_use_sample_id) {
            bool source_is_multisample =
                source_key.msaa_samples != xenos::MsaaSamples::k1X;
            bool host_depth_is_multisample =
                mode_info.uses_host_depth &&
                shader_key.host_depth_source_msaa_samples !=
                    xenos::MsaaSamples::k1X &&
                !shader_key.host_depth_source_is_copy;
            if (!source_is_multisample && !host_depth_is_multisample) {
              transfer_use_sample_id = false;
            }
          }
          shader_key.dest_sample_id_from_sample =
              transfer_use_sample_id ? 1u : 0u;

          transfer_invocations_.emplace_back(transfer, shader_key);
          if (pass) {
            transfer_invocations_.back().transfer.host_depth_source = nullptr;
          }
        }
      }

      std::sort(transfer_invocations_.begin(), transfer_invocations_.end());

      if (stencil_clear_needed) {
        MTL::RenderPipelineState* clear_pipeline =
            GetOrCreateTransferClearPipeline(dest_pixel_format, false, true,
                                             dest_sample_count);
        MTL::DepthStencilState* stencil_clear_state =
            GetTransferStencilClearState();
        if (clear_pipeline && stencil_clear_state) {
          MTL::RenderCommandEncoder* encoder = ensure_transfer_encoder();
          if (encoder) {
            TransferClearDepthConstants constants = {};
            constants.depth = 0.0f;
            encoder->setRenderPipelineState(clear_pipeline);
            encoder->setDepthStencilState(stencil_clear_state);
            encoder->setStencilReferenceValue(0);
            encoder->setFragmentBytes(&constants, sizeof(constants), 0);
            for (const Transfer& transfer : transfers_for_shaders) {
              Transfer::Rectangle
                  rectangles[Transfer::kMaxRectanglesWithCutout];
              uint32_t rectangle_count = transfer.GetRectangles(
                  dest_key.base_tiles, dest_key.GetPitchTiles(),
                  dest_key.msaa_samples, IsKey64bpp(dest_key), rectangles,
                  resolve_clear_rectangle);
              for (uint32_t rect_index = 0; rect_index < rectangle_count;
                   ++rect_index) {
                if (!set_rect_viewport(encoder, rectangles[rect_index])) {
                  continue;
                }
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                        NS::UInteger(0), NS::UInteger(3));
              }
            }
          }
        }
      }
      MTL::RenderCommandEncoder* encoder = ensure_transfer_encoder();
      if (encoder) {
        bool transfer_viewport_full_set = false;
        MTL::ScissorRect last_transfer_scissor = {};
        bool last_transfer_scissor_valid = false;
        MTL::RenderPipelineState* last_transfer_pipeline = nullptr;
        MTL::DepthStencilState* last_transfer_depth_state = nullptr;
        MTL::Buffer* last_transfer_fragment_buffer_1 = nullptr;
        std::array<MTL::Texture*, 3> last_transfer_fragment_textures = {
            nullptr, nullptr, nullptr};
        bool last_transfer_stencil_reference_valid = false;
        uint32_t last_transfer_stencil_reference = 0;
        bool transfer_constants_valid = false;
        TransferShaderConstants last_transfer_constants = {};
        enum class TransferVertexSlot1Binding { kNone, kBuffer, kBytes };
        TransferVertexSlot1Binding last_transfer_vertex_slot_1_binding =
            TransferVertexSlot1Binding::kNone;
        MTL::Buffer* last_transfer_vertex_buffer_1 = nullptr;
        size_t last_transfer_vertex_buffer_1_offset = 0;
        bool last_transfer_vertex_bytes_1_valid = false;
        TransferRectInstance last_transfer_vertex_bytes_1 = {};
        auto bind_transfer_pipeline = [&](MTL::RenderPipelineState* pipeline) {
          if (last_transfer_pipeline != pipeline) {
            encoder->setRenderPipelineState(pipeline);
            last_transfer_pipeline = pipeline;
          }
        };
        auto bind_transfer_depth_state = [&](MTL::DepthStencilState* state) {
          if (last_transfer_depth_state != state) {
            encoder->setDepthStencilState(state);
            last_transfer_depth_state = state;
          }
        };
        auto bind_transfer_fragment_texture = [&](uint32_t index,
                                                  MTL::Texture* texture) {
          if (index >= last_transfer_fragment_textures.size()) {
            return;
          }
          if (last_transfer_fragment_textures[index] != texture) {
            encoder->setFragmentTexture(texture, index);
            last_transfer_fragment_textures[index] = texture;
          }
        };
        auto bind_transfer_fragment_buffer_1 = [&](MTL::Buffer* buffer) {
          if (last_transfer_fragment_buffer_1 != buffer) {
            encoder->setFragmentBuffer(buffer, 0, 1);
            last_transfer_fragment_buffer_1 = buffer;
          }
        };
        auto bind_transfer_stencil_reference = [&](uint32_t reference) {
          if (!last_transfer_stencil_reference_valid ||
              last_transfer_stencil_reference != reference) {
            encoder->setStencilReferenceValue(reference);
            last_transfer_stencil_reference = reference;
            last_transfer_stencil_reference_valid = true;
          }
        };
        auto bind_transfer_constants =
            [&](const TransferShaderConstants& constants) {
              if (!transfer_constants_valid ||
                  std::memcmp(&last_transfer_constants, &constants,
                              sizeof(constants)) != 0) {
                encoder->setVertexBytes(&constants, sizeof(constants), 0);
                encoder->setFragmentBytes(&constants, sizeof(constants), 0);
                last_transfer_constants = constants;
                transfer_constants_valid = true;
              }
            };
        auto bind_transfer_scissor = [&](const MTL::ScissorRect& scissor) {
          if (!last_transfer_scissor_valid ||
              last_transfer_scissor.x != scissor.x ||
              last_transfer_scissor.y != scissor.y ||
              last_transfer_scissor.width != scissor.width ||
              last_transfer_scissor.height != scissor.height) {
            encoder->setScissorRect(scissor);
            last_transfer_scissor = scissor;
            last_transfer_scissor_valid = true;
          }
        };
        auto bind_transfer_vertex_buffer_1 = [&](MTL::Buffer* buffer,
                                                 size_t offset) {
          if (last_transfer_vertex_slot_1_binding !=
                  TransferVertexSlot1Binding::kBuffer ||
              last_transfer_vertex_buffer_1 != buffer ||
              last_transfer_vertex_buffer_1_offset != offset) {
            encoder->setVertexBuffer(buffer, offset, 1);
            last_transfer_vertex_slot_1_binding =
                TransferVertexSlot1Binding::kBuffer;
            last_transfer_vertex_buffer_1 = buffer;
            last_transfer_vertex_buffer_1_offset = offset;
            last_transfer_vertex_bytes_1_valid = false;
          }
        };
        auto bind_transfer_vertex_bytes_1 =
            [&](const TransferRectInstance& rect_instance) {
              if (last_transfer_vertex_slot_1_binding !=
                      TransferVertexSlot1Binding::kBytes ||
                  !last_transfer_vertex_bytes_1_valid ||
                  std::memcmp(&last_transfer_vertex_bytes_1, &rect_instance,
                              sizeof(rect_instance)) != 0) {
                encoder->setVertexBytes(&rect_instance, sizeof(rect_instance),
                                        1);
                last_transfer_vertex_slot_1_binding =
                    TransferVertexSlot1Binding::kBytes;
                last_transfer_vertex_buffer_1 = nullptr;
                last_transfer_vertex_buffer_1_offset = 0;
                last_transfer_vertex_bytes_1 = rect_instance;
                last_transfer_vertex_bytes_1_valid = true;
              }
            };
        auto bind_transfer_vertex_bytes_1_span =
            [&](const TransferRectInstance* rect_instances,
                uint32_t rect_instance_count) {
              if (!rect_instances || !rect_instance_count) {
                return;
              }
              encoder->setVertexBytes(
                  rect_instances,
                  size_t(rect_instance_count) * sizeof(TransferRectInstance),
                  1);
              last_transfer_vertex_slot_1_binding =
                  TransferVertexSlot1Binding::kBytes;
              last_transfer_vertex_buffer_1 = nullptr;
              last_transfer_vertex_buffer_1_offset = 0;
              last_transfer_vertex_bytes_1_valid = false;
            };
        auto set_full_transfer_viewport_scissor = [&]() {
          if (!transfer_viewport_full_set) {
            MTL::Viewport vp;
            vp.originX = 0.0;
            vp.originY = 0.0;
            vp.width = double(dest_width);
            vp.height = double(dest_height);
            vp.znear = 0.0;
            vp.zfar = 1.0;
            encoder->setViewport(vp);
            transfer_viewport_full_set = true;
          }
          MTL::ScissorRect scissor;
          scissor.x = 0;
          scissor.y = 0;
          scissor.width = dest_width;
          scissor.height = dest_height;
          bind_transfer_scissor(scissor);
        };

        std::vector<Transfer::Rectangle> merged_transfer_rectangles;
        for (size_t invocation_index = 0;
             invocation_index < transfer_invocations_.size();) {
          const auto& invocation = transfer_invocations_[invocation_index];
          size_t merged_invocation_end = invocation_index + 1;
          while (merged_invocation_end < transfer_invocations_.size() &&
                 invocation.CanBeMergedIntoOneDraw(
                     transfer_invocations_[merged_invocation_end])) {
            ++merged_invocation_end;
          }

          merged_transfer_rectangles.clear();
          merged_transfer_rectangles.reserve(
              (merged_invocation_end - invocation_index) *
              Transfer::kMaxRectanglesWithCutout);
          for (size_t merged_index = invocation_index;
               merged_index < merged_invocation_end; ++merged_index) {
            Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
            uint32_t rectangle_count =
                transfer_invocations_[merged_index].transfer.GetRectangles(
                    dest_key.base_tiles, dest_key.GetPitchTiles(),
                    dest_key.msaa_samples, IsKey64bpp(dest_key), rectangles,
                    resolve_clear_rectangle);
            for (uint32_t rect_index = 0; rect_index < rectangle_count;
                 ++rect_index) {
              merged_transfer_rectangles.push_back(rectangles[rect_index]);
            }
          }
          invocation_index = merged_invocation_end;
          if (merged_transfer_rectangles.empty()) {
            continue;
          }

          const Transfer& transfer = invocation.transfer;
          const TransferShaderKey& shader_key = invocation.shader_key;
          const TransferModeInfo& mode_info =
              kTransferModeInfos[size_t(shader_key.mode)];
          bool is_stencil_bit = mode_info.output == TransferOutput::kStencilBit;
          bool needs_source_stencil =
              !mode_info.source_is_color &&
              (mode_info.output == TransferOutput::kColor ||
               mode_info.output == TransferOutput::kStencilBit);

          auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
          if (!source_rt) {
            continue;
          }

          RenderTargetKey source_key = source_rt->key();
          bool source_is_uint = false;
          MTL::PixelFormat source_transfer_format = MTL::PixelFormatInvalid;
          if (mode_info.source_is_color) {
            source_transfer_format = GetColorOwnershipTransferPixelFormat(
                source_key.GetColorFormat(), &source_is_uint);
          }

          if (is_stencil_bit) {
            // Depth/stencil state set per-bit below.
          } else if (dest_is_depth) {
            bind_transfer_depth_state(GetTransferDepthStencilState(true));
          } else {
            MTL::DepthStencilState* no_depth_state =
                GetTransferNoDepthStencilState();
            if (!no_depth_state) {
              continue;
            }
            bind_transfer_depth_state(no_depth_state);
          }

          // Bind source textures.
          if (mode_info.source_is_color) {
            MTL::Texture* source_texture = source_rt->transfer_texture();
            if (!source_texture) {
              continue;
            }
            assert_true(source_texture->pixelFormat() == source_transfer_format,
                        "Transfer source must use ownership pixel format");
            bind_transfer_fragment_texture(0, source_texture);
          } else {
            MTL::Texture* depth_texture = source_rt->texture();
            if (!depth_texture) {
              continue;
            }
            bind_transfer_fragment_texture(0, depth_texture);
            if (needs_source_stencil) {
              MTL::Texture* stencil_texture = GetStencilTextureView(source_rt);
              if (!stencil_texture) {
                continue;
              }
              bind_transfer_fragment_texture(1, stencil_texture);
            }
          }

          // Bind host depth source if needed.
          if (mode_info.uses_host_depth) {
            uint32_t host_depth_index = mode_info.source_is_color ? 1 : 2;
            if (shader_key.host_depth_source_is_copy) {
              if (edram_buffer_) {
                bind_transfer_fragment_buffer_1(edram_buffer_);
              } else {
                MTL::Buffer* dummy = GetTransferDummyBuffer();
                if (dummy) {
                  bind_transfer_fragment_buffer_1(dummy);
                }
              }
              MTL::Texture* dummy_host_depth = GetTransferDummyDepthTexture(1);
              if (!dummy_host_depth) {
                continue;
              }
              bind_transfer_fragment_texture(host_depth_index,
                                             dummy_host_depth);
            } else {
              MTL::Buffer* dummy = GetTransferDummyBuffer();
              if (!dummy) {
                continue;
              }
              bind_transfer_fragment_buffer_1(dummy);
              auto* host_depth_rt =
                  static_cast<MetalRenderTarget*>(transfer.host_depth_source);
              MTL::Texture* host_depth_texture =
                  host_depth_rt ? host_depth_rt->texture() : nullptr;
              if (!host_depth_texture) {
                continue;
              }
              bind_transfer_fragment_texture(host_depth_index,
                                             host_depth_texture);
            }
          }

          TransferShaderConstants constants = {};
          constants.address.dest_pitch = dest_key.GetPitchTiles();
          constants.address.source_pitch = source_key.GetPitchTiles();
          constants.address.source_to_dest =
              int32_t(dest_key.base_tiles) - int32_t(source_key.base_tiles);
          constants.host_depth_address = {};
          if (mode_info.uses_host_depth &&
              !shader_key.host_depth_source_is_copy) {
            auto* host_depth_rt =
                static_cast<MetalRenderTarget*>(transfer.host_depth_source);
            if (host_depth_rt) {
              RenderTargetKey host_depth_key = host_depth_rt->key();
              constants.host_depth_address.dest_pitch =
                  dest_key.GetPitchTiles();
              constants.host_depth_address.source_pitch =
                  host_depth_key.GetPitchTiles();
              constants.host_depth_address.source_to_dest =
                  int32_t(dest_key.base_tiles) -
                  int32_t(host_depth_key.base_tiles);
            }
          }
          constants.source_format = source_key.resource_format;
          constants.dest_format = dest_key.resource_format;
          constants.source_is_depth = source_key.is_depth ? 1 : 0;
          constants.dest_is_depth = dest_key.is_depth ? 1 : 0;
          constants.source_is_uint = source_is_uint ? 1 : 0;
          constants.dest_is_uint = dest_is_uint ? 1 : 0;
          // Don't treat gamma-as-unorm16 as 64bpp for source coordinate math.
          // It's 64bpp in storage, but represents a single pixel, not two
          // packed 32bpp halves.
          constants.source_is_64bpp = source_key.Is64bpp() ? 1 : 0;
          constants.dest_is_64bpp = IsKey64bpp(dest_key) ? 1 : 0;
          constants.source_msaa_samples =
              MsaaSamplesToCount(source_key.msaa_samples);
          constants.dest_msaa_samples =
              MsaaSamplesToCount(dest_key.msaa_samples);
          constants.host_depth_source_msaa_samples =
              MsaaSamplesToCount(shader_key.host_depth_source_msaa_samples);
          constants.host_depth_source_is_copy =
              shader_key.host_depth_source_is_copy ? 1 : 0;
          constants.depth_round = depth_round;
          constants.msaa_2x_supported = msaa_2x_supported_ ? 1 : 0;
          constants.tile_width_samples = tile_width_samples;
          constants.tile_height_samples = tile_height_samples;
          uint32_t dest_tile_width_pixels =
              tile_width_samples >>
              ((constants.dest_is_64bpp != 0u) +
               (constants.dest_msaa_samples >= 4u ? 1u : 0u));
          uint32_t dest_tile_height_pixels =
              tile_height_samples >>
              (constants.dest_msaa_samples >= 2u ? 1u : 0u);
          constants.dest_tile_width_pixels = dest_tile_width_pixels;
          constants.dest_tile_height_pixels = dest_tile_height_pixels;
          constants.dest_tile_width_pixels_inv =
              dest_tile_width_pixels ? (1.0f / float(dest_tile_width_pixels))
                                     : 0.0f;
          constants.dest_tile_height_pixels_inv =
              dest_tile_height_pixels ? (1.0f / float(dest_tile_height_pixels))
                                      : 0.0f;
          uint32_t source_pitch_tiles = source_key.GetPitchTiles();
          constants.source_pitch_tiles_inv =
              source_pitch_tiles ? (1.0f / float(source_pitch_tiles)) : 0.0f;
          constants.host_depth_source_pitch_tiles_inv = 0.0f;
          if (mode_info.uses_host_depth &&
              !shader_key.host_depth_source_is_copy) {
            auto* host_depth_rt =
                static_cast<MetalRenderTarget*>(transfer.host_depth_source);
            if (host_depth_rt) {
              uint32_t host_pitch_tiles = host_depth_rt->key().GetPitchTiles();
              constants.host_depth_source_pitch_tiles_inv =
                  host_pitch_tiles ? (1.0f / float(host_pitch_tiles)) : 0.0f;
            }
          }
          constants.dest_pixel_to_ndc_x =
              dest_width ? (2.0f / float(dest_width)) : 0.0f;
          constants.dest_pixel_to_ndc_y =
              dest_height ? (2.0f / float(dest_height)) : 0.0f;
          constants.dest_sample_id = 0;

          const uint32_t rectangle_count =
              uint32_t(merged_transfer_rectangles.size());

          std::vector<TransferTileBatch> tile_batches;
          MTL::Buffer* rect_instance_buffer = nullptr;
          size_t rect_instance_buffer_offset = 0;
          uint32_t rect_instance_count = 0;
          std::vector<TransferRectInstance> rect_instance_fallback;
          bool use_tile_instancing = false;
          if (::cvars::metal_transfer_tile_instancing) {
            use_tile_instancing = build_tile_batches(
                merged_transfer_rectangles.data(), rectangle_count, constants,
                mode_info.uses_host_depth,
                shader_key.host_depth_source_is_copy != 0, tile_batches);
            if (!use_tile_instancing &&
                transfer_tile_instance_predictive_cutoff_hit) {
              static uint64_t last_tile_predictive_log_frame = 0;
              if (last_tile_predictive_log_frame != frame_id_) {
                XELOGI(
                    "Metal transfer tile instancing fallback: predicted "
                    "frame usage {} KiB + {} KiB near cap {} KiB",
                    transfer_tile_instance_predictive_used_bytes >> 10,
                    transfer_tile_instance_predictive_candidate_bytes >> 10,
                    transfer_tile_instance_predictive_threshold_bytes >> 10);
                last_tile_predictive_log_frame = frame_id_;
              }
              transfer_tile_instance_predictive_cutoff_hit = false;
            }
            if (!use_tile_instancing &&
                transfer_tile_instance_adaptive_cutoff_hit) {
              static uint64_t last_tile_adaptive_log_frame = 0;
              if (last_tile_adaptive_log_frame != frame_id_) {
                XELOGI(
                    "Metal transfer tile instancing fallback: adaptive cutoff "
                    "{} KiB > {} KiB (rects={})",
                    transfer_tile_instance_adaptive_candidate_bytes >> 10,
                    transfer_tile_instance_adaptive_limit_bytes >> 10,
                    transfer_tile_instance_adaptive_rect_count);
                last_tile_adaptive_log_frame = frame_id_;
              }
              transfer_tile_instance_adaptive_cutoff_hit = false;
            }
            if (!use_tile_instancing && transfer_tile_instance_budget_hit) {
              static uint64_t last_tile_budget_log_frame = 0;
              if (last_tile_budget_log_frame != frame_id_) {
                XELOGW(
                    "Metal transfer tile instancing fallback: exceeded {} MiB "
                    "instance-buffer budget this frame",
                    kTransferTileInstanceBufferMaxBytes >> 20);
                last_tile_budget_log_frame = frame_id_;
              }
              transfer_tile_instance_budget_hit = false;
            }
          }
          if (!use_tile_instancing) {
            if (rectangle_count > 1) {
              build_rect_instance_stream(merged_transfer_rectangles.data(),
                                         rectangle_count, rect_instance_buffer,
                                         rect_instance_buffer_offset,
                                         rect_instance_count);
            }
            if (!rect_instance_buffer || !rect_instance_count) {
              rect_instance_fallback.reserve(rectangle_count);
              for (uint32_t rect_index = 0; rect_index < rectangle_count;
                   ++rect_index) {
                uint32_t scaled_x = 0;
                uint32_t scaled_y = 0;
                uint32_t scaled_width = 0;
                uint32_t scaled_height = 0;
                if (!get_scaled_rect(merged_transfer_rectangles[rect_index],
                                     scaled_x, scaled_y, scaled_width,
                                     scaled_height) ||
                    !scaled_width || !scaled_height) {
                  continue;
                }
                TransferRectInstance rect_instance = {};
                rect_instance.origin_x = float(scaled_x);
                rect_instance.origin_y = float(scaled_y);
                rect_instance.size_x = float(scaled_width);
                rect_instance.size_y = float(scaled_height);
                rect_instance_fallback.push_back(rect_instance);
              }
            }
          }

          MTL::RenderPipelineState* pipeline = GetOrCreateTransferPipelines(
              shader_key, dest_pixel_format, dest_is_uint, use_tile_instancing);
          if (!pipeline) {
            continue;
          }
          bind_transfer_pipeline(pipeline);

          bool use_sample_id_for_invocation =
              shader_key.dest_sample_id_from_sample != 0;
          auto draw_transfer_samples = [&](auto&& draw_fn) {
            if (use_sample_id_for_invocation || dest_sample_count <= 1) {
              draw_fn(0);
              return;
            }
            for (uint32_t sample_id = 0; sample_id < dest_sample_count;
                 ++sample_id) {
              draw_fn(sample_id);
            }
          };

          auto draw_transfer = [&](uint32_t sample_id) {
            constants.dest_sample_id = sample_id;
            bind_transfer_constants(constants);
            if (use_tile_instancing) {
              set_full_transfer_viewport_scissor();
              for (const auto& batch : tile_batches) {
                bind_transfer_scissor(batch.scissor);
                bind_transfer_vertex_buffer_1(batch.buffer,
                                              batch.buffer_offset);
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip,
                                        NS::UInteger(0), NS::UInteger(4),
                                        NS::UInteger(batch.instance_count));
              }
            } else {
              if (rect_instance_buffer && rect_instance_count) {
                set_full_transfer_viewport_scissor();
                bind_transfer_vertex_buffer_1(rect_instance_buffer,
                                              rect_instance_buffer_offset);
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip,
                                        NS::UInteger(0), NS::UInteger(4),
                                        NS::UInteger(rect_instance_count));
              } else if (!rect_instance_fallback.empty()) {
                set_full_transfer_viewport_scissor();
                constexpr uint32_t kTransferRectInlineBatchMax = 240;
                const TransferRectInstance* rect_instances =
                    rect_instance_fallback.data();
                uint32_t rect_instances_remaining =
                    uint32_t(rect_instance_fallback.size());
                while (rect_instances_remaining) {
                  uint32_t batch_count = std::min(rect_instances_remaining,
                                                  kTransferRectInlineBatchMax);
                  if (batch_count == 1) {
                    bind_transfer_vertex_bytes_1(*rect_instances);
                  } else {
                    bind_transfer_vertex_bytes_1_span(rect_instances,
                                                      batch_count);
                  }
                  encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip,
                                          NS::UInteger(0), NS::UInteger(4),
                                          NS::UInteger(batch_count));
                  rect_instances += batch_count;
                  rect_instances_remaining -= batch_count;
                }
              }
            }
          };

          if (is_stencil_bit) {
            for (uint32_t bit = 0; bit < 8; ++bit) {
              MTL::DepthStencilState* stencil_state =
                  GetTransferStencilBitState(bit);
              if (!stencil_state) {
                continue;
              }
              constants.stencil_mask = uint32_t(1) << bit;
              constants.stencil_clear = 0;
              bind_transfer_depth_state(stencil_state);
              bind_transfer_stencil_reference(uint32_t(1) << bit);
              draw_transfer_samples(draw_transfer);
            }
          } else {
            constants.stencil_mask = 0;
            constants.stencil_clear = 0;
            draw_transfer_samples(draw_transfer);
          }
          any_transfers_done = true;
        }
      }
    }

    if (resolve_clear_needed && !resolve_clear_via_load_action) {
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_is_depth) {
        uint32_t depth_guest_clear_value =
            (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        float depth_host_clear_value = 0.0f;
        switch (dest_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            depth_host_clear_value =
                xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            depth_host_clear_value =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        MTL::RenderPipelineState* clear_pipeline =
            GetOrCreateTransferClearPipeline(dest_pixel_format, false, true,
                                             dest_sample_count);
        MTL::DepthStencilState* clear_state = GetTransferDepthClearState();
        if (clear_pipeline && clear_state) {
          MTL::RenderCommandEncoder* clear_encoder = ensure_transfer_encoder();
          if (clear_encoder) {
            TransferClearDepthConstants constants = {};
            constants.depth = depth_host_clear_value;
            clear_encoder->setRenderPipelineState(clear_pipeline);
            clear_encoder->setDepthStencilState(clear_state);
            clear_encoder->setStencilReferenceValue(uint32_t(clear_value) &
                                                    0xFF);
            clear_encoder->setFragmentBytes(&constants, sizeof(constants), 0);
            Transfer::Rectangle clear_rect = *resolve_clear_rectangle;
            if (set_rect_viewport(clear_encoder, clear_rect)) {
              clear_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                            NS::UInteger(0), NS::UInteger(3));
            }
          }
        }
      } else {
        TransferClearColorFloatConstants float_constants = {};
        TransferClearColorUintConstants uint_constants = {};
        bool clear_via_drawing = false;
        switch (dest_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            if (gamma_render_target_as_unorm16_) {
              for (uint32_t j = 0; j < 3; ++j) {
                float_constants.color[j] =
                    xenos::PWLGammaToLinear(float_constants.color[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::
              k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            for (uint32_t j = 0; j < 2; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            float_constants.color[1] = float(uint32_t(clear_value >> 32));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value) ||
                uint64_t(float_constants.color[1]) !=
                    uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }

        bool clear_is_uint = false;
        MTL::PixelFormat clear_format = GetColorOwnershipTransferPixelFormat(
            dest_key.GetColorFormat(), &clear_is_uint);
        MTL::Texture* clear_texture = dest_metal_rt->transfer_texture();
        bool clear_use_uint = clear_is_uint;

        if (clear_use_uint) {
          switch (dest_key.GetColorFormat()) {
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value) & 0xFFFF;
              uint_constants.color[1] = (uint32_t(clear_value) >> 16) & 0xFFFF;
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            case xenos::ColorRenderTargetFormat::k_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value) & 0xFFFF;
              uint_constants.color[1] = (uint32_t(clear_value) >> 16) & 0xFFFF;
              uint_constants.color[2] = (uint32_t(clear_value >> 32)) & 0xFFFF;
              uint_constants.color[3] =
                  (uint32_t(clear_value >> 32) >> 16) & 0xFFFF;
              break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value);
              uint_constants.color[1] = 0;
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value);
              uint_constants.color[1] = uint32_t(clear_value >> 32);
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            default:
              break;
          }
        }

        if (clear_via_drawing && clear_use_uint) {
          uint_constants.color[0] = uint32_t(clear_value);
          uint_constants.color[1] = uint32_t(clear_value >> 32);
          uint_constants.color[2] = 0;
          uint_constants.color[3] = 0;
        }

        if (clear_texture) {
          MTL::RenderPipelineState* clear_pipeline =
              GetOrCreateTransferClearPipeline(clear_format, clear_use_uint,
                                               false, dest_sample_count);
          if (clear_pipeline) {
            MTL::RenderCommandEncoder* clear_encoder =
                ensure_transfer_encoder();
            if (clear_encoder) {
              MTL::DepthStencilState* no_depth_state =
                  GetTransferNoDepthStencilState();
              if (!no_depth_state) {
                continue;
              }
              clear_encoder->setRenderPipelineState(clear_pipeline);
              clear_encoder->setDepthStencilState(no_depth_state);
              if (clear_use_uint) {
                clear_encoder->setFragmentBytes(&uint_constants,
                                                sizeof(uint_constants), 0);
              } else {
                clear_encoder->setFragmentBytes(&float_constants,
                                                sizeof(float_constants), 0);
              }
              Transfer::Rectangle clear_rect = *resolve_clear_rectangle;
              if (set_rect_viewport(clear_encoder, clear_rect)) {
                clear_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                              NS::UInteger(0), NS::UInteger(3));
              }
            }
          }
        }
      }
    }

    if (transfer_encoder) {
      transfer_encoder->endEncoding();
    }
  }
}

MTL::RenderPipelineState* MetalRenderTargetCache::GetOrCreateTransferPipelines(
    const TransferShaderKey& key, MTL::PixelFormat dest_format,
    bool dest_is_uint, bool tile_instanced) {
  TransferShaderKey pipeline_key = key;
  pipeline_key.host_depth_source_is_copy = 0;
  auto& pipeline_map =
      tile_instanced ? transfer_tile_pipelines_ : transfer_pipelines_;
  auto it = pipeline_map.find(pipeline_key);
  if (it != pipeline_map.end()) {
    return it->second;
  }

  const TransferModeInfo& mode_info = kTransferModeInfos[size_t(key.mode)];
  TransferOutput output = mode_info.output;
  bool source_is_color = mode_info.source_is_color;
  bool has_host_depth = mode_info.uses_host_depth;

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);

  bool source_is_uint = false;
  if (source_is_color) {
    GetColorOwnershipTransferPixelFormat(source_color_format, &source_is_uint);
  }
  bool source_is_64bpp = false;
  if (source_is_color) {
    source_is_64bpp =
        xenos::IsColorRenderTargetFormat64bpp(source_color_format);
  }
  bool dest_is_depth = output != TransferOutput::kColor;
  bool dest_is_64bpp = false;
  if (!dest_is_depth) {
    dest_is_64bpp =
        xenos::IsColorRenderTargetFormat64bpp(dest_color_format) ||
        (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
         gamma_render_target_as_unorm16_);
  }
  bool source_needs_stencil =
      !source_is_color && (output == TransferOutput::kColor ||
                           output == TransferOutput::kStencilBit);

  uint32_t dest_component_count = 1;
  if (output == TransferOutput::kColor) {
    dest_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
  }

  bool source_is_multisample =
      key.source_msaa_samples != xenos::MsaaSamples::k1X;
  bool dest_is_multisample = key.dest_msaa_samples != xenos::MsaaSamples::k1X;
  bool host_depth_is_multisample =
      key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X;
  uint32_t host_depth_texture_index = source_is_color ? 1 : 2;

  auto append_define = [](std::string& source, const char* name,
                          uint32_t value) {
    source.append("#define ");
    source.append(name);
    source.push_back(' ');
    source.append(std::to_string(value));
    source.push_back('\n');
  };

  std::string source;
  source.reserve(16384);
  append_define(source, "XE_TRANSFER_SOURCE_IS_COLOR", source_is_color ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_IS_DEPTH", source_is_color ? 0 : 1);
  append_define(source, "XE_TRANSFER_SOURCE_NEEDS_STENCIL",
                source_needs_stencil ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_IS_UINT", source_is_uint ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_IS_64BPP", source_is_64bpp ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_IS_MULTISAMPLE",
                source_is_multisample ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_IS_UINT", dest_is_uint ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_IS_DEPTH", dest_is_depth ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_IS_64BPP", dest_is_64bpp ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_COMPONENTS", dest_component_count);
  append_define(source, "XE_TRANSFER_DEST_IS_MULTISAMPLE",
                dest_is_multisample ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_SAMPLE_ID_FROM_SAMPLE",
                key.dest_sample_id_from_sample ? 1 : 0);
  append_define(source, "XE_TRANSFER_HAS_HOST_DEPTH", has_host_depth ? 1 : 0);
  append_define(source, "XE_TRANSFER_HOST_DEPTH_IS_MULTISAMPLE",
                host_depth_is_multisample ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_FORMAT",
                key.source_resource_format);
  append_define(source, "XE_TRANSFER_DEST_FORMAT", key.dest_resource_format);
  append_define(source, "XE_TRANSFER_SOURCE_MSAA_SAMPLES",
                MsaaSamplesToCount(key.source_msaa_samples));
  append_define(source, "XE_TRANSFER_DEST_MSAA_SAMPLES",
                MsaaSamplesToCount(key.dest_msaa_samples));
  append_define(source, "XE_TRANSFER_HOST_DEPTH_MSAA_SAMPLES",
                MsaaSamplesToCount(key.host_depth_source_msaa_samples));
  append_define(source, "XE_TRANSFER_SOURCE_TEXTURE_INDEX", 0);
  append_define(source, "XE_TRANSFER_STENCIL_TEXTURE_INDEX", 1);
  append_define(source, "XE_TRANSFER_HOST_DEPTH_TEXTURE_INDEX",
                host_depth_texture_index);
  append_define(source, "XE_TRANSFER_OUTPUT_COLOR",
                output == TransferOutput::kColor ? 1 : 0);
  append_define(source, "XE_TRANSFER_OUTPUT_DEPTH",
                output == TransferOutput::kDepth ? 1 : 0);
  append_define(source, "XE_TRANSFER_OUTPUT_STENCIL_BIT",
                output == TransferOutput::kStencilBit ? 1 : 0);
  append_define(source, "XE_TRANSFER_TILE_INSTANCED", tile_instanced ? 1 : 0);
  append_define(source, "XE_TRANSFER_FAST_DIVMOD",
                ::cvars::metal_transfer_fast_divmod ? 1 : 0);
  append_define(source, "XE_FMT_8_8_8_8",
                uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8));
  append_define(source, "XE_FMT_8_8_8_8_GAMMA",
                uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA));
  append_define(source, "XE_FMT_2_10_10_10",
                uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10));
  append_define(
      source, "XE_FMT_2_10_10_10_AS_10_10_10_10",
      uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10));
  append_define(source, "XE_FMT_2_10_10_10_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT));
  append_define(
      source, "XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16",
      uint32_t(
          xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16));
  append_define(source, "XE_FMT_16_16",
                uint32_t(xenos::ColorRenderTargetFormat::k_16_16));
  append_define(source, "XE_FMT_16_16_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_16_16_FLOAT));
  append_define(source, "XE_FMT_16_16_16_16",
                uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16));
  append_define(source, "XE_FMT_16_16_16_16_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT));
  append_define(source, "XE_FMT_32_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_32_FLOAT));
  append_define(source, "XE_FMT_32_32_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_32_32_FLOAT));
  append_define(source, "XE_FMT_D24S8",
                uint32_t(xenos::DepthRenderTargetFormat::kD24S8));
  append_define(source, "XE_FMT_D24FS8",
                uint32_t(xenos::DepthRenderTargetFormat::kD24FS8));
  append_define(source, "XE_GAMMA_RT_AS_UNORM16",
                gamma_render_target_as_unorm16_ ? 1 : 0);

  static const char kTransferShaderSource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct TransferAddressConstants {
  uint dest_pitch;
  uint source_pitch;
  int source_to_dest;
};

struct TransferShaderConstants {
  TransferAddressConstants address;
  TransferAddressConstants host_depth_address;
  uint source_format;
  uint dest_format;
  uint source_is_depth;
  uint dest_is_depth;
  uint source_is_uint;
  uint dest_is_uint;
  uint source_is_64bpp;
  uint dest_is_64bpp;
  uint source_msaa_samples;
  uint dest_msaa_samples;
  uint host_depth_source_msaa_samples;
  uint host_depth_source_is_copy;
  uint depth_round;
  uint msaa_2x_supported;
  uint tile_width_samples;
  uint tile_height_samples;
  uint dest_tile_width_pixels;
  uint dest_tile_height_pixels;
  float dest_tile_width_pixels_inv;
  float dest_tile_height_pixels_inv;
  float source_pitch_tiles_inv;
  float host_depth_source_pitch_tiles_inv;
  float dest_pixel_to_ndc_x;
  float dest_pixel_to_ndc_y;
  uint dest_sample_id;
  uint stencil_mask;
  uint stencil_clear;
};

struct TransferTileInstance {
  float2 tile_origin;
  uint tile_index;
  uint padding;
  uint2 source_base;
  uint2 host_base;
};

struct TransferRectInstance {
  float2 origin;
  float2 size;
};

constant uint kEdramTileCount = 2048u;

inline uint XeBitFieldMask(uint count) {
  if (count >= 32u) {
    return 0xFFFFFFFFu;
  }
  return (1u << count) - 1u;
}

inline uint XeBitFieldInsert(uint base, uint insert, uint offset, uint count) {
  uint mask = XeBitFieldMask(count) << offset;
  return (base & ~mask) | ((insert << offset) & mask);
}

inline uint XeBitFieldExtract(uint value, uint offset, uint count) {
  return (value >> offset) & XeBitFieldMask(count);
}

inline uint XeRoundToNearestEven(float value) {
  float floor_value = floor(value);
  float frac = value - floor_value;
  uint result = uint(floor_value);
  if (frac > 0.5f || (frac == 0.5f && (result & 1u))) {
    result += 1u;
  }
  return result;
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

inline float XeSaturateNoNaN(float value) {
  float clamped = clamp(value, 0.0f, 1.0f);
  return (clamped == clamped) ? clamped : 0.0f;
}

inline float XeTruncToFloat(float value) {
  return trunc(value);
}

inline float XePWLGammaToLinear(float value) {
  float clamped = XeSaturateNoNaN(value);
  float scale;
  float offset;
  if (clamped >= (96.0f / 255.0f)) {
    if (clamped >= (192.0f / 255.0f)) {
      scale = 8.0f / 1024.0f;
      offset = -1024.0f;
    } else {
      scale = 4.0f / 1024.0f;
      offset = -256.0f;
    }
  } else {
    if (clamped >= (64.0f / 255.0f)) {
      scale = 2.0f / 1024.0f;
      offset = -64.0f;
    } else {
      scale = 1.0f / 1024.0f;
      offset = 0.0f;
    }
  }
  float linear = clamped * (255.0f * 1024.0f) * scale + offset;
  linear += XeTruncToFloat(linear * scale);
  return linear * (1.0f / 1023.0f);
}

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

inline float XeLinearToPWLGamma(float value) {
  float clamped = XeSaturateNoNaN(value);
  float scale;
  float offset;
  if (clamped >= (128.0f / 1023.0f)) {
    if (clamped >= (512.0f / 1023.0f)) {
      scale = 1023.0f / 8.0f;
      offset = 128.0f / 255.0f;
    } else {
      scale = 1023.0f / 4.0f;
      offset = 64.0f / 255.0f;
    }
  } else {
    if (clamped >= (64.0f / 1023.0f)) {
      scale = 1023.0f / 2.0f;
      offset = 32.0f / 255.0f;
    } else {
      scale = 1023.0f;
      offset = 0.0f;
    }
  }
  return XeTruncToFloat(clamped * scale) * (1.0f / 255.0f) + offset;
}

inline float3 XePWLGammaToLinear3(float3 v) {
  return float3(XePWLGammaToLinear(v.r), XePWLGammaToLinear(v.g),
                XePWLGammaToLinear(v.b));
}

inline float3 XeLinearToPWLGamma3(float3 v) {
  return float3(XeLinearToPWLGamma(v.r), XeLinearToPWLGamma(v.g),
                XeLinearToPWLGamma(v.b));
}

uint XePreClampedFloat32To7e3(float value) {
  uint f32 = as_type<uint>(value);
  uint biased_f32;
  if (f32 < 0x3E800000u) {
    uint f32_exp = f32 >> 23u;
    uint shift = 125u - f32_exp;
    shift = min(shift, 24u);
    uint mantissa = (f32 & 0x7FFFFFu) | 0x800000u;
    biased_f32 = mantissa >> shift;
  } else {
    biased_f32 = f32 + 0xC2000000u;
  }
  uint round_bit = (biased_f32 >> 16u) & 1u;
  uint f10 = biased_f32 + 0x7FFFu + round_bit;
  return (f10 >> 16u) & 0x3FFu;
}

uint XeUnclampedFloat32To7e3(float value) {
  float clamped = min(max(value, 0.0f), 31.875f);
  return XePreClampedFloat32To7e3(clamped);
}

float XeFloat7e3To32(uint f10) {
  f10 &= 0x3FFu;
  if (f10 == 0u) {
    return 0.0f;
  }
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint mantissa_lzcnt = clz(mantissa) - 24u;
    exponent = uint(int(1) - int(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7Fu;
  }
  uint f32 = ((exponent + 124u) << 23u) | (mantissa << 16u);
  return as_type<float>(f32);
}

uint XeFloat32To20e4(float value, bool round_to_nearest_even) {
  uint f32 = as_type<uint>(value);
  f32 = min((f32 <= 0x7FFFFFFFu) ? f32 : 0u, 0x3FFFFFF8u);
  uint denormalized =
      ((f32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32 >> 23u), 24u);
  uint f24 = (f32 < 0x38800000u) ? denormalized : (f32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24 += 3u + ((f24 >> 3u) & 1u);
  }
  return (f24 >> 3u) & 0xFFFFFFu;
}

float XeFloat20e4To32(uint f24, bool remap_to_0_to_0_5) {
  if (f24 == 0u) {
    return 0.0f;
  }
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20u;
  if (exponent == 0u) {
    uint msb = 31u - clz(mantissa);
    uint mantissa_lzcnt = 20u - msb;
    exponent = 1u - mantissa_lzcnt;
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFFu;
  }
  uint bias = remap_to_0_to_0_5 ? 111u : 112u;
  uint f32 = ((exponent + bias) << 23u) | (mantissa << 3u);
  return as_type<float>(f32);
}

float XeUnorm24To32(uint n24) {
  return float(n24 + (n24 >> 23u)) * (1.0f / 16777216.0f);
}

uint XePackColorRGBA8(float4 color) {
  uint r = XePackUnorm(color.r, 255.0f);
  uint g = XePackUnorm(color.g, 255.0f);
  uint b = XePackUnorm(color.b, 255.0f);
  uint a = XePackUnorm(color.a, 255.0f);
  return r | (g << 8u) | (b << 16u) | (a << 24u);
}

uint XePackColorRGB10A2(float4 color) {
  uint r = XePackUnorm(color.r, 1023.0f);
  uint g = XePackUnorm(color.g, 1023.0f);
  uint b = XePackUnorm(color.b, 1023.0f);
  uint a = XePackUnorm(color.a, 3.0f);
  return r | (g << 10u) | (b << 20u) | (a << 30u);
}

uint XePackColorRGB10A2Float(float4 color) {
  uint r = XeUnclampedFloat32To7e3(color.r);
  uint g = XeUnclampedFloat32To7e3(color.g);
  uint b = XeUnclampedFloat32To7e3(color.b);
  uint a = XePackUnorm(color.a, 3.0f);
  return (r & 0x3FFu) | ((g & 0x3FFu) << 10u) |
         ((b & 0x3FFu) << 20u) | ((a & 0x3u) << 30u);
}

struct VSOut {
  float4 position [[position]];
  float2 tile_origin [[flat]];
  uint tile_index [[flat]];
  uint2 source_base [[flat]];
  uint2 host_base [[flat]];
};

vertex VSOut transfer_vs(uint vid [[vertex_id]]) {
  float2 pt = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(pt * 2.0f - 1.0f, 0.0f, 1.0f);
  out.tile_origin = float2(0.0f);
  out.tile_index = 0u;
  out.source_base = uint2(0u);
  out.host_base = uint2(0u);
  return out;
}

vertex VSOut transfer_rect_vs(uint vid [[vertex_id]],
                              uint iid [[instance_id]],
                              constant TransferShaderConstants& constants
                                  [[buffer(0)]],
                              constant TransferRectInstance* instances
                                  [[buffer(1)]]) {
  float2 quad = float2(float(vid & 1), float(vid >> 1));
  TransferRectInstance inst = instances[iid];
  float2 pos_pixel = inst.origin + quad * inst.size;
  float2 ndc;
  ndc.x = pos_pixel.x * constants.dest_pixel_to_ndc_x - 1.0f;
  ndc.y = 1.0f - pos_pixel.y * constants.dest_pixel_to_ndc_y;
  VSOut out;
  out.position = float4(ndc, 0.0f, 1.0f);
  out.tile_origin = float2(0.0f);
  out.tile_index = 0u;
  out.source_base = uint2(0u);
  out.host_base = uint2(0u);
  return out;
}

vertex VSOut transfer_tile_vs(uint vid [[vertex_id]],
                              uint iid [[instance_id]],
                              constant TransferShaderConstants& constants
                                  [[buffer(0)]],
                              device const TransferTileInstance* instances
                                  [[buffer(1)]]) {
  float2 quad = float2(float(vid & 1), float(vid >> 1));
  TransferTileInstance inst = instances[iid];
  float2 tile_size = float2(constants.dest_tile_width_pixels,
                            constants.dest_tile_height_pixels);
  float2 pos_pixel = inst.tile_origin + quad * tile_size;
  float2 ndc;
  ndc.x = pos_pixel.x * constants.dest_pixel_to_ndc_x - 1.0f;
  ndc.y = 1.0f - pos_pixel.y * constants.dest_pixel_to_ndc_y;
  VSOut out;
  out.position = float4(ndc, 0.0f, 1.0f);
  out.tile_origin = inst.tile_origin;
  out.tile_index = inst.tile_index;
  out.source_base = inst.source_base;
  out.host_base = inst.host_base;
  return out;
}

#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
    #if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d_ms<uint, access::read> xe_transfer_source \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #else
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d<uint, access::read> xe_transfer_source \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #endif
  #else
    #if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d_ms<float, access::read> xe_transfer_source \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #else
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d<float, access::read> xe_transfer_source \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #endif
  #endif
#else
  #if XE_TRANSFER_SOURCE_NEEDS_STENCIL
    #if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d_ms<float, access::read> xe_transfer_source_depth \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]], \
            texture2d_ms<uint, access::read> xe_transfer_source_stencil \
              [[texture(XE_TRANSFER_STENCIL_TEXTURE_INDEX)]]
    #else
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d<float, access::read> xe_transfer_source_depth \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]], \
            texture2d<uint, access::read> xe_transfer_source_stencil \
              [[texture(XE_TRANSFER_STENCIL_TEXTURE_INDEX)]]
    #endif
  #else
    #if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d_ms<float, access::read> xe_transfer_source_depth \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #else
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d<float, access::read> xe_transfer_source_depth \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #endif
  #endif
#endif

#if XE_TRANSFER_HAS_HOST_DEPTH
  #define XE_TRANSFER_HOST_DEPTH_BUFFER_PARAM \
      , device const uint* xe_transfer_host_depth_buffer [[buffer(1)]]
#else
  #define XE_TRANSFER_HOST_DEPTH_BUFFER_PARAM
#endif

#if XE_TRANSFER_HAS_HOST_DEPTH
  #if XE_TRANSFER_HOST_DEPTH_IS_MULTISAMPLE
    #define XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM \
        , texture2d_ms<float, access::read> xe_transfer_host_depth \
            [[texture(XE_TRANSFER_HOST_DEPTH_TEXTURE_INDEX)]]
  #else
    #define XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM \
        , texture2d<float, access::read> xe_transfer_host_depth \
            [[texture(XE_TRANSFER_HOST_DEPTH_TEXTURE_INDEX)]]
  #endif
#else
  #define XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM
#endif

#if XE_TRANSFER_DEST_IS_MULTISAMPLE && XE_TRANSFER_DEST_SAMPLE_ID_FROM_SAMPLE
  #define XE_TRANSFER_SAMPLE_ID_PARAM , uint xe_sample_id [[sample_id]]
#else
  #define XE_TRANSFER_SAMPLE_ID_PARAM
#endif

#if XE_TRANSFER_OUTPUT_COLOR
  #if XE_TRANSFER_DEST_IS_UINT
    #if XE_TRANSFER_DEST_COMPONENTS == 1
      typedef uint XeTransferColorOutType;
    #elif XE_TRANSFER_DEST_COMPONENTS == 2
      typedef uint2 XeTransferColorOutType;
    #else
      typedef uint4 XeTransferColorOutType;
    #endif
  #else
    #if XE_TRANSFER_DEST_COMPONENTS == 1
      typedef float XeTransferColorOutType;
    #elif XE_TRANSFER_DEST_COMPONENTS == 2
      typedef float2 XeTransferColorOutType;
    #else
      typedef float4 XeTransferColorOutType;
    #endif
  #endif

struct TransferColorOut {
  XeTransferColorOutType color [[color(0)]];
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  uint sample_mask [[sample_mask]];
#endif
};

fragment TransferColorOut transfer_ps(
    VSOut in [[stage_in]],
    constant TransferShaderConstants& constants [[buffer(0)]]
    XE_TRANSFER_HOST_DEPTH_BUFFER_PARAM
    XE_TRANSFER_SOURCE_PARAMS
    XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM
    XE_TRANSFER_SAMPLE_ID_PARAM) {
  uint2 dest_pixel = uint2(in.position.xy);
  uint dest_sample_id = 0u;
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  #if XE_TRANSFER_DEST_SAMPLE_ID_FROM_SAMPLE
    dest_sample_id = xe_sample_id;
  #else
    dest_sample_id = constants.dest_sample_id;
  #endif
#endif

  uint tile_width_samples = constants.tile_width_samples;
  uint tile_height_samples = constants.tile_height_samples;
  uint dest_tile_width_pixels = constants.dest_tile_width_pixels;
  uint dest_tile_height_pixels = constants.dest_tile_height_pixels;

  uint dest_tile_pixel_x = 0u;
  uint dest_tile_pixel_y = 0u;
  uint dest_tile_index = 0u;
#if XE_TRANSFER_TILE_INSTANCED
  uint2 tile_origin = uint2(in.tile_origin);
  dest_tile_pixel_x = dest_pixel.x - tile_origin.x;
  dest_tile_pixel_y = dest_pixel.y - tile_origin.y;
  dest_tile_index = in.tile_index;
#else
  uint dest_tile_index_x = 0u;
  uint dest_tile_index_y = 0u;
#if XE_TRANSFER_FAST_DIVMOD
  XeFastDivMod(dest_pixel.x, dest_tile_width_pixels,
               constants.dest_tile_width_pixels_inv, dest_tile_index_x,
               dest_tile_pixel_x);
  XeFastDivMod(dest_pixel.y, dest_tile_height_pixels,
               constants.dest_tile_height_pixels_inv, dest_tile_index_y,
               dest_tile_pixel_y);
#else
  dest_tile_index_x = dest_pixel.x / dest_tile_width_pixels;
  dest_tile_pixel_x = dest_pixel.x % dest_tile_width_pixels;
  dest_tile_index_y = dest_pixel.y / dest_tile_height_pixels;
  dest_tile_pixel_y = dest_pixel.y % dest_tile_height_pixels;
#endif

  dest_tile_index =
      dest_tile_index_x +
      dest_tile_index_y * constants.address.dest_pitch;
#endif

  uint source_sample_id = dest_sample_id;
  uint source_tile_pixel_x = dest_tile_pixel_x;
  uint source_tile_pixel_y = dest_tile_pixel_y;
  uint source_color_half = 0u;
  bool source_color_half_valid = false;

  bool source_is_64bpp = XE_TRANSFER_SOURCE_IS_64BPP != 0u;
  bool dest_is_64bpp = XE_TRANSFER_DEST_IS_64BPP != 0u;
  uint source_msaa = XE_TRANSFER_SOURCE_MSAA_SAMPLES;
  uint dest_msaa = XE_TRANSFER_DEST_MSAA_SAMPLES;
  bool msaa_2x_supported = constants.msaa_2x_supported != 0u;

  if (!source_is_64bpp && dest_is_64bpp) {
    if (source_msaa >= 4u) {
      if (dest_msaa >= 4u) {
        source_sample_id = dest_sample_id & 2u;
        source_tile_pixel_x =
            XeBitFieldInsert(dest_sample_id, dest_tile_pixel_x, 1u, 31u);
      } else if (dest_msaa == 2u) {
        if (msaa_2x_supported) {
          source_sample_id = (dest_sample_id ^ 1u) << 1u;
        } else {
          source_sample_id = dest_sample_id & 2u;
        }
      } else {
        source_sample_id =
            XeBitFieldInsert(0u, dest_tile_pixel_y, 1u, 1u);
        source_tile_pixel_y = dest_tile_pixel_y >> 1u;
      }
    } else {
      if (dest_msaa >= 4u) {
        source_tile_pixel_x = XeBitFieldInsert(
            dest_tile_pixel_x << 2u, dest_sample_id, 1u, 1u);
      } else {
        source_tile_pixel_x = dest_tile_pixel_x << 1u;
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    if (dest_msaa >= 4u) {
      if (source_msaa >= 4u) {
        source_sample_id =
            XeBitFieldInsert(dest_sample_id, dest_tile_pixel_x, 0u, 1u);
        source_tile_pixel_x = dest_tile_pixel_x >> 1u;
      }
      source_color_half = dest_sample_id & 1u;
      source_color_half_valid = true;
    } else {
      if (source_msaa >= 4u) {
        source_sample_id = XeBitFieldExtract(dest_tile_pixel_x, 1u, 1u);
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            source_sample_id = XeBitFieldInsert(
                source_sample_id, dest_sample_id ^ 1u, 1u, 1u);
          } else {
            source_sample_id = XeBitFieldInsert(
                dest_sample_id, source_sample_id, 0u, 1u);
          }
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, dest_tile_pixel_y, 1u, 1u);
          source_tile_pixel_y = dest_tile_pixel_y >> 1u;
        }
        source_tile_pixel_x = dest_tile_pixel_x >> 2u;
      } else {
        source_tile_pixel_x = dest_tile_pixel_x >> 1u;
      }
      source_color_half = dest_tile_pixel_x & 1u;
      source_color_half_valid = true;
    }
  } else {
    if (source_msaa != dest_msaa) {
      if (source_msaa >= 4u) {
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            source_sample_id = XeBitFieldInsert(
                dest_tile_pixel_x, dest_sample_id ^ 1u, 1u, 31u);
          } else {
            source_sample_id = XeBitFieldInsert(
                dest_sample_id, dest_tile_pixel_x, 0u, 1u);
          }
          source_tile_pixel_x = dest_tile_pixel_x >> 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              dest_tile_pixel_x & 1u, dest_tile_pixel_y, 1u, 1u);
          source_tile_pixel_x = dest_tile_pixel_x >> 1u;
          source_tile_pixel_y = dest_tile_pixel_y >> 1u;
        }
      } else if (dest_msaa >= 4u) {
        source_tile_pixel_x = XeBitFieldInsert(
            dest_sample_id, dest_tile_pixel_x, 1u, 31u);
      }
    }
  }

  if (source_msaa < 4u && source_msaa != dest_msaa) {
    if (dest_msaa >= 4u) {
      if (source_msaa == 2u) {
        source_sample_id = dest_sample_id >> 1u;
        if (msaa_2x_supported) {
          source_sample_id ^= 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, source_sample_id, 1u, 1u);
        }
      } else {
        source_tile_pixel_y = XeBitFieldInsert(
            dest_sample_id >> 1u, dest_tile_pixel_y, 1u, 31u);
      }
    } else {
      if (source_msaa == 2u) {
        source_sample_id = dest_tile_pixel_y & 1u;
        if (msaa_2x_supported) {
          source_sample_id ^= 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, source_sample_id, 1u, 1u);
        }
        source_tile_pixel_y = dest_tile_pixel_y >> 1u;
      } else {
        if (msaa_2x_supported) {
          source_tile_pixel_y = XeBitFieldInsert(
              dest_sample_id ^ 1u, dest_tile_pixel_y, 1u, 31u);
        } else {
          source_tile_pixel_y = XeBitFieldInsert(
              dest_sample_id >> 1u, dest_tile_pixel_y, 1u, 31u);
        }
      }
    }
  }

  uint source_pixel_width_dwords_log2 =
      (source_msaa >= 4u ? 1u : 0u) + (source_is_64bpp ? 1u : 0u);

  if ((XE_TRANSFER_SOURCE_IS_DEPTH != 0u) != (XE_TRANSFER_DEST_IS_DEPTH != 0u)) {
    uint source_32bpp_tile_half_pixels =
        tile_width_samples >> (1u + source_pixel_width_dwords_log2);
    if (source_tile_pixel_x < source_32bpp_tile_half_pixels) {
      source_tile_pixel_x += source_32bpp_tile_half_pixels;
    } else {
      source_tile_pixel_x -= source_32bpp_tile_half_pixels;
    }
  }

  uint source_pixel_x = 0u;
  uint source_pixel_y = 0u;
#if XE_TRANSFER_TILE_INSTANCED
  source_pixel_x = in.source_base.x + source_tile_pixel_x;
  source_pixel_y = in.source_base.y + source_tile_pixel_y;
#else
  uint source_tile_index =
      uint(int(dest_tile_index) + constants.address.source_to_dest) &
      (kEdramTileCount - 1u);
  uint source_pitch_tiles = constants.address.source_pitch;
  uint source_tile_index_y = 0u;
  uint source_tile_index_x = 0u;
  XeFastDivMod(source_tile_index, source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_index_y,
               source_tile_index_x);
  source_pixel_x =
      source_tile_index_x *
          (tile_width_samples >> source_pixel_width_dwords_log2) +
      source_tile_pixel_x;
  source_pixel_y =
      source_tile_index_y *
          (tile_height_samples >> (source_msaa >= 2u ? 1u : 0u)) +
      source_tile_pixel_y;
#endif

  bool load_two = !source_is_64bpp && dest_is_64bpp;
  uint source_pixel_x1 = source_pixel_x;
  uint source_sample_id1 = source_sample_id;
  if (load_two) {
    if (source_msaa >= 4u) {
      source_sample_id1 = source_sample_id | 1u;
    } else {
      source_pixel_x1 = source_pixel_x | 1u;
    }
  }

#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
  uint4 source_color0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y),
                              source_sample_id);
#else
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y));
#endif
  uint4 source_color1 = source_color0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1);
#else
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y));
#endif
  }
  #else
  float4 source_color0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y),
                              source_sample_id);
#else
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y));
#endif
  float4 source_color1 = source_color0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1);
#else
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y));
#endif
  }
  #endif
#else
  float source_depth0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source_depth.read(uint2(source_pixel_x, source_pixel_y),
                                    source_sample_id).r;
#else
      xe_transfer_source_depth.read(uint2(source_pixel_x, source_pixel_y)).r;
#endif
  float source_depth1 = source_depth0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_depth1 = xe_transfer_source_depth.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1).r;
#else
    source_depth1 = xe_transfer_source_depth.read(
        uint2(source_pixel_x1, source_pixel_y)).r;
#endif
  }
#if XE_TRANSFER_SOURCE_NEEDS_STENCIL
  uint source_stencil0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source_stencil.read(uint2(source_pixel_x, source_pixel_y),
                                      source_sample_id).r;
#else
      xe_transfer_source_stencil.read(uint2(source_pixel_x, source_pixel_y)).r;
#endif
  uint source_stencil1 = source_stencil0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_stencil1 = xe_transfer_source_stencil.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1).r;
#else
    source_stencil1 = xe_transfer_source_stencil.read(
        uint2(source_pixel_x1, source_pixel_y)).r;
#endif
  }
#else
  uint source_stencil0 = 0u;
  uint source_stencil1 = 0u;
#endif
#endif

#if XE_TRANSFER_SOURCE_IS_COLOR
  if (source_is_64bpp && !dest_is_64bpp && source_color_half_valid) {
    uint source_component_count = 0u;
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_32_FLOAT:
        source_component_count = 1u;
        break;
      case XE_FMT_16_16:
      case XE_FMT_16_16_FLOAT:
      case XE_FMT_32_32_FLOAT:
        source_component_count = 2u;
        break;
      default:
        source_component_count = 4u;
        break;
    }
    if (source_component_count == 2u) {
  #if XE_TRANSFER_SOURCE_IS_UINT
      source_color0[0] = source_color_half != 0u ? source_color0[1]
                                                 : source_color0[0];
  #else
      source_color0[0] = source_color_half != 0u ? source_color0[1]
                                                 : source_color0[0];
  #endif
    } else if (source_color_half != 0u) {
  #if XE_TRANSFER_SOURCE_IS_UINT
      source_color0[0] = source_color0[2];
      source_color0[1] = source_color0[3];
  #else
      source_color0[0] = source_color0[2];
      source_color0[1] = source_color0[3];
  #endif
    }
  }
#endif

#if XE_TRANSFER_DEST_IS_UINT
  uint4 out_color = uint4(0u);
#else
  float4 out_color = float4(0.0f);
#endif

  if (dest_is_64bpp) {
    uint2 packed64 = uint2(0u);
#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_16_16:
      case XE_FMT_16_16_FLOAT:
        packed64.x = source_color0[0] | (source_color0[1] << 16u);
        packed64.y = source_color1[0] | (source_color1[1] << 16u);
        break;
      case XE_FMT_16_16_16_16:
      case XE_FMT_16_16_16_16_FLOAT:
        packed64.x = source_color0[0] | (source_color0[1] << 16u);
        packed64.y = source_color0[2] | (source_color0[3] << 16u);
        break;
      case XE_FMT_32_FLOAT:
        packed64.x = source_color0[0];
        packed64.y = source_color1[0];
        break;
      case XE_FMT_32_32_FLOAT:
        packed64.x = source_color0[0];
        packed64.y = source_color0[1];
        break;
      default:
        packed64.x = source_color0[0];
        packed64.y = source_color1[0];
        break;
    }
  #else
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_8_8_8_8_GAMMA: {
#if XE_GAMMA_RT_AS_UNORM16
        float4 gamma_color0 = source_color0;
        float4 gamma_color1 = source_color1;
        gamma_color0.rgb = XeLinearToPWLGamma3(gamma_color0.rgb);
        gamma_color1.rgb = XeLinearToPWLGamma3(gamma_color1.rgb);
        packed64.x = XePackColorRGBA8(gamma_color0);
        packed64.y = XePackColorRGBA8(gamma_color1);
#else
        packed64.x = XePackColorRGBA8(source_color0);
        packed64.y = XePackColorRGBA8(source_color1);
#endif
      } break;
      case XE_FMT_8_8_8_8:
        packed64.x = XePackColorRGBA8(source_color0);
        packed64.y = XePackColorRGBA8(source_color1);
        break;
      case XE_FMT_2_10_10_10:
      case XE_FMT_2_10_10_10_AS_10_10_10_10:
        packed64.x = XePackColorRGB10A2(source_color0);
        packed64.y = XePackColorRGB10A2(source_color1);
        break;
      case XE_FMT_2_10_10_10_FLOAT:
      case XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16:
        packed64.x = XePackColorRGB10A2Float(source_color0);
        packed64.y = XePackColorRGB10A2Float(source_color1);
        break;
      case XE_FMT_32_FLOAT:
        packed64.x = as_type<uint>(source_color0[0]);
        packed64.y = as_type<uint>(source_color1[0]);
        break;
      case XE_FMT_32_32_FLOAT:
        packed64.x = as_type<uint>(source_color0[0]);
        packed64.y = as_type<uint>(source_color0[1]);
        break;
      default:
        packed64.x = as_type<uint>(source_color0[0]);
        packed64.y = as_type<uint>(source_color1[0]);
        break;
    }
  #endif
#else
    uint depth24_0 = 0u;
    uint depth24_1 = 0u;
    if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_D24FS8) {
      bool round_depth = constants.depth_round != 0u;
      depth24_0 = XeFloat32To20e4(source_depth0 * 2.0f, round_depth);
      depth24_1 = XeFloat32To20e4(source_depth1 * 2.0f, round_depth);
    } else {
      depth24_0 = XeRoundToNearestEven(
          clamp(source_depth0, 0.0f, 1.0f) * 16777215.0f);
      depth24_1 = XeRoundToNearestEven(
          clamp(source_depth1, 0.0f, 1.0f) * 16777215.0f);
    }
    packed64.x = (depth24_0 << 8u) | (source_stencil0 & 0xFFu);
    packed64.y = (depth24_1 << 8u) | (source_stencil1 & 0xFFu);
#endif

    if (XE_TRANSFER_DEST_FORMAT == XE_FMT_32_32_FLOAT) {
#if XE_TRANSFER_DEST_IS_UINT
      out_color = uint4(packed64.x, packed64.y, 0u, 0u);
#else
      out_color = float4(as_type<float>(packed64.x),
                         as_type<float>(packed64.y), 0.0f, 0.0f);
#endif
    } else {
      uint4 components = uint4(packed64.x & 0xFFFFu, packed64.x >> 16u,
                               packed64.y & 0xFFFFu, packed64.y >> 16u);
#if XE_TRANSFER_DEST_IS_UINT
      out_color = components;
#else
      out_color = float4(components);
#endif
    }
  } else {
    bool wrote_direct = false;
    uint packed32 = 0u;
#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_16_16:
      case XE_FMT_16_16_FLOAT:
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_16_16 ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_16_16_FLOAT) {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(source_color0[0], source_color0[1], 0u, 0u);
#else
          out_color = float4(float(source_color0[0]),
                             float(source_color0[1]), 0.0f, 0.0f);
#endif
          wrote_direct = true;
        } else {
          packed32 = source_color0[0] | (source_color0[1] << 16u);
        }
        break;
      case XE_FMT_16_16_16_16:
      case XE_FMT_16_16_16_16_FLOAT:
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_16_16 ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_16_16_FLOAT) {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(source_color0[0], source_color0[1], 0u, 0u);
#else
          out_color = float4(float(source_color0[0]),
                             float(source_color0[1]), 0.0f, 0.0f);
#endif
          wrote_direct = true;
        } else {
          packed32 = source_color0[0] | (source_color0[1] << 16u);
        }
        break;
      case XE_FMT_32_FLOAT:
      case XE_FMT_32_32_FLOAT:
        packed32 = source_color0[0];
        break;
      default:
        packed32 = source_color0[0];
        break;
    }
  #else
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_8_8_8_8:
      case XE_FMT_8_8_8_8_GAMMA: {
        float4 color = source_color0;
#if XE_GAMMA_RT_AS_UNORM16
        if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_8_8_8_8_GAMMA &&
            (XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8 ||
             XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8_GAMMA) &&
            XE_TRANSFER_DEST_FORMAT != XE_TRANSFER_SOURCE_FORMAT) {
          if (XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8) {
            color.rgb = XeLinearToPWLGamma3(color.rgb);
          } else {
            color.rgb = XePWLGammaToLinear3(color.rgb);
          }
        }
#endif
#if !XE_TRANSFER_DEST_IS_UINT
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8 ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8_GAMMA) {
          out_color = color;
          wrote_direct = true;
        } else
#endif
        {
#if XE_GAMMA_RT_AS_UNORM16
          if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_8_8_8_8_GAMMA) {
            color.rgb = XeLinearToPWLGamma3(color.rgb);
          }
#endif
          uint packed_component_offset = 0u;
          if (XE_TRANSFER_DEST_IS_DEPTH != 0u) {
            packed_component_offset = 1u;
          }
          packed32 =
              XePackUnorm(color[packed_component_offset], 255.0f);
          if (XE_TRANSFER_DEST_IS_DEPTH == 0u) {
            packed32 |= XePackUnorm(color[packed_component_offset + 1],
                                    255.0f) << 8u;
            packed32 |= XePackUnorm(color[packed_component_offset + 2],
                                    255.0f) << 16u;
            packed32 |= XePackUnorm(color[packed_component_offset + 3],
                                    255.0f) << 24u;
          }
        }
      } break;
      case XE_FMT_2_10_10_10:
      case XE_FMT_2_10_10_10_AS_10_10_10_10:
#if !XE_TRANSFER_DEST_IS_UINT
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_2_10_10_10 ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_2_10_10_10_AS_10_10_10_10) {
          out_color = source_color0;
          wrote_direct = true;
        } else
#endif
        {
          packed32 = XePackUnorm(source_color0[0], 1023.0f);
          if (XE_TRANSFER_DEST_IS_DEPTH == 0u) {
            packed32 |= XePackUnorm(source_color0[1], 1023.0f) << 10u;
            packed32 |= XePackUnorm(source_color0[2], 1023.0f) << 20u;
            packed32 |= XePackUnorm(source_color0[3], 3.0f) << 30u;
          }
        }
        break;
      case XE_FMT_2_10_10_10_FLOAT:
      case XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16:
#if !XE_TRANSFER_DEST_IS_UINT
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_2_10_10_10_FLOAT ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16) {
          out_color = source_color0;
          wrote_direct = true;
        } else
#endif
        {
          packed32 = XeUnclampedFloat32To7e3(source_color0[0]);
          if (XE_TRANSFER_DEST_IS_DEPTH == 0u) {
            packed32 |= XeUnclampedFloat32To7e3(source_color0[1]) << 10u;
            packed32 |= XeUnclampedFloat32To7e3(source_color0[2]) << 20u;
            packed32 |= XePackUnorm(source_color0[3], 3.0f) << 30u;
          }
        }
        break;
      case XE_FMT_32_FLOAT:
      case XE_FMT_32_32_FLOAT:
        packed32 = as_type<uint>(source_color0[0]);
        break;
      default:
        packed32 = as_type<uint>(source_color0[0]);
        break;
    }
  #endif
#else
    if (XE_TRANSFER_DEST_IS_DEPTH != 0u &&
        XE_TRANSFER_DEST_FORMAT == XE_TRANSFER_SOURCE_FORMAT) {
      TransferColorOut out;
      out.color = XeTransferColorOutType(source_depth0);
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
      out.sample_mask = 1u << dest_sample_id;
#endif
      return out;
    }
    if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_D24FS8) {
      bool round_depth = constants.depth_round != 0u;
      packed32 = XeFloat32To20e4(source_depth0 * 2.0f, round_depth);
    } else {
      packed32 = XeRoundToNearestEven(
          clamp(source_depth0, 0.0f, 1.0f) * 16777215.0f);
    }
    if (XE_TRANSFER_DEST_IS_DEPTH == 0u) {
      packed32 = (packed32 << 8u) | (source_stencil0 & 0xFFu);
    }
#endif

    if (!wrote_direct) {
      switch (XE_TRANSFER_DEST_FORMAT) {
        case XE_FMT_8_8_8_8: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
          out_color = float4(
              float((packed32 >> 0u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 8u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 16u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 24u) & 0xFFu) * (1.0f / 255.0f));
#endif
        } break;
        case XE_FMT_8_8_8_8_GAMMA: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
          float4 color = float4(
              float((packed32 >> 0u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 8u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 16u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 24u) & 0xFFu) * (1.0f / 255.0f));
#if XE_GAMMA_RT_AS_UNORM16
          color.rgb = XePWLGammaToLinear3(color.rgb);
#endif
          out_color = color;
#endif
        } break;
        case XE_FMT_2_10_10_10:
        case XE_FMT_2_10_10_10_AS_10_10_10_10: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
          out_color = float4(
              float((packed32 >> 0u) & 0x3FFu) * (1.0f / 1023.0f),
              float((packed32 >> 10u) & 0x3FFu) * (1.0f / 1023.0f),
              float((packed32 >> 20u) & 0x3FFu) * (1.0f / 1023.0f),
              float((packed32 >> 30u) & 0x3u) * (1.0f / 3.0f));
#endif
        } break;
        case XE_FMT_2_10_10_10_FLOAT:
        case XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
          out_color = float4(
              XeFloat7e3To32((packed32 >> 0u) & 0x3FFu),
              XeFloat7e3To32((packed32 >> 10u) & 0x3FFu),
              XeFloat7e3To32((packed32 >> 20u) & 0x3FFu),
              float((packed32 >> 30u) & 0x3u) * (1.0f / 3.0f));
#endif
        } break;
        case XE_FMT_16_16:
        case XE_FMT_16_16_FLOAT: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32 & 0xFFFFu, packed32 >> 16u, 0u, 0u);
#else
          out_color = float4(float(packed32 & 0xFFFFu),
                             float(packed32 >> 16u), 0.0f, 0.0f);
#endif
        } break;
        case XE_FMT_32_FLOAT: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
          out_color = float4(as_type<float>(packed32), 0.0f, 0.0f, 0.0f);
#endif
        } break;
        default:
          break;
      }
    }
  }

  TransferColorOut out;
#if XE_TRANSFER_DEST_COMPONENTS == 1
  out.color = out_color.x;
#elif XE_TRANSFER_DEST_COMPONENTS == 2
  out.color = out_color.xy;
#else
  out.color = out_color;
#endif
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  out.sample_mask = 1u << dest_sample_id;
#endif
  return out;
}
#elif XE_TRANSFER_OUTPUT_DEPTH || XE_TRANSFER_OUTPUT_STENCIL_BIT
struct TransferDepthOut {
  float depth [[depth(any)]];
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  uint sample_mask [[sample_mask]];
#endif
};

fragment TransferDepthOut transfer_ps(
    VSOut in [[stage_in]],
    constant TransferShaderConstants& constants [[buffer(0)]]
    XE_TRANSFER_HOST_DEPTH_BUFFER_PARAM
    XE_TRANSFER_SOURCE_PARAMS
    XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM
    XE_TRANSFER_SAMPLE_ID_PARAM) {
  uint2 dest_pixel = uint2(in.position.xy);
  uint dest_sample_id = 0u;
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  #if XE_TRANSFER_DEST_SAMPLE_ID_FROM_SAMPLE
    dest_sample_id = xe_sample_id;
  #else
    dest_sample_id = constants.dest_sample_id;
  #endif
#endif

  uint tile_width_samples = constants.tile_width_samples;
  uint tile_height_samples = constants.tile_height_samples;
  uint dest_tile_width_pixels = constants.dest_tile_width_pixels;
  uint dest_tile_height_pixels = constants.dest_tile_height_pixels;

  uint dest_tile_pixel_x = 0u;
  uint dest_tile_pixel_y = 0u;
  uint dest_tile_index = 0u;
#if XE_TRANSFER_TILE_INSTANCED
  uint2 tile_origin = uint2(in.tile_origin);
  dest_tile_pixel_x = dest_pixel.x - tile_origin.x;
  dest_tile_pixel_y = dest_pixel.y - tile_origin.y;
  dest_tile_index = in.tile_index;
#else
  uint dest_tile_index_x = 0u;
  uint dest_tile_index_y = 0u;
#if XE_TRANSFER_FAST_DIVMOD
  XeFastDivMod(dest_pixel.x, dest_tile_width_pixels,
               constants.dest_tile_width_pixels_inv, dest_tile_index_x,
               dest_tile_pixel_x);
  XeFastDivMod(dest_pixel.y, dest_tile_height_pixels,
               constants.dest_tile_height_pixels_inv, dest_tile_index_y,
               dest_tile_pixel_y);
#else
  dest_tile_index_x = dest_pixel.x / dest_tile_width_pixels;
  dest_tile_pixel_x = dest_pixel.x % dest_tile_width_pixels;
  dest_tile_index_y = dest_pixel.y / dest_tile_height_pixels;
  dest_tile_pixel_y = dest_pixel.y % dest_tile_height_pixels;
#endif

  dest_tile_index =
      dest_tile_index_x +
      dest_tile_index_y * constants.address.dest_pitch;
#endif

  uint source_sample_id = dest_sample_id;
  uint source_tile_pixel_x = dest_tile_pixel_x;
  uint source_tile_pixel_y = dest_tile_pixel_y;

  bool source_is_64bpp = XE_TRANSFER_SOURCE_IS_64BPP != 0u;
  bool dest_is_64bpp = XE_TRANSFER_DEST_IS_64BPP != 0u;
  uint source_msaa = XE_TRANSFER_SOURCE_MSAA_SAMPLES;
  uint dest_msaa = XE_TRANSFER_DEST_MSAA_SAMPLES;
  bool msaa_2x_supported = constants.msaa_2x_supported != 0u;

  if (!source_is_64bpp && dest_is_64bpp) {
    if (source_msaa >= 4u) {
      if (dest_msaa >= 4u) {
        source_sample_id = dest_sample_id & 2u;
        source_tile_pixel_x =
            XeBitFieldInsert(dest_sample_id, dest_tile_pixel_x, 1u, 31u);
      } else if (dest_msaa == 2u) {
        if (msaa_2x_supported) {
          source_sample_id = (dest_sample_id ^ 1u) << 1u;
        } else {
          source_sample_id = dest_sample_id & 2u;
        }
      } else {
        source_sample_id =
            XeBitFieldInsert(0u, dest_tile_pixel_y, 1u, 1u);
        source_tile_pixel_y = dest_tile_pixel_y >> 1u;
      }
    } else {
      if (dest_msaa >= 4u) {
        source_tile_pixel_x = XeBitFieldInsert(
            dest_tile_pixel_x << 2u, dest_sample_id, 1u, 1u);
      } else {
        source_tile_pixel_x = dest_tile_pixel_x << 1u;
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    if (dest_msaa >= 4u) {
      if (source_msaa >= 4u) {
        source_sample_id =
            XeBitFieldInsert(dest_sample_id, dest_tile_pixel_x, 0u, 1u);
        source_tile_pixel_x = dest_tile_pixel_x >> 1u;
      }
    } else {
      if (source_msaa >= 4u) {
        source_sample_id = XeBitFieldExtract(dest_tile_pixel_x, 1u, 1u);
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            source_sample_id = XeBitFieldInsert(
                source_sample_id, dest_sample_id ^ 1u, 1u, 1u);
          } else {
            source_sample_id = XeBitFieldInsert(
                dest_sample_id, source_sample_id, 0u, 1u);
          }
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, dest_tile_pixel_y, 1u, 1u);
          source_tile_pixel_y = dest_tile_pixel_y >> 1u;
        }
        source_tile_pixel_x = dest_tile_pixel_x >> 2u;
      } else {
        source_tile_pixel_x = dest_tile_pixel_x >> 1u;
      }
    }
  } else {
    if (source_msaa != dest_msaa) {
      if (source_msaa >= 4u) {
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            source_sample_id = XeBitFieldInsert(
                dest_tile_pixel_x, dest_sample_id ^ 1u, 1u, 31u);
          } else {
            source_sample_id = XeBitFieldInsert(
                dest_sample_id, dest_tile_pixel_x, 0u, 1u);
          }
          source_tile_pixel_x = dest_tile_pixel_x >> 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              dest_tile_pixel_x & 1u, dest_tile_pixel_y, 1u, 1u);
          source_tile_pixel_x = dest_tile_pixel_x >> 1u;
          source_tile_pixel_y = dest_tile_pixel_y >> 1u;
        }
      } else if (dest_msaa >= 4u) {
        source_tile_pixel_x = XeBitFieldInsert(
            dest_sample_id, dest_tile_pixel_x, 1u, 31u);
      }
    }
  }

  if (source_msaa < 4u && source_msaa != dest_msaa) {
    if (dest_msaa >= 4u) {
      if (source_msaa == 2u) {
        source_sample_id = dest_sample_id >> 1u;
        if (msaa_2x_supported) {
          source_sample_id ^= 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, source_sample_id, 1u, 1u);
        }
      } else {
        source_tile_pixel_y = XeBitFieldInsert(
            dest_sample_id >> 1u, dest_tile_pixel_y, 1u, 31u);
      }
    } else {
      if (source_msaa == 2u) {
        source_sample_id = dest_tile_pixel_y & 1u;
        if (msaa_2x_supported) {
          source_sample_id ^= 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, source_sample_id, 1u, 1u);
        }
        source_tile_pixel_y = dest_tile_pixel_y >> 1u;
      } else {
        if (msaa_2x_supported) {
          source_tile_pixel_y = XeBitFieldInsert(
              dest_sample_id ^ 1u, dest_tile_pixel_y, 1u, 31u);
        } else {
          source_tile_pixel_y = XeBitFieldInsert(
              dest_sample_id >> 1u, dest_tile_pixel_y, 1u, 31u);
        }
      }
    }
  }

  uint source_pixel_width_dwords_log2 =
      (source_msaa >= 4u ? 1u : 0u) + (source_is_64bpp ? 1u : 0u);

  if ((XE_TRANSFER_SOURCE_IS_DEPTH != 0u) != (XE_TRANSFER_DEST_IS_DEPTH != 0u)) {
    uint source_32bpp_tile_half_pixels =
        tile_width_samples >> (1u + source_pixel_width_dwords_log2);
    if (source_tile_pixel_x < source_32bpp_tile_half_pixels) {
      source_tile_pixel_x += source_32bpp_tile_half_pixels;
    } else {
      source_tile_pixel_x -= source_32bpp_tile_half_pixels;
    }
  }

  uint source_pixel_x = 0u;
  uint source_pixel_y = 0u;
#if XE_TRANSFER_TILE_INSTANCED
  source_pixel_x = in.source_base.x + source_tile_pixel_x;
  source_pixel_y = in.source_base.y + source_tile_pixel_y;
#else
  uint source_tile_index =
      uint(int(dest_tile_index) + constants.address.source_to_dest) &
      (kEdramTileCount - 1u);
  uint source_pitch_tiles = constants.address.source_pitch;
  uint source_tile_index_y = 0u;
  uint source_tile_index_x = 0u;
  XeFastDivMod(source_tile_index, source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_index_y,
               source_tile_index_x);
  source_pixel_x =
      source_tile_index_x *
          (tile_width_samples >> source_pixel_width_dwords_log2) +
      source_tile_pixel_x;
  source_pixel_y =
      source_tile_index_y *
          (tile_height_samples >> (source_msaa >= 2u ? 1u : 0u)) +
      source_tile_pixel_y;
#endif

  bool load_two = !source_is_64bpp && dest_is_64bpp;
  uint source_pixel_x1 = source_pixel_x;
  uint source_sample_id1 = source_sample_id;
  if (load_two) {
    if (source_msaa >= 4u) {
      source_sample_id1 = source_sample_id | 1u;
    } else {
      source_pixel_x1 = source_pixel_x | 1u;
    }
  }

#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
  uint4 source_color0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y),
                              source_sample_id);
#else
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y));
#endif
  uint4 source_color1 = source_color0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1);
#else
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y));
#endif
  }
  #else
  float4 source_color0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y),
                              source_sample_id);
#else
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y));
#endif
  float4 source_color1 = source_color0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1);
#else
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y));
#endif
  }
  #endif
#else
  float source_depth0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source_depth.read(uint2(source_pixel_x, source_pixel_y),
                                    source_sample_id).r;
#else
      xe_transfer_source_depth.read(uint2(source_pixel_x, source_pixel_y)).r;
#endif
  float source_depth1 = source_depth0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_depth1 = xe_transfer_source_depth.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1).r;
#else
    source_depth1 = xe_transfer_source_depth.read(
        uint2(source_pixel_x1, source_pixel_y)).r;
#endif
  }
#if XE_TRANSFER_SOURCE_NEEDS_STENCIL
  uint source_stencil0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source_stencil.read(uint2(source_pixel_x, source_pixel_y),
                                      source_sample_id).r;
#else
      xe_transfer_source_stencil.read(uint2(source_pixel_x, source_pixel_y)).r;
#endif
  uint source_stencil1 = source_stencil0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_stencil1 = xe_transfer_source_stencil.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1).r;
#else
    source_stencil1 = xe_transfer_source_stencil.read(
        uint2(source_pixel_x1, source_pixel_y)).r;
#endif
  }
#else
  uint source_stencil0 = 0u;
  uint source_stencil1 = 0u;
#endif
#endif

  uint packed = 0u;
#if !XE_TRANSFER_OUTPUT_STENCIL_BIT
  bool packed_only_depth = false;
#endif
#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
  switch (XE_TRANSFER_SOURCE_FORMAT) {
    case XE_FMT_16_16:
    case XE_FMT_16_16_FLOAT:
    case XE_FMT_16_16_16_16:
    case XE_FMT_16_16_16_16_FLOAT:
      packed = source_color0[0] | (source_color0[1] << 16u);
      break;
    case XE_FMT_32_FLOAT:
    case XE_FMT_32_32_FLOAT:
      packed = source_color0[0];
      break;
    default:
      packed = source_color0[0];
      break;
  }
  #else
  switch (XE_TRANSFER_SOURCE_FORMAT) {
    case XE_FMT_8_8_8_8:
    case XE_FMT_8_8_8_8_GAMMA: {
      float4 color = source_color0;
      if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_8_8_8_8_GAMMA) {
#if XE_GAMMA_RT_AS_UNORM16 || XE_TRANSFER_OUTPUT_STENCIL_BIT
        color.rgb = XeLinearToPWLGamma3(color.rgb);
#endif
      }
      // Match D3D12: color -> depth transfers derive guest depth from bits
      // 8:31 of the packed 32bpp color word rather than treating depth as a
      // standalone component.
      packed = XePackColorRGBA8(color);
    } break;
    case XE_FMT_2_10_10_10:
    case XE_FMT_2_10_10_10_AS_10_10_10_10: {
      packed = XePackColorRGB10A2(source_color0);
    } break;
    case XE_FMT_2_10_10_10_FLOAT:
    case XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16: {
      packed = XePackColorRGB10A2Float(source_color0);
    } break;
    case XE_FMT_32_FLOAT:
    case XE_FMT_32_32_FLOAT:
      packed = as_type<uint>(source_color0[0]);
      break;
    default:
      packed = as_type<uint>(source_color0[0]);
      break;
  }
  #endif
#else
  if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_D24FS8) {
    bool round_depth = constants.depth_round != 0u;
    packed = XeFloat32To20e4(source_depth0 * 2.0f, round_depth);
  } else {
    packed = XeRoundToNearestEven(
        clamp(source_depth0, 0.0f, 1.0f) * 16777215.0f);
  }
  if (XE_TRANSFER_DEST_IS_DEPTH != 0u) {
#if !XE_TRANSFER_OUTPUT_STENCIL_BIT
    packed_only_depth = true;
#endif
  } else {
    packed = (packed << 8u) | (source_stencil0 & 0xFFu);
  }
#endif
#if XE_TRANSFER_OUTPUT_STENCIL_BIT && !XE_TRANSFER_SOURCE_IS_COLOR
  packed = source_stencil0;
#endif

#if XE_TRANSFER_OUTPUT_STENCIL_BIT
  if (constants.stencil_clear == 0u) {
    if ((packed & constants.stencil_mask) == 0u) {
      discard_fragment();
    }
  }
  TransferDepthOut out;
  out.depth = 0.0f;
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  out.sample_mask = 1u << dest_sample_id;
#endif
  return out;
#else
  uint guest_depth24 = packed;
  if (!packed_only_depth) {
    guest_depth24 = packed >> 8u;
  }

  float host_depth32 = 0.0f;
  bool has_host_depth = false;

#if XE_TRANSFER_HAS_HOST_DEPTH
  if (constants.host_depth_source_is_copy == 0u) {
    uint host_tile_pixel_x = dest_tile_pixel_x;
    uint host_tile_pixel_y = dest_tile_pixel_y;
    uint host_sample_id = dest_sample_id;
    uint host_msaa = XE_TRANSFER_HOST_DEPTH_MSAA_SAMPLES;

    if (host_msaa != dest_msaa) {
      if (host_msaa >= 4u) {
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            host_sample_id = XeBitFieldInsert(
                dest_tile_pixel_x, dest_sample_id ^ 1u, 1u, 31u);
          } else {
            host_sample_id = XeBitFieldInsert(
                dest_sample_id, dest_tile_pixel_x, 0u, 1u);
          }
          host_tile_pixel_x = dest_tile_pixel_x >> 1u;
        } else {
          host_sample_id = XeBitFieldInsert(
              dest_tile_pixel_x & 1u, dest_tile_pixel_y, 1u, 1u);
          host_tile_pixel_x = dest_tile_pixel_x >> 1u;
          host_tile_pixel_y = dest_tile_pixel_y >> 1u;
        }
      } else if (dest_msaa >= 4u) {
        host_tile_pixel_x = XeBitFieldInsert(
            dest_sample_id, dest_tile_pixel_x, 1u, 31u);
      }

      if (host_msaa < 4u) {
        if (dest_msaa >= 4u) {
          if (host_msaa == 2u) {
            host_sample_id = dest_sample_id >> 1u;
            if (msaa_2x_supported) {
              host_sample_id ^= 1u;
            } else {
              host_sample_id = XeBitFieldInsert(
                  host_sample_id, host_sample_id, 1u, 1u);
            }
          } else {
            host_tile_pixel_y = XeBitFieldInsert(
                dest_sample_id >> 1u, dest_tile_pixel_y, 1u, 31u);
          }
        } else {
          if (host_msaa == 2u) {
            host_sample_id = dest_tile_pixel_y & 1u;
            if (msaa_2x_supported) {
              host_sample_id ^= 1u;
            } else {
              host_sample_id = XeBitFieldInsert(
                  host_sample_id, host_sample_id, 1u, 1u);
            }
            host_tile_pixel_y = dest_tile_pixel_y >> 1u;
          } else {
            if (msaa_2x_supported) {
              host_tile_pixel_y = XeBitFieldInsert(
                  dest_sample_id ^ 1u, dest_tile_pixel_y, 1u, 31u);
            } else {
              host_tile_pixel_y = XeBitFieldInsert(
                  dest_sample_id >> 1u, dest_tile_pixel_y, 1u, 31u);
            }
          }
        }
      }
    }

    uint host_pixel_x = 0u;
    uint host_pixel_y = 0u;
#if XE_TRANSFER_TILE_INSTANCED
    host_pixel_x = in.host_base.x + host_tile_pixel_x;
    host_pixel_y = in.host_base.y + host_tile_pixel_y;
#else
    uint host_tile_index =
        uint(int(dest_tile_index) +
             constants.host_depth_address.source_to_dest) &
        (kEdramTileCount - 1u);
    uint host_pitch_tiles = constants.host_depth_address.source_pitch;
    uint host_tile_index_y = 0u;
    uint host_tile_index_x = 0u;
    XeFastDivMod(host_tile_index, host_pitch_tiles,
                 constants.host_depth_source_pitch_tiles_inv,
                 host_tile_index_y, host_tile_index_x);
    host_pixel_x =
        host_tile_index_x *
            (tile_width_samples >> (host_msaa >= 4u ? 1u : 0u)) +
        host_tile_pixel_x;
    host_pixel_y =
        host_tile_index_y *
            (tile_height_samples >> (host_msaa >= 2u ? 1u : 0u)) +
        host_tile_pixel_y;
#endif

#if XE_TRANSFER_HOST_DEPTH_IS_MULTISAMPLE
    host_depth32 = xe_transfer_host_depth.read(
        uint2(host_pixel_x, host_pixel_y), host_sample_id).r;
#else
    host_depth32 =
        xe_transfer_host_depth.read(uint2(host_pixel_x, host_pixel_y)).r;
#endif
  } else {
    uint dest_tile_sample_x = dest_tile_pixel_x;
    uint dest_tile_sample_y = dest_tile_pixel_y;
    if (dest_msaa >= 2u) {
      if (dest_msaa >= 4u) {
        dest_tile_sample_x = XeBitFieldInsert(
            dest_sample_id, dest_tile_pixel_x, 1u, 31u);
      }
      uint vert_sample = 0u;
      if (dest_msaa == 2u && msaa_2x_supported) {
        vert_sample = dest_sample_id ^ 1u;
      } else {
        vert_sample = dest_sample_id >> 1u;
      }
      dest_tile_sample_y = XeBitFieldInsert(
          vert_sample, dest_tile_pixel_y, 1u, 31u);
    }
    uint host_depth_offset =
        (tile_width_samples * tile_height_samples) * dest_tile_index +
        tile_width_samples * dest_tile_sample_y + dest_tile_sample_x;
    host_depth32 =
        as_type<float>(xe_transfer_host_depth_buffer[host_depth_offset]);
  }
  has_host_depth = true;
#endif

  float fragment_depth = 0.0f;
  if (XE_TRANSFER_DEST_FORMAT == XE_FMT_D24FS8) {
    float guest_depth32 = XeFloat20e4To32(guest_depth24, true);
    fragment_depth = guest_depth32;
  } else {
    fragment_depth = XeUnorm24To32(guest_depth24);
  }

  if (has_host_depth) {
    uint host_depth24 = 0u;
    if (XE_TRANSFER_DEST_FORMAT == XE_FMT_D24FS8) {
      bool round_depth = constants.depth_round != 0u;
      host_depth24 = XeFloat32To20e4(host_depth32, round_depth);
    } else {
      host_depth24 = XeRoundToNearestEven(
          clamp(host_depth32, 0.0f, 1.0f) * 16777215.0f);
    }
    if (host_depth24 == guest_depth24) {
      fragment_depth = host_depth32;
    }
  }

  TransferDepthOut out;
  out.depth = fragment_depth;
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  out.sample_mask = 1u << dest_sample_id;
#endif
  return out;
#endif
}
#endif
)METAL";

  source.append(kTransferShaderSource);

  NS::Error* error = nullptr;
  auto src_str = NS::String::string(source.c_str(), NS::UTF8StringEncoding);
  MTL::Library* lib = device_->newLibrary(src_str, nullptr, &error);
  if (!lib) {
    XELOGE(
        "GetOrCreateTransferPipelines: failed to compile transfer MSL "
        "(mode={}): "
        "{}",
        int(key.mode),
        error && error->localizedDescription()
            ? error->localizedDescription()->utf8String()
            : "unknown error");
    return nullptr;
  }

  const char* vs_entry =
      tile_instanced ? "transfer_tile_vs" : "transfer_rect_vs";
  auto vs_name = NS::String::string(vs_entry, NS::UTF8StringEncoding);
  auto ps_name = NS::String::string("transfer_ps", NS::UTF8StringEncoding);
  MTL::Function* vs = lib->newFunction(vs_name);
  MTL::Function* ps = lib->newFunction(ps_name);
  if (!vs || !ps) {
    XELOGE("GetOrCreateTransferPipelines: failed to get transfer shader entry");
    if (vs) {
      vs->release();
    }
    if (ps) {
      ps->release();
    }
    lib->release();
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs);
  desc->setFragmentFunction(ps);

  if (output == TransferOutput::kColor) {
    desc->colorAttachments()->object(0)->setPixelFormat(dest_format);
  } else {
    desc->colorAttachments()->object(0)->setPixelFormat(
        MTL::PixelFormatInvalid);
    desc->setDepthAttachmentPixelFormat(dest_format);
    if (dest_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        dest_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
      desc->setStencilAttachmentPixelFormat(dest_format);
    }
  }

  uint32_t sample_count = 1;
  if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
    sample_count = 2;
  } else if (key.dest_msaa_samples == xenos::MsaaSamples::k4X) {
    sample_count = 4;
  }
  desc->setSampleCount(sample_count);

  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);

  desc->release();
  vs->release();
  ps->release();
  lib->release();

  if (!pipeline) {
    XELOGE(
        "GetOrCreateTransferPipelines: failed to create pipeline (mode={}): {}",
        int(key.mode),
        error && error->localizedDescription()
            ? error->localizedDescription()->utf8String()
            : "unknown error");
    return nullptr;
  }

  pipeline_map.emplace(pipeline_key, pipeline);

  return pipeline;
}

MTL::Library* MetalRenderTargetCache::GetOrCreateTransferLibrary() {
  if (transfer_library_) {
    return transfer_library_;
  }
  static const char kTransferLibrarySource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
  float4 position [[position]];
};

struct TransferClearColorFloatConstants {
  float4 color;
};

struct TransferClearColorUintConstants {
  uint4 color;
};

struct TransferClearDepthConstants {
  float4 depth;
};

vertex VSOut transfer_clear_vs(uint vid [[vertex_id]]) {
  float2 pt = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(pt * 2.0f - 1.0f, 0.0f, 1.0f);
  return out;
}

fragment float4 transfer_clear_color_float_ps(
    VSOut in [[stage_in]],
    constant TransferClearColorFloatConstants& constants [[buffer(0)]]) {
  return constants.color;
}

fragment uint4 transfer_clear_color_uint_ps(
    VSOut in [[stage_in]],
    constant TransferClearColorUintConstants& constants [[buffer(0)]]) {
  return constants.color;
}

struct TransferDepthOut {
  float depth [[depth(any)]];
};

fragment TransferDepthOut transfer_clear_depth_ps(
    VSOut in [[stage_in]],
    constant TransferClearDepthConstants& constants [[buffer(0)]]) {
  TransferDepthOut out;
  out.depth = constants.depth.x;
  return out;
}
)METAL";

  NS::Error* error = nullptr;
  auto source_str =
      NS::String::string(kTransferLibrarySource, NS::UTF8StringEncoding);
  transfer_library_ = device_->newLibrary(source_str, nullptr, &error);
  if (!transfer_library_) {
    XELOGE("GetOrCreateTransferLibrary: failed to compile transfer library: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
  }
  return transfer_library_;
}

MTL::RenderPipelineState*
MetalRenderTargetCache::GetOrCreateTransferClearPipeline(
    MTL::PixelFormat dest_format, bool dest_is_uint, bool is_depth,
    uint32_t sample_count) {
  uint32_t key = uint32_t(dest_format);
  key ^= (sample_count & 0x7u) << 24;
  if (dest_is_uint) {
    key ^= 1u << 30;
  }
  if (is_depth) {
    key ^= 1u << 31;
  }
  auto it = transfer_clear_pipelines_.find(key);
  if (it != transfer_clear_pipelines_.end()) {
    return it->second;
  }

  MTL::Library* lib = GetOrCreateTransferLibrary();
  if (!lib) {
    return nullptr;
  }

  auto vs_name =
      NS::String::string("transfer_clear_vs", NS::UTF8StringEncoding);
  const char* ps_name_cstr = nullptr;
  if (is_depth) {
    ps_name_cstr = "transfer_clear_depth_ps";
  } else {
    ps_name_cstr = dest_is_uint ? "transfer_clear_color_uint_ps"
                                : "transfer_clear_color_float_ps";
  }
  auto ps_name = NS::String::string(ps_name_cstr, NS::UTF8StringEncoding);

  MTL::Function* vs = lib->newFunction(vs_name);
  MTL::Function* ps = lib->newFunction(ps_name);
  if (!vs || !ps) {
    XELOGE(
        "GetOrCreateTransferClearPipeline: missing transfer clear functions");
    if (vs) {
      vs->release();
    }
    if (ps) {
      ps->release();
    }
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs);
  desc->setFragmentFunction(ps);
  desc->setSampleCount(sample_count ? sample_count : 1);

  if (is_depth) {
    desc->colorAttachments()->object(0)->setPixelFormat(
        MTL::PixelFormatInvalid);
    desc->setDepthAttachmentPixelFormat(dest_format);
    if (dest_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        dest_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
      desc->setStencilAttachmentPixelFormat(dest_format);
    }
  } else {
    desc->colorAttachments()->object(0)->setPixelFormat(dest_format);
  }

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);

  desc->release();
  vs->release();
  ps->release();

  if (!pipeline) {
    XELOGE("GetOrCreateTransferClearPipeline: failed to create pipeline: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
    return nullptr;
  }

  transfer_clear_pipelines_.emplace(key, pipeline);
  return pipeline;
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyTexture(
    MTL::PixelFormat format, uint32_t sample_count) {
  if (!device_) {
    return nullptr;
  }
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(1);
  desc->setHeight(1);
  desc->setPixelFormat(format);
  desc->setTextureType(sample_count > 1 ? MTL::TextureType2DMultisample
                                        : MTL::TextureType2D);
  desc->setSampleCount(sample_count ? sample_count : 1);
  MTL::TextureUsage usage = MTL::TextureUsageShaderRead;
  if (format == MTL::PixelFormatDepth32Float_Stencil8 ||
      format == MTL::PixelFormatDepth24Unorm_Stencil8) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);
  desc->setStorageMode(MTL::StorageModePrivate);
  MTL::Texture* tex = nullptr;
  if (render_target_heap_pool_) {
    tex = render_target_heap_pool_->CreateTexture(desc);
  }
  if (!tex) {
    tex = device_->newTexture(desc);
  }
  desc->release();
  return tex;
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyColorFloatTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_color_float_[index]) {
    transfer_dummy_color_float_[index] =
        GetTransferDummyTexture(MTL::PixelFormatRGBA8Unorm, sample_count);
  }
  return transfer_dummy_color_float_[index];
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyColorUintTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_color_uint_[index]) {
    transfer_dummy_color_uint_[index] =
        GetTransferDummyTexture(MTL::PixelFormatRGBA8Uint, sample_count);
  }
  return transfer_dummy_color_uint_[index];
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyDepthTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_depth_[index]) {
    transfer_dummy_depth_[index] = GetTransferDummyTexture(
        MTL::PixelFormatDepth32Float_Stencil8, sample_count);
  }
  return transfer_dummy_depth_[index];
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyStencilTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_stencil_[index]) {
    MTL::Texture* depth_tex = GetTransferDummyDepthTexture(sample_count);
    if (!depth_tex) {
      return nullptr;
    }
    transfer_dummy_stencil_[index] =
        depth_tex->newTextureView(MTL::PixelFormatX32_Stencil8);
  }
  return transfer_dummy_stencil_[index];
}

MTL::Buffer* MetalRenderTargetCache::GetTransferDummyBuffer() {
  if (!transfer_dummy_buffer_ && device_) {
    transfer_dummy_buffer_ =
        device_->newBuffer(sizeof(uint32_t), MTL::ResourceStorageModeShared);
    if (transfer_dummy_buffer_) {
    }
    if (transfer_dummy_buffer_) {
      std::memset(transfer_dummy_buffer_->contents(), 0, sizeof(uint32_t));
    }
  }
  return transfer_dummy_buffer_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferDepthStencilState(
    bool depth_write) {
  if (transfer_depth_state_) {
    return transfer_depth_state_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(::cvars::depth_transfer_not_equal_test
                                    ? MTL::CompareFunctionNotEqual
                                    : MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(depth_write);
  transfer_depth_state_ = device_->newDepthStencilState(desc);
  desc->release();
  return transfer_depth_state_;
}

MTL::DepthStencilState*
MetalRenderTargetCache::GetTransferNoDepthStencilState() {
  if (transfer_depth_state_none_) {
    return transfer_depth_state_none_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);
  transfer_depth_state_none_ = device_->newDepthStencilState(desc);
  desc->release();
  return transfer_depth_state_none_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferDepthClearState() {
  if (transfer_depth_clear_state_) {
    return transfer_depth_clear_state_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(true);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(0xFF);
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_depth_clear_state_ = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_depth_clear_state_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferStencilClearState() {
  if (transfer_stencil_clear_state_) {
    return transfer_stencil_clear_state_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(0xFF);
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_stencil_clear_state_ = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_stencil_clear_state_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferStencilBitState(
    uint32_t bit) {
  if (bit >= 8) {
    return nullptr;
  }
  if (transfer_stencil_bit_states_[bit]) {
    return transfer_stencil_bit_states_[bit];
  }
  uint32_t mask = uint32_t(1) << bit;
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(uint32_t(mask));
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_stencil_bit_states_[bit] = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_stencil_bit_states_[bit];
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
