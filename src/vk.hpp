#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

// Vulkan interface.

#include <include/core/SkCanvas.h>
#include <include/core/SkRefCnt.h>
#include <include/gpu/graphite/BackendSemaphore.h>
#include <include/gpu/graphite/Context.h>
#include <include/gpu/vk/VulkanMemoryAllocator.h>
#include <vulkan/vulkan_core.h>

#include "dmabuf.hpp"
#include "status.hpp"

class SkImage;

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

// Imports a client dmabuf as a GPU-backed image. Tries a zero-copy Vulkan
// external-memory import and falls back to a mapped upload for drivers that
// cannot import the buffer directly. Takes desc by value and closes its plane
// fds when done. Runs on the compositor thread, which exclusively owns the
// import recorder. Returns null on failure.
sk_sp<SkImage> ImportDmabuf(DmabufImage desc);

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