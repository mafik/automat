// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSamplingOptions.h>
#include <include/gpu/GrDirectContext.h>

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

  // The image will be initialized with the given width and height. Missing arguments will be
  // initialized to make the image proportional. If both arguments are missing, the image will
  // assume 300 DPI.
  //
  // Image will never be stretched. The smaller dimension will be used to calculate the scale.
  static PersistentImage MakeFromAsset(maf::fs::VFile& asset, float width = 0, float height = 0);

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