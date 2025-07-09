// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkTileMode.h>
#include <include/gpu/graphite/ImageProvider.h>

#include "time.hh"
#include "virtual_fs.hh"

namespace automat {

constexpr static SkSamplingOptions kDefaultSamplingOptions =
    SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);

constexpr static SkSamplingOptions kFastSamplingOptions =
    SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNone);

constexpr static SkSamplingOptions kNearestMipmapSamplingOptions =
    SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kLinear);

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
    SkSamplingOptions sampling_options = kDefaultSamplingOptions;
  };

  constexpr static MakeArgs kDefaultArgs = {.width = 0,
                                            .height = 0,
                                            .scale = 0,
                                            .tile_x = SkTileMode::kClamp,
                                            .tile_y = SkTileMode::kClamp,
                                            .raw_shader = false,
                                            .sampling_options = kDefaultSamplingOptions};

  static PersistentImage MakeFromSkImage(sk_sp<SkImage> image, MakeArgs = kDefaultArgs);

  static PersistentImage MakeFromAsset(fs::VFile& asset, MakeArgs = kDefaultArgs);

  void draw(SkCanvas&);
};

sk_sp<SkImage> DecodeImage(fs::VFile& asset);

// Caching strategy:
// - If an image was used during the last frame, keep it in the cache
// - Clear the cache until it's below 1GB
// - Start removing images from the oldest ones
struct AutomatImageProvider : public skgpu::graphite::ImageProvider {
  struct CacheEntry {
    sk_sp<SkImage> image;
    time::SteadyPoint last_used;
  };
  std::unordered_map<uint32_t, CacheEntry> cache;
  time::SteadyPoint last_tick = time::kZeroSteady;

  sk_sp<SkImage> findOrCreate(skgpu::graphite::Recorder* recorder, const SkImage* image,
                              SkImage::RequiredProperties) override;

  // Called at the end of each frame to clear unused textures.
  void TickCache();
};

extern sk_sp<skgpu::graphite::ImageProvider> image_provider;

}  // namespace automat