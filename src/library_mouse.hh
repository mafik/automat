#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

// This file contains shared code for mouse-related objects.

#include <include/effects/SkRuntimeEffect.h>

#include "textures.hh"

namespace automat::library::mouse {

constexpr float kTextureScale = 0.00005;

extern PersistentImage base_texture;

SkRuntimeEffect& GetPixelGridRuntimeEffect();

}  // namespace automat::library::mouse