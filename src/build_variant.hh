// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#pragma maf add debug compile argument "-DAUTOMAT_BUILD_VARIANT=1"
#pragma maf add release compile argument "-DAUTOMAT_BUILD_VARIANT=2"
#pragma maf add fast compile argument "-DAUTOMAT_BUILD_VARIANT=3"

namespace automat::build_variant {

// Build variant constants - values set by the build system via preprocessor defines
#if AUTOMAT_BUILD_VARIANT == 1
constexpr bool Debug = true;
constexpr bool Release = false;
constexpr bool Fast = false;
#elif AUTOMAT_BUILD_VARIANT == 2
constexpr bool Debug = false;
constexpr bool Release = true;
constexpr bool Fast = false;
#elif AUTOMAT_BUILD_VARIANT == 3
constexpr bool Debug = false;
constexpr bool Release = false;
constexpr bool Fast = true;
#else
#warning "AUTOMAT_BUILD_VARIANT should be set to 1, 2, or 3"
constexpr bool Debug = false;
constexpr bool Release = false;
constexpr bool Fast = false;
#endif

constexpr bool NotDebug = !Debug;
constexpr bool NotRelease = !Release;
constexpr bool NotFast = !Fast;

}  // namespace automat::build_variant