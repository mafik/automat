// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "library_mouse.hh"

#include <include/core/SkShader.h>

#include "embedded.hh"
#include "global_resources.hh"
#include "log.hh"

namespace automat::library::mouse {

PersistentImage base_texture = PersistentImage::MakeFromAsset(
    embedded::assets_mouse_base_webp, PersistentImage::MakeArgs{.scale = kTextureScale});

SkRuntimeEffect& GetPixelGridRuntimeEffect() {
  static const auto runtime_effect = []() {
    Status status;
    auto runtime_effect = resources::CompileShader(embedded::assets_pixel_grid_rt_sksl, status);
    if (!OK(status)) {
      FATAL << status;
    }
    return runtime_effect;
  }();
  return *runtime_effect;
}

}  // namespace automat::library::mouse