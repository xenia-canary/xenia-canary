/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_CODEC_STRB_H_
#define XENIA_KERNEL_XAM_AVATAR_CODEC_STRB_H_

#include <cstdint>
#include <optional>
#include <span>

namespace xe::kernel::xam::avatar::codec {

// Returns the first matching block after checking all block boundaries.
// The span borrows the input. This reads a generic STRB envelope; it does not
// verify a YTGR signature or establish the identity or type of an asset.
std::optional<std::span<const uint8_t>> FindStrbBlock(
    std::span<const uint8_t> bytes, uint64_t wanted_id);

}  // namespace xe::kernel::xam::avatar::codec

#endif  // XENIA_KERNEL_XAM_AVATAR_CODEC_STRB_H_
