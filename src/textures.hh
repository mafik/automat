// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSamplingOptions.h>
#include <include/gpu/GrDirectContext.h>

#include "include/core/SkTileMode.h"
#include "virtual_fs.hh"
#include "widget.hh"

namespace automat {

// Solves the SkImage destruction problem by releasing the image at Automat's shutdown.
struct PersistentImage {
  std::optional<sk_sp<SkImage>> image;
  std::optional<sk_sp<SkShader>> shader;
  SkPaint paint;
  float scale;

  int widthPx();
  int heightPx();

  float width();
  float height();

  // Defines how the Image will be mapped to local coordinate space.
  //
  // Automat uses metric coordinates while images use pixels.
  //
  // Specify at most one of [width, height, scale]. The other values will be calculated
  // automatically. When no values are specified, the image will be displayed at 300 DPI.
  struct MakeArgs {
    float width = 0;
    float height = 0;
    float scale = 0;
    SkTileMode tile_x = SkTileMode::kClamp;
    SkTileMode tile_y = SkTileMode::kClamp;
    bool raw_shader = false;  // raw shaders don't apply gamma correction
  };

  static PersistentImage MakeFromAsset(maf::fs::VFile& asset, MakeArgs = {
                                                                  .width = 0,
                                                                  .height = 0,
                                                                  .scale = 0,
                                                                  .tile_x = SkTileMode::kClamp,
                                                                  .tile_y = SkTileMode::kClamp,
                                                                  .raw_shader = false,
                                                              });

  // SkImage& operator*() const { return **image; }
  // SkImage* operator->() const { return image->get(); }
  // operator SkImage*() { return image->get(); }

  void draw(SkCanvas&);
};

// Pass non-null DrawContext to create GPU-backed image (MUCH cheaper to draw).
sk_sp<SkImage> MakeImageFromAsset(maf::fs::VFile& asset, gui::DrawContext*);

sk_sp<SkImage> CacheImage(gui::DrawContext& ctx, const maf::Str& key,
                          std::function<sk_sp<SkImage>()> generator);

constexpr static SkSamplingOptions kDefaultSamplingOptions =
    SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);

}  // namespace automat