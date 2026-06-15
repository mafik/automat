#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <unistd.h>

#include <cstdint>
#include <utility>

namespace automat {

// TODO: Move DMA logic into dma.hpp (DmabufImage, ImportDmabufImage)

// A client-provided dmabuf frame: everything needed to import the shared buffer
// as a GPU texture. Owns its plane file descriptors and closes them on
// destruction, so it can be dropped anywhere without leaking. The import takes
// the fds it consumes (Vulkan owns an fd after a successful import) by clearing
// those slots. Move-only so fd ownership is never ambiguous.
struct DmabufImage {
  static constexpr int kMaxPlanes = 4;
  int width = 0;
  int height = 0;
  uint32_t drm_format = 0;  // DRM fourcc, e.g. DRM_FORMAT_ARGB8888
  uint64_t modifier = 0;
  bool opaque = false;    // XRGB (alpha forced opaque) vs ARGB (premultiplied)
  bool y_invert = false;  // content rows run bottom-to-top
  int plane_count = 0;
  int fds[kMaxPlanes] = {-1, -1, -1, -1};  // TODO: use FD from fd.hpp
  uint32_t offsets[kMaxPlanes] = {0, 0, 0, 0};
  uint32_t strides[kMaxPlanes] = {0, 0, 0, 0};

  DmabufImage() = default;
  DmabufImage(const DmabufImage&) = delete;
  DmabufImage& operator=(const DmabufImage&) = delete;
  DmabufImage(DmabufImage&& other) { *this = std::move(other); }
  DmabufImage& operator=(DmabufImage&& other) {
    if (this != &other) {
      CloseFds();
      width = other.width;
      height = other.height;
      drm_format = other.drm_format;
      modifier = other.modifier;
      opaque = other.opaque;
      y_invert = other.y_invert;
      plane_count = other.plane_count;
      for (int i = 0; i < kMaxPlanes; ++i) {
        fds[i] = other.fds[i];
        offsets[i] = other.offsets[i];
        strides[i] = other.strides[i];
        other.fds[i] = -1;
      }
      other.plane_count = 0;
    }
    return *this;
  }
  ~DmabufImage() { CloseFds(); }

  void CloseFds() {
    for (int i = 0; i < plane_count; ++i) {
      if (fds[i] >= 0) {
        ::close(fds[i]);
        fds[i] = -1;
      }
    }
  }
};

}  // namespace automat
