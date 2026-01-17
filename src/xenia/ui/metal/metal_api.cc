/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// This file provides the single implementation point for metal-cpp to avoid
// duplicate symbol errors. It should be linked by all Metal backend files
// that need Metal API access.

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "third_party/metal-cpp/Foundation/Foundation.hpp"
#include "third_party/metal-cpp/Metal/Metal.hpp"
#include "third_party/metal-cpp/QuartzCore/QuartzCore.hpp"
