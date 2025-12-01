// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "textures.hh"

#include <include/core/SkData.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkShader.h>
#include <include/gpu/graphite/Image.h>
#include <include/gpu/graphite/Recording.h>

#include <tracy/Tracy.hpp>

#include "concurrentqueue.hh"
#include "embedded.hh"
#include "log.hh"
#include "math.hh"
#include "thread_name.hh"
#include "time.hh"
#include "units.hh"
#include "virtual_fs.hh"
#include "vk.hh"

namespace automat {

// Load an image from static (never deallocated) memory
sk_sp<SkImage> DecodeStaticImage(StrView content) {
  auto data = SkData::MakeWithoutCopy(content.data(), content.size());
  return SkImages::DeferredFromEncodedData(data);
}

// Load an image from an ephemeral (may be deallocated) memory
//
// It makes a copy of the image content for later decoding
sk_sp<SkImage> DecodeTemporaryImage(StrView content) {
  auto data = SkData::MakeWithCopy(content.data(), content.size());
  return SkImages::DeferredFromEncodedData(data);
}

static Vec<PersistentImage*>& AllPersistentImages() {
  static Vec<PersistentImage*> persistent_images;
  return persistent_images;
}

static void RegisterPersistentImage(PersistentImage& img) {
  AllPersistentImages().emplace_back(&img);
}

void PersistentImage::ReleaseAll() {
  for (auto* img : AllPersistentImages()) {
    img->shader.reset();
    img->image.reset();
    img->paint.reset();
  }
}

PersistentImage::PersistentImage(sk_sp<SkImage> image, MakeArgs args) : image(image) {
  float scale;

  if (args.matrix.has_value()) {
    matrix = *args.matrix;
    scale = matrix.getScaleX();
  } else {
    float width, height;
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
    matrix = SkMatrix::Scale(scale, -scale).postTranslate(0, height);
  }
  shader = args.raw_shader
               ? image->makeRawShader(args.tile_x, args.tile_y, args.sampling_options, matrix)
               : image->makeShader(args.tile_x, args.tile_y, args.sampling_options, matrix);
  paint.setShader(*shader);
  // TODO: there is a bug here - the image is loaded lazily, so its width & height are not available
  // yet.
  // This should be fixed by going through all the places where PersistentImages are used and making
  // sure to pass the dimensions.
  rect = matrix.mapRect(SkRect::MakeIWH(image->width(), image->height()));
  AllPersistentImages().push_back(this);
}

void PersistentImage::PreloadAll() {
  ZoneScoped;
  std::vector<std::jthread> workers;
  struct Task {
    PersistentImage* persistent_image;
    AutomatImageProvider::CacheEntry* cache_entry;
  };
  moodycamel::ConcurrentQueue<Task> tasks;
  for (auto* texture : AllPersistentImages()) {
    if (!(*texture->image)->isTextureBacked()) {
      tasks.enqueue({texture, &image_provider->cache[(*texture->image)->uniqueID()]});
    }
  }

  std::vector<std::unique_ptr<skgpu::graphite::Recording>> recordings;
  recordings.resize(std::thread::hardware_concurrency());
  for (int i = std::thread::hardware_concurrency() - 1; i >= 0; --i) {
    auto WorkFunc = [i, &recordings, &tasks, recorder = vk::graphite_context->makeRecorder()]() {
      if (i) {
        SetThreadName("PersistentImagePreloader", 1);
      }
      Task task;
      while (tasks.try_dequeue(task)) {
        ZoneScopedN("TextureFromImage");
        auto props = SkImage::RequiredProperties{.fMipmapped = true};
        task.cache_entry->image =
            SkImages::TextureFromImage(recorder.get(), task.persistent_image->image->get(), props);
      }
      recordings[i] = recorder->snap();
    };
    // Start some extra threads, but also use the current thread to do some processing
    if (i) {
      workers.emplace_back(std::move(WorkFunc));
    } else {
      WorkFunc();
    }
  }
  {
    ZoneScopedN("wait for workers");
    workers.clear();
  }
  {
    ZoneScopedN("insertRecording");
    for (int i = 0; i < recordings.size(); ++i) {
      auto insert_recording = skgpu::graphite::InsertRecordingInfo{
          .fRecording = recordings[i].get(),
      };
      vk::graphite_context->insertRecording(insert_recording);
    }
  }
  {
    ZoneScopedN("submit");
    vk::graphite_context->submit();
    // vk::graphite_context->submit(skgpu::graphite::SyncToCpu::kYes);
  }
}

PersistentImage PersistentImage::MakeFromAsset(fs::VFile& asset, MakeArgs args) {
  Status status;
  sk_sp<SkImage> image;
  fs::real.Map(asset.path, [&](StrView content) { image = DecodeTemporaryImage(content); }, status);
  if (!OK(status)) {
    image = DecodeStaticImage(asset.content);
  }
  return PersistentImage(std::move(image), args);
}

int PersistentImage::widthPx() { return (*image)->width(); }
int PersistentImage::heightPx() { return (*image)->height(); }

float PersistentImage::width() { return widthPx() * scale(); }
float PersistentImage::height() { return heightPx() * scale(); }

void PersistentImage::draw(SkCanvas& canvas) {
  if (!image) {
    ERROR << "Attempt to draw an uninitialized PersistentImage";
    return;
  }
  rect = matrix.mapRect(SkRect::MakeIWH((*image)->width(), (*image)->height()));
  canvas.drawRect(rect, paint);
}

sk_sp<AutomatImageProvider> image_provider;

sk_sp<SkImage> AutomatImageProvider::findOrCreate(skgpu::graphite::Recorder* recorder,
                                                  const SkImage* image,
                                                  SkImage::RequiredProperties props) {
  auto guard = std::lock_guard(mutex);
  ZoneScopedN("AutomatImageProvider::findOrCreate");
  auto it = cache.find(image->uniqueID());
  if (it == cache.end()) {
    std::string msg = "Creating image " + std::to_string(image->uniqueID());
    TracyMessage(msg.c_str(), msg.size());
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

namespace textures {

PersistentImage& PointingHandColor() {
  static auto pointing_hand_color =
      PersistentImage::MakeFromAsset(embedded::assets_pointing_hand_color_webp, {.height = 8.8_mm});
  return pointing_hand_color;
}

PersistentImage& PressingHandColor() {
  static auto pressing_hand_color =
      PersistentImage::MakeFromAsset(embedded::assets_pressing_hand_color_webp, {.height = 8.8_mm});
  return pressing_hand_color;
}

}  // namespace textures

}  // namespace automat
