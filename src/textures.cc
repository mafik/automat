// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "textures.hh"

#include <include/core/SkData.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkShader.h>
#include <include/gpu/graphite/Image.h>

#include "log.hh"
#include "math.hh"
#include "time.hh"

namespace automat {

sk_sp<SkImage> DecodeImage(fs::VFile& asset) {
  auto& content = asset.content;
  auto data = SkData::MakeWithoutCopy(content.data(), content.size());
  return SkImages::DeferredFromEncodedData(data);
}

PersistentImage PersistentImage::MakeFromSkImage(sk_sp<SkImage> image, MakeArgs args) {
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
    scale = 0.0254f /* meters / inch */ / 300.f /* pixels / inch */;
    width = image->width() * scale;
    height = image->height() * scale;
  }
  auto matrix = SkMatrix::Scale(scale, -scale).postTranslate(0, height);
  auto shader = args.raw_shader
                    ? image->makeRawShader(args.tile_x, args.tile_y, args.sampling_options, matrix)
                    : image->makeShader(args.tile_x, args.tile_y, args.sampling_options, matrix);
  SkPaint paint;
  paint.setShader(shader);
  return PersistentImage{
      .image = image,
      .shader = shader,
      .paint = paint,
      .scale = scale,
  };
}

PersistentImage PersistentImage::MakeFromAsset(fs::VFile& asset, MakeArgs args) {
  return MakeFromSkImage(DecodeImage(asset), args);
}

int PersistentImage::widthPx() { return (*image)->width(); }
int PersistentImage::heightPx() { return (*image)->height(); }

float PersistentImage::width() { return widthPx() * scale; }
float PersistentImage::height() { return heightPx() * scale; }

void PersistentImage::draw(SkCanvas& canvas) {
  if (!image) {
    ERROR << "Attempt to draw an uninitialized PersistentImage";
    return;
  }

  Rect rect = Rect(0, 0, width(), height());
  canvas.drawRect(rect, paint);
}

sk_sp<skgpu::graphite::ImageProvider> image_provider;

sk_sp<SkImage> AutomatImageProvider::findOrCreate(skgpu::graphite::Recorder* recorder,
                                                  const SkImage* image,
                                                  SkImage::RequiredProperties props) {
  auto it = cache.find(image->uniqueID());
  if (it == cache.end()) {
    auto texture = SkImages::TextureFromImage(recorder, image, props);
    auto [it2, _] = cache.emplace(image->uniqueID(), texture);
    it = it2;
  }
  it->second.last_used = last_tick;
  return it->second.image;
}

void AutomatImageProvider::TickCache() {
  size_t total_size = 0;
  for (auto& [id, entry] : cache) {
    total_size += entry.image->textureSize();
  }
  if (total_size > 1024 * 1024 * 1024) {
    std::vector<uint32_t> unused_ids_by_age;
    for (auto& [id, entry] : cache) {
      if (entry.last_used < last_tick) {
        unused_ids_by_age.push_back(id);
      }
    }
    std::sort(unused_ids_by_age.begin(), unused_ids_by_age.end(),
              [&](uint32_t a, uint32_t b) { return cache[a].last_used > cache[b].last_used; });
    while (!unused_ids_by_age.empty() && total_size > 1024 * 1024 * 1024) {
      auto id = unused_ids_by_age.back();
      unused_ids_by_age.pop_back();
      total_size -= cache[id].image->textureSize();
      cache.erase(id);
    }
  }
  last_tick = time::SteadyNow();
}

}  // namespace automat