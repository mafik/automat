// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "black_hole.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>

#include <cmath>

#include "embedded.hh"
#include "global_resources.hh"
#include "root_widget.hh"
#include "time.hh"

namespace automat::ui {

BlackHole::BlackHole(RootWidget* parent) : Widget(parent) {}

SkPath BlackHole::Shape() const {
  auto& root_widget = ParentRootWidget();
  return SkPath::Circle(root_widget.size.width, root_widget.size.height, radius);
}

RootWidget& BlackHole::ParentRootWidget() const { return static_cast<RootWidget&>(*parent); }

animation::Phase BlackHole::Tick(time::Timer& timer) {
  auto& root_widget = ParentRootWidget();
  float target_radius = root_widget.drag_action_count > 0 ? kMaxRadius : 0;
  auto phase = animation::ExponentialApproach(target_radius, timer.d, 0.1, radius);
  if (radius > 0) {
    phase |= animation::Animating;
  }
  return phase;
}
void BlackHole::Draw(SkCanvas& canvas) const {
  if (radius == 0) {
    return;
  }

  auto localToPx = canvas.getLocalToDeviceAs3x3();

  canvas.save();
  canvas.resetMatrix();

  auto& root_widget = ParentRootWidget();

  Status status;
  static auto effect = resources::CompileShader(embedded::assets_black_hole_rt_sksl, status);
  if (!OK(status)) {
    FATAL << status;
  }
  auto builder = SkRuntimeEffectBuilder(effect);
  builder.uniform("iCenterPx") = localToPx.mapPoint(root_widget.size);
  builder.uniform("iTime") = (float)time::SteadySaw<M_PI * 2>();
  builder.uniform("iRadiusPx") = localToPx.mapRadius(radius);
  builder.uniform("iMaxRadiusPx") = localToPx.mapRadius(kMaxRadius);

  auto runtime_shader_filter = SkImageFilters::RuntimeShader(builder, "iBackground", nullptr);

  auto save_layer_rec = SkCanvas::SaveLayerRec(nullptr, nullptr, runtime_shader_filter.get(),
                                               SkCanvas::kInitWithPrevious_SaveLayerFlag);
  canvas.saveLayer(save_layer_rec);
  canvas.restore();
  canvas.restore();
}

bool BlackHole::CanDrop(Location&) const { return true; }

SkMatrix BlackHole::DropSnap(const Rect& bounds, Vec2 bounds_origin, Vec2* fixed_point) {
  auto& root_widget = ParentRootWidget();
  auto& size = root_widget.size;

  Vec2 box_size = bounds.Size();
  float diagonal = Length(box_size);
  SkMatrix window2canvas = root_widget.WindowToCanvas();
  float scale = window2canvas.mapRadius(radius) / diagonal * 0.9f;
  scale = std::clamp<float>(scale, 0.1, 0.5);

  // Center of the visible black hole "pie" in canvas coordinates.
  Vec2 target_center = window2canvas.mapPoint(size - box_size / diagonal * radius / 2);

  return SkMatrix::Translate(target_center - bounds.Center())
      .postScale(scale, scale, target_center.x, target_center.y);
}

void BlackHole::DropLocation(Ptr<Location>&&) { audio::Play(embedded::assets_SFX_trash_wav); }

}  // namespace automat::ui
