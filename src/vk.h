#pragma once

// Vulkan interface.

#include <include/core/SkCanvas.h>

namespace automat::vk {

constexpr int cfg_MSAASampleCount = 1;  // TODO: figure out why only 1 works
constexpr bool cfg_DisableVsync = true;

std::string Init();
void Destroy();

std::string Resize(int width_hint, int height_hint);

SkCanvas* GetBackbufferCanvas();
void Present();

}  // namespace automat::vk