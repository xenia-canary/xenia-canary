/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_TOML_FWD_H_
#define XENIA_BASE_TOML_FWD_H_

// toml++ is a large header-only library, so headers that only pass its types
// around by pointer or reference use these declarations instead of including
// it. They must name the inline namespace toml++ declares its types in -
// cvar.cc checks TOML_LIB_MAJOR against it.
namespace toml {
inline namespace v3 {
class node;
class table;
}  // namespace v3
}  // namespace toml

#endif  // XENIA_BASE_TOML_FWD_H_
