// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

// Vulkan interface.

#include <include/core/SkCanvas.h>

namespace automat::vk {

constexpr int cfg_MSAASampleCount = 4;
constexpr bool cfg_DisableVsync = true;

extern bool initialized;

std::string Init();
void Destroy();

std::string Resize(int width_hint, int height_hint);

SkCanvas* GetBackbufferCanvas();
void Present();

}  // namespace automat::vk