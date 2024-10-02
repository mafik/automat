// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "textures.hh"

#include <include/core/SkData.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/GrDirectContext.h>
#include <include/gpu/ganesh/SkImageGanesh.h>

#include "animation.hh"
#include "widget.hh"

namespace automat {

struct ImageCache {
  std::unordered_map<maf::Str, sk_sp<SkImage>> images;
};

animation::PerDisplay<ImageCache> image_cache;

sk_sp<SkImage> CacheImage(gui::DrawContext& ctx, const maf::Str& key,
                          std::function<sk_sp<SkImage>()> generator) {
  auto& cache = image_cache[ctx.display];
  if (auto it = cache.images.find(key); it != cache.images.end()) {
    return it->second;
  }
  auto image = generator();
  cache.images[key] = image;
  return image;
}

sk_sp<SkImage> MakeImageFromAsset(maf::fs::VFile& asset, gui::DrawContext* dctx) {
  if (dctx) {
    auto& cache = image_cache[dctx->display];
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
}  // namespace automat