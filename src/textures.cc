// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "textures.hh"

#include <include/core/SkData.h>
#include <include/core/SkShader.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/GrDirectContext.h>
#include <include/gpu/ganesh/SkImageGanesh.h>

#include "include/core/SkMatrix.h"
#include "log.hh"
#include "widget.hh"

namespace automat {

struct ImageCache {
  std::unordered_map<maf::Str, sk_sp<SkImage>> images;
};

ImageCache image_cache;

sk_sp<SkImage> CacheImage(gui::DrawContext& ctx, const maf::Str& key,
                          std::function<sk_sp<SkImage>()> generator) {
  auto& cache = image_cache;
  if (auto it = cache.images.find(key); it != cache.images.end()) {
    return it->second;
  }
  auto image = generator();
  cache.images[key] = image;
  return image;
}

sk_sp<SkImage> MakeImageFromAsset(maf::fs::VFile& asset, gui::DrawContext* dctx) {
  if (dctx) {
    auto& cache = image_cache;
    if (auto it = cache.images.find(maf::Str(asset.path)); it != cache.images.end()) {
      return it->second;
    }
  }
  auto& content = asset.content;
  auto data = SkData::MakeWithoutCopy(content.data(), content.size());
  auto image = SkImages::DeferredFromEncodedData(data);
  if (dctx) {
    if (auto gr_ctx = (GrDirectContext*)*dctx) {
      image = image->withDefaultMipmaps();
      image = SkImages::TextureFromImage(gr_ctx, image.get(), skgpu::Mipmapped::kYes);
    }
  }
  return image;
}
PersistentImage PersistentImage::MakeFromAsset(maf::fs::VFile& asset, MakeArgs args) {
  auto& content = asset.content;
  auto data = SkData::MakeWithoutCopy(content.data(), content.size());
  auto image = SkImages::DeferredFromEncodedData(data);

  float scale, width, height;
  if (args.scale) {
    scale = args.scale;
    width = image->width() * scale;
    height = image->height() * scale;
  } else if (args.width) {
    scale = args.width / image->width();
    width = args.width;
    height = image->height() * scale;
  } else if (args.height) {
    scale = args.height / image->height();
    width = image->width() * scale;
    height = args.height;
  } else {
    scale = 1 / 300.f * 25.4f;
    width = image->width() * scale;
    height = image->height() * scale;
  }
  auto matrix = SkMatrix::Scale(scale, -scale).postTranslate(0, height);
  auto shader =
      args.raw_shader
          ? image->makeRawShader(args.tile_x, args.tile_y, kDefaultSamplingOptions, matrix)
          : image->makeShader(args.tile_x, args.tile_y, kDefaultSamplingOptions, matrix);
  SkPaint paint;
  paint.setShader(shader);
  return PersistentImage{
      .image = image,
      .shader = shader,
      .paint = paint,
      .scale = scale,
  };
}

int PersistentImage::widthPx() { return (*image)->width(); }
int PersistentImage::heightPx() { return (*image)->height(); }

float PersistentImage::width() { return widthPx() * scale; }
float PersistentImage::height() { return heightPx() * scale; }

void PersistentImage::draw(SkCanvas& canvas) {
  if (!image) {
    ERROR << "Attempt to draw an uninitialized PersistentImage";
  }

  Rect rect = Rect(0, 0, width(), height());
  canvas.drawRect(rect, paint);
}
}  // namespace automat