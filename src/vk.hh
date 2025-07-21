// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

// Vulkan interface.

#include <include/core/SkCanvas.h>
#include <include/gpu/graphite/BackendSemaphore.h>
#include <include/gpu/graphite/Context.h>

#include "status.hh"

namespace automat::vk {

constexpr int cfg_MSAASampleCount = 1;
constexpr bool cfg_DisableVsync = true;

extern bool initialized;
extern std::unique_ptr<skgpu::graphite::Context> graphite_context, background_context;

void Init(Status&);
void Destroy();

skgpu::graphite::BackendSemaphore CreateSemaphore();
void DestroySemaphore(const skgpu::graphite::BackendSemaphore&);

void Resize(int width_hint, int height_hint, Status& status);

SkCanvas* GetBackbufferCanvas();
void Present();

}  // namespace automat::vk