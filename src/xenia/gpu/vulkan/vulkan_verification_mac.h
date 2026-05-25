/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_VERIFICATION_MAC_H_
#define XENIA_GPU_VULKAN_VULKAN_VERIFICATION_MAC_H_

namespace xe {
namespace ui {
namespace vulkan {
class VulkanDevice;
}  // namespace vulkan
}  // namespace ui

namespace gpu {
namespace vulkan {
void VerifyVulkanSubmodules(const ui::vulkan::VulkanDevice* device);
}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_VERIFICATION_MAC_H_
