/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/register_file.h"
#include <array>

#include "xenia/base/math.h"

namespace xe {
namespace gpu {

RegisterFile::RegisterFile() {
  std::memset(values, 0, sizeof(values));

  // Several context registers power up with non-zero values on real Xenos
  // hardware, so a title that reads one of them before writing it would
  // otherwise observe 0. These are the hardware reset defaults, corroborated by
  // the AMD R6xx/R7xx 3D register reference and the Mesa r600g driver, which
  // programs the same registers to the same values at context init.
  values[XE_GPU_REG_VGT_MAX_VTX_INDX] = 0x0000FFFF;
  values[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] = 0x0000FFFF;
  values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 0x20002000;  // 8192 x 8192
  values[XE_GPU_REG_RB_STENCILREFMASK_BF] = 0x00FFFF00;
  values[XE_GPU_REG_PA_SU_POINT_SIZE] = 0x00080008;
  values[XE_GPU_REG_PA_SU_POINT_MINMAX] = 0x04000010;
  values[XE_GPU_REG_PA_SU_LINE_CNTL] = 0x00000008;
  values[XE_GPU_REG_PA_SC_LINE_CNTL] = 0x00000400;
  values[XE_GPU_REG_VGT_HOS_REUSE_DEPTH] = 0x0000000E;
  values[XE_GPU_REG_VGT_VERTEX_REUSE_BLOCK_CNTL] = 0x0000000E;
  values[XE_GPU_REG_VGT_OUT_DEALLOC_CNTL] = 0x00000010;
  // Guard-band clip/discard adjust (2.0 clip, 1.0 discard); hardwired on
  // hardware.
  values[XE_GPU_REG_PA_CL_GB_VERT_CLIP_ADJ] = 0x40000000;  // 2.0f
  values[XE_GPU_REG_PA_CL_GB_VERT_DISC_ADJ] = 0x3F800000;  // 1.0f
  values[XE_GPU_REG_PA_CL_GB_HORZ_CLIP_ADJ] = 0x40000000;  // 2.0f
  values[XE_GPU_REG_PA_CL_GB_HORZ_DISC_ADJ] = 0x3F800000;  // 1.0f
  values[XE_GPU_REG_PA_SC_AA_MASK] = 0x0000FFFF;
  // Tessellation levels also reset to 1.0f on hardware, but the backends apply
  // the effective factor as (register + 1.0f), so initializing them here would
  // change the effective reset factor; left out for separate review.
  // values[XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL] = 0x3F800000;  // 1.0f
  // values[XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL] = 0x3F800000;  // 1.0f
}
constexpr unsigned int GetHighestRegisterNumber() {
  uint32_t highest = 0;
#define XE_GPU_REGISTER(index, type, name) \
  highest = std::max<uint32_t>(highest, index);
#include "xenia/gpu/register_table.inc"
#undef XE_GPU_REGISTER

  return highest;
}
constexpr unsigned int GetLowestRegisterNumber() {
  uint32_t lowest = UINT_MAX;
#define XE_GPU_REGISTER(index, type, name) \
  lowest = std::min<uint32_t>(lowest, index);
#include "xenia/gpu/register_table.inc"
#undef XE_GPU_REGISTER

  return lowest;
}

static constexpr uint32_t lowest_register = GetLowestRegisterNumber();
static constexpr uint32_t highest_register = GetHighestRegisterNumber();

static constexpr uint32_t total_num_registers =
    highest_register - lowest_register;

static constexpr uint32_t num_required_words_for_registers =
    ((total_num_registers + 63) & ~63) / 64;
// can't use bitset, its not constexpr in c++ 17
using ValidRegisterBitset = std::array<
    uint64_t,
    num_required_words_for_registers>;  // std::bitset<highest_register
                                        // - lowest_register>;

static constexpr ValidRegisterBitset BuildValidRegisterBitset() {
  ValidRegisterBitset result{};
#define XE_GPU_REGISTER(index, type, name)  \
  result[(index - lowest_register) / 64] |= \
      1ULL << ((index - lowest_register) % 64);

#include "xenia/gpu/register_table.inc"
#undef XE_GPU_REGISTER

  return result;
}
static constexpr ValidRegisterBitset valid_register_bitset =
    BuildValidRegisterBitset();

const RegisterInfo* RegisterFile::GetRegisterInfo(uint32_t index) {
  switch (index) {
#define XE_GPU_REGISTER(index, type, name) \
  case index: {                            \
    static const RegisterInfo reg_info = { \
        RegisterInfo::Type::type,          \
        #name,                             \
    };                                     \
    return &reg_info;                      \
  }
#include "xenia/gpu/register_table.inc"
#undef XE_GPU_REGISTER
    default:
      return nullptr;
  }
}
/*
        todo: this still uses a lot of cpu! our bitset is too large
*/
bool RegisterFile::IsValidRegister(uint32_t index) {
  if (XE_UNLIKELY(index < lowest_register) ||
      XE_UNLIKELY(index > highest_register)) {
    return false;
  }
  uint32_t register_linear_index = index - lowest_register;

  return (valid_register_bitset[register_linear_index / 64] &
          (1ULL << (register_linear_index % 64))) != 0;
}
}  //  namespace gpu
}  //  namespace xe
