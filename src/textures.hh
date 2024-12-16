// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkTileMode.h>
#include <include/gpu/graphite/ImageProvider.h>

#include "virtual_fs.hh"

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

  constexpr static MakeArgs kDefaultArgs = {.width = 0,
                                            .height = 0,
                                            .scale = 0,
                                            .tile_x = SkTileMode::kClamp,
                                            .tile_y = SkTileMode::kClamp,
                                            .raw_shader = false};

  static PersistentImage MakeFromSkImage(sk_sp<SkImage> image, MakeArgs = kDefaultArgs);

  static PersistentImage MakeFromAsset(maf::fs::VFile& asset, MakeArgs = kDefaultArgs);

  void draw(SkCanvas&);
};

sk_sp<SkImage> DecodeImage(maf::fs::VFile& asset);

constexpr static SkSamplingOptions kDefaultSamplingOptions =
    SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);

struct AutomatImageProvider : public skgpu::graphite::ImageProvider {
  std::unordered_map<uint32_t, sk_sp<SkImage>> cache;
  sk_sp<SkImage> findOrCreate(skgpu::graphite::Recorder* recorder, const SkImage* image,
                              SkImage::RequiredProperties) override;
};

extern sk_sp<skgpu::graphite::ImageProvider> image_provider;

}  // namespace automat