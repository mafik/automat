// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "render_shadows.hpp"

#include <include/core/SkBlendMode.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkSurface.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/graphite/BackendTexture.h>
#include <include/gpu/graphite/Surface.h>
#include <include/gpu/graphite/vk/VulkanGraphiteTypes.h>
#include <vulkan/vulkan_core.h>

#include <mutex>

#include "color.hpp"
#include "embedded.hpp"
#include "global_resources.hpp"
#include "log.hpp"
#include "status.hpp"

namespace automat {

constexpr float kMaxHeightMeters = 0.016f;

static skgpu::graphite::TextureInfo HeightTextureInfo() {
  skgpu::graphite::VulkanTextureInfo info{};
  info.fFormat = VK_FORMAT_B8G8R8A8_UNORM;
  info.fImageUsageFlags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  return skgpu::graphite::TextureInfos::MakeVulkan(info);
}

static std::mutex height_texture_mutex;
static skgpu::graphite::BackendTexture height_texture;  // shared by the recorder threads

thread_local sk_sp<SkImage> height_image;
thread_local SkIVector height_origin;

void RenderShadowHeightMap(skgpu::graphite::Recorder& recorder, SkSpan<const ShadowCaster> casters,
                           SkISize baker_size, SkIVector baker_origin) {
  {
    std::lock_guard lock(height_texture_mutex);
    if (!height_texture.isValid() || height_texture.dimensions() != baker_size) {
      if (height_texture.isValid()) {
        recorder.deleteBackendTexture(height_texture);
      }
      height_texture = recorder.createBackendTexture(baker_size, HeightTextureInfo());
    }
  }
  auto surface = SkSurfaces::WrapBackendTexture(&recorder, height_texture, kBGRA_8888_SkColorType,
                                                nullptr, nullptr);
  height_image = surface ? SkSurfaces::AsImage(surface) : nullptr;
  height_origin = baker_origin;
  if (!surface) {
    return;
  }
  auto* canvas = surface->getCanvas();
  canvas->clear(SK_ColorTRANSPARENT);
  for (auto& caster : casters) {
    float encoded = std::min(caster.elevation / kMaxHeightMeters, 1.f);
    float alpha_to_red[20] = {};  // r = a * encoded, a = 1
    alpha_to_red[3] = encoded;
    alpha_to_red[19] = 1;
    SkPaint paint;
    paint.setColorFilter(SkColorFilters::Matrix(alpha_to_red));
    paint.setBlendMode(SkBlendMode::kLighten);
    auto dst = SkRect::Make(caster.surface_bounds_root.makeOffset(-baker_origin));
    canvas->drawImageRect(caster.texture, dst, SkSamplingOptions(SkFilterMode::kLinear), &paint);
  }
}

void DrawShadow(SkCanvas& canvas, const ShadowCaster& caster) {
  if (!height_image || !caster.texture) {
    return;
  }
  Status status;
  static auto effect = resources::CompileShader(embedded::assets_shadow_rt_sksl, status);
  if (!effect) {
    ERROR_ONCE << "shadow_rt.sksl: " << status;
    return;
  }
  SkMatrix local_to_tex = caster.matrix;
  local_to_tex.postConcat(
      SkMatrix::RectToRect(SkRect::Make(caster.surface_bounds_root),
                           SkRect::MakeWH(caster.texture->width(), caster.texture->height())));
  SkMatrix local_to_height = caster.matrix;
  local_to_height.postTranslate(-height_origin.x(), -height_origin.y());

  SkRuntimeEffectBuilder builder(effect);
  builder.uniform("local_to_tex") = local_to_tex;
  builder.uniform("local_to_height") = local_to_height;
  builder.uniform("elevation") = caster.elevation;
  auto tint = SkColor4f::FromColor("#09000c5b"_color).premul();
  builder.uniform("tint") = SkV4{tint.fR, tint.fG, tint.fB, tint.fA};
  SkSamplingOptions sampling(SkFilterMode::kLinear);
  builder.child("caster") =
      caster.texture->makeShader(SkTileMode::kDecal, SkTileMode::kDecal, sampling);
  builder.child("height_map") =
      height_image->makeShader(SkTileMode::kDecal, SkTileMode::kDecal, sampling);
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setShader(builder.makeShader());
  canvas.drawRect(ShadowBounds(caster.surface_bounds_local, caster.elevation).sk, paint);
}

void ShutdownShadows(skgpu::graphite::Recorder& recorder) {
  std::lock_guard lock(height_texture_mutex);
  if (height_texture.isValid()) {
    recorder.deleteBackendTexture(height_texture);
    height_texture = {};
  }
}

}  // namespace automat
