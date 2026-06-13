// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

// Vulkan interface.

#include <include/core/SkCanvas.h>
#include <include/core/SkRefCnt.h>
#include <include/gpu/graphite/BackendSemaphore.h>
#include <include/gpu/graphite/Context.h>
#include <include/gpu/vk/VulkanMemoryAllocator.h>
#include <vulkan/vulkan_core.h>

#include "status.hpp"

namespace automat::vk {

constexpr int cfg_MSAASampleCount = 1;
constexpr bool cfg_DisableVsync = true;
constexpr bool kDebugVulkanMemory = false;

extern bool initialized;
extern std::unique_ptr<skgpu::graphite::Context> graphite_context, background_context;

void Init(Status&);
void Destroy();

void Resize(int width_hint, int height_hint, Status&);

SkCanvas* AcquireCanvas();
void Present();

// Reports basic memory stats through Tracy plots.
// Enable kDebugVulkanMemory for even more plots.
void ReportMemoryStats();

// RAII wrapper for VkSemaphore.
struct Semaphore {
  VkSemaphore vk_semaphore = VK_NULL_HANDLE;

  void Create(Status&);
  void Destroy();

  // Constructs a null semaphore.
  Semaphore() = default;

  // Constructs & initializes a semaphore.
  Semaphore(Status&);

  // No copies
  Semaphore(const Semaphore&) = delete;
  Semaphore& operator=(const Semaphore&) = delete;

  // Moving is allowed
  Semaphore(Semaphore&& other) : vk_semaphore(other.vk_semaphore) {
    other.vk_semaphore = VK_NULL_HANDLE;
  }
  Semaphore& operator=(Semaphore&& other);

  ~Semaphore();

  operator bool() { return vk_semaphore != VK_NULL_HANDLE; }
  operator VkSemaphore() { return vk_semaphore; }
  operator skgpu::graphite::BackendSemaphore();
};

}  // namespace automat::vk