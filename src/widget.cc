// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkDrawable.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/graphite/Context.h>
#include <include/gpu/graphite/Surface.h>

#include <ranges>

#include "../build/generated/embedded.hh"
#include "font.hh"
#include "global_resources.hh"
#include "log.hh"
#include "renderer.hh"
#include "root_widget.hh"
#include "textures.hh"
#include "time.hh"
#include "units.hh"
#include "vk.hh"

using namespace automat;
using namespace maf;
using namespace std;

namespace automat::gui {

void Widget::PreDrawChildren(SkCanvas& canvas) const {
  for (auto& widget : ranges::reverse_view(Children())) {
    canvas.save();
    canvas.concat(widget->local_to_parent);
    widget->PreDraw(canvas);
    canvas.restore();
  }
}

void Widget::DrawCached(SkCanvas& canvas) const {
  if (pack_frame_texture_bounds == nullopt) {
    return Draw(canvas);
  }

  canvas.drawDrawable(compose_surface_drawable);
}

void Widget::WakeAnimation() const {
  auto now = time::SteadyNow();
  if (wake_time == time::SteadyPoint::max()) {
    // When a widget is woken up after a long sleep, we assume that it was just rendered. This
    // prevents the animation from thinking that the initial frame took a very long time.
    last_tick_time = now;
  }
  wake_time = min(wake_time, now);
}

void Widget::DrawChildCachced(SkCanvas& canvas, const Widget& child) const {
  canvas.save();
  canvas.concat(child.local_to_parent);
  child.DrawCached(canvas);
  canvas.restore();
}

void Widget::DrawChildrenSpan(SkCanvas& canvas, Span<shared_ptr<Widget>> widgets) const {
  std::ranges::reverse_view rv{widgets};
  for (auto& widget : rv) {
    DrawChildCachced(canvas, *widget);
  }  // for each Widget
}

void Widget::DrawChildren(SkCanvas& canvas) const {
  PreDrawChildren(canvas);
  for (auto& child : ranges::reverse_view(Children())) {
    DrawChildCachced(canvas, *child);
  }
}

SkMatrix TransformDown(const Widget& to) {
  auto up = TransformUp(to);
  SkMatrix down;
  (void)up.invert(&down);
  return down;
}

SkMatrix TransformUp(const Widget& from) {
  SkMatrix up = from.local_to_parent.asM33();
  if (from.parent) {
    up.postConcat(TransformUp(*from.parent));
  }
  return up;
}

SkMatrix TransformBetween(const Widget& from, const Widget& to) {
  // TODO: optimize by finding the closest common parent
  auto up = TransformUp(from);
  auto down = TransformDown(to);
  return SkMatrix::Concat(down, up);
}

Str ToStr(shared_ptr<Widget> widget) {
  Str ret;
  while (widget) {
    ret = Str(widget->Name()) + (ret.empty() ? "" : " -> " + ret);
  }
  return ret;
}

std::map<uint32_t, Widget*>& GetWidgetIndex() {
  static std::map<uint32_t, Widget*> widget_index = {};
  return widget_index;
}

Widget::Widget() { GetWidgetIndex()[ID()] = this; }
Widget::~Widget() { GetWidgetIndex().erase(ID()); }

void Widget::CheckAllWidgetsReleased() {
  auto& widget_index = GetWidgetIndex();
  if (widget_index.empty()) {
    return;
  }
  ERROR << "Leaked references to " << widget_index.size() << " widget(s):";
  for (auto& [id, widget] : widget_index) {
    auto name = widget->Name();
    ERROR << f("  %p with ID %d with name %*s", widget, id, name.size(), name.data());
  }
}

uint32_t Widget::ID() const {
  static atomic<uint32_t> id_counter = 1;
  if (id == 0) {
    id = id_counter++;
  }
  return id;
}

Widget* Widget::Find(uint32_t id) {
  if (auto it = GetWidgetIndex().find(id); it != GetWidgetIndex().end()) {
    return it->second;
  } else {
    return nullptr;
  }
}

PackFrameRequest next_frame_request = {};

void Widget::RenderToSurface(SkCanvas& root_canvas) {
  skgpu::graphite::RecorderOptions options;
  options.fImageProvider = image_provider;
  auto recorder = vk::graphite_context->makeRecorder(options);
  auto cpu_started = time::SteadyNow();
  auto root_surface = root_canvas.getSurface();
  auto image_info = root_surface->imageInfo().makeDimensions(surface_bounds_root.size());

  surface = SkSurfaces::RenderTarget(recorder.get(), image_info, skgpu::Mipmapped::kNo,
                                     &root_surface->props(), Name());

  auto graphite_canvas = surface->getCanvas();
  graphite_canvas->clear(SK_ColorTRANSPARENT);
  graphite_canvas->translate(-surface_bounds_root.left(), -surface_bounds_root.top());
  // Remove all Drawables by converting the commands into SkPicture
  recording->makePictureSnapshot()->playback(graphite_canvas);

  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "BeginFlush(";
    debug_render_events += Name();
    debug_render_events += ") ";
  }
  auto graphite_recording = recorder->snap();
  skgpu::graphite::InsertRecordingInfo insert_recording_info;
  insert_recording_info.fRecording = graphite_recording.get();
  insert_recording_info.fFinishedContext = new std::weak_ptr(WeakPtr());
  insert_recording_info.fFinishedProc = [](skgpu::graphite::GpuFinishedContext context,
                                           skgpu::CallbackResult) {
    auto weak_ptr = static_cast<std::weak_ptr<Widget>*>(context);
    auto shared_ptr = weak_ptr->lock();
    delete weak_ptr;
    Widget* w = shared_ptr.get();
    if (w == nullptr) {
      return;
    }
    auto id = w->ID();
    float gpu_time;
    if (w->gpu_started == time::SteadyPoint::min()) {
      gpu_time = 0;
      w->gpu_started = time::SteadyPoint::max();
    } else if (w->gpu_started == time::SteadyPoint::max()) {
      gpu_time = 0;
      ERROR << "FinishedProc for " << w->Name()
            << " was called multiple times, without SubmittedProc in between.";
    } else {
      gpu_time = (float)(time::SteadyNow() - w->gpu_started).count();
      w->gpu_started = time::SteadyPoint::min();
    }
    float render_time = max(gpu_time, w->cpu_time);
    if (render_time > 1) {
      LOG << "Widget " << w->Name() << " took " << render_time << "s to render";
    }
    next_frame_request.render_results.push_back({id, render_time});
    if constexpr (kDebugRendering && kDebugRenderEvents) {
      debug_render_events += "Finished(";
      debug_render_events += w->Name();
      debug_render_events += ") ";
    }
  };

  vk::graphite_context->insertRecording(insert_recording_info);

  bool success = vk::graphite_context->submit();

  if (gpu_started == time::SteadyPoint::min()) {
    gpu_started = time::SteadyNow();
  } else if (gpu_started == time::SteadyPoint::max()) {
    // Sometimes fFinishedProc is called before fSubmittedProc.
    // When this happens, fFinishedProc sets the gpu_started to a guard value (max).
    // When we see this value in SubmittedProc, this means that we have been reordered
    // and we shouldn't record the time.
    gpu_started = time::SteadyPoint::min();
  } else {
    ERROR << "SubmittedProc for " << Name()
          << " was called multiple times without FinishedProc in between. Current "
             "submitted success = "
          << success;
  }

  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "EndFlush(";
    debug_render_events += Name();
    debug_render_events += ") ";
  }
  cpu_time = (time::SteadyNow() - cpu_started).count();

  window_to_local.mapRect(&surface_bounds_local.sk, SkRect::Make(surface_bounds_root));
  draw_texture_anchors = pack_frame_texture_anchors;
  draw_texture_bounds = *pack_frame_texture_bounds;
}

// Lifetime of the frame (from the Widget's perspective):
// - Update - includes the logic / animation update for the widget
// - Draw - records drawing commands into SkDrawable
// - RenderToSurface - Widget renders its SkSurface using the recorded commands
// - ComposeSurface - compose the drawn SkSurface onto the canvas

void Widget::ComposeSurface(SkCanvas* canvas) const {
  if constexpr (kDebugRendering) {
    SkPaint texture_bounds_paint;  // translucent black
    texture_bounds_paint.setStyle(SkPaint::kStroke_Style);
    texture_bounds_paint.setColor(SkColorSetARGB(128, 0, 0, 0));
    canvas->drawRect(draw_texture_bounds.sk, texture_bounds_paint);
  }

  if (surface == nullptr) {
    // LOG << "Missing surface for " << Name();
  } else {
    // Inside entry we have a cached surface that was renderd with old matrix. Now we want to
    // draw this surface using canvas.getTotalMatrix(). We do this by appending the inverse of
    // the old matrix to the current canvas. When the surface is drawn, its hardcoded matrix
    // will cancel the inverse and leave us with canvas.getTotalMatrix().
    // SkMatrix old_inverse;
    // (void)entry->draw_matrix.invert(&old_inverse);
    // canvas->concat(old_inverse);
    // entry->surface->draw(canvas, 0, 0);

    // Alternative approach, where we map the old texture to the new bounds:
    SkRect surface_size = SkRect::MakeWH(surface->width(), surface->height());

    auto anchor_count = min<int>(draw_texture_anchors.size(), fresh_texture_anchors.size());

    if (anchor_count == 2) {
      SkSamplingOptions sampling;
      static auto builder =
          resources::RuntimeEffectBuilder(embedded::assets_anchor_warp_rt_sksl.content);

      builder->uniform("surfaceOrigin") = Rect(surface_bounds_local).BottomLeftCorner();
      builder->uniform("surfaceSize") = Rect(surface_bounds_local).Size();
      builder->uniform("surfaceResolution") = Vec2(surface->width(), surface->height());
      builder->uniform("anchorsLast").set(&draw_texture_anchors[0], anchor_count);
      builder->uniform("anchorsCurr").set(&fresh_texture_anchors[0], anchor_count);
      builder->child("surface") = SkSurfaces::AsImage(surface)->makeShader(sampling);

      auto shader = builder->makeShader();
      SkPaint paint;
      paint.setShader(shader);

      // Heuristic for finding same texture bounds (guaranteed to contain the whole widget):
      // - for every anchor move the old texture bounds by its displacement
      // - compute a union of all the moved bounds
      Rect new_anchor_bounds = fresh_texture_anchors[0];
      for (int i = 0; i < anchor_count; ++i) {
        Vec2 delta = fresh_texture_anchors[i] - draw_texture_anchors[i];
        Rect offset_bounds = surface_bounds_local.sk.makeOffset(delta);
        new_anchor_bounds.ExpandToInclude(offset_bounds);
      }
      SkMatrix flip = SkMatrix::I();
      flip.preScale(1, -1, 0, surface_bounds_local.CenterY());
      canvas->concat(flip);
      canvas->drawRect(new_anchor_bounds.sk, paint);

      if constexpr (kDebugRendering) {
        SkPaint anchor_paint;
        anchor_paint.setStyle(SkPaint::kFill_Style);
        anchor_paint.setColor(SkColorSetARGB(128, 0, 0, 0));
        for (auto& anchor : fresh_texture_anchors) {
          canvas->drawCircle(anchor.sk, 1_mm, anchor_paint);
        }
      }
    } else {
      canvas->save();

      // Maps from the local coordinates to surface UV
      SkMatrix surface_transform;
      Rect unit = Rect::MakeCornerZero(1, 1);
      surface_transform.postConcat(SkMatrix::RectToRect(surface_bounds_local.sk, unit));
      // Skia puts the origin at the top left corner (going down), but we use bottom left (going
      // up). This flip makes all the textures composite in our coordinate system correctly.
      surface_transform.preScale(1, -1, 0, surface_bounds_local.CenterY());
      if (anchor_count) {
        SkMatrix anchor_mapping;
        // Apply the inverse transform to the surface mapping - we want to get the original texture
        // position. Note that this transform uses `draw_texture_anchors` which have been saved
        // during the last RenderToSurface.
        if (anchor_mapping.setPolyToPoly(&fresh_texture_anchors[0].sk, &draw_texture_anchors[0].sk,
                                         anchor_count)) {
          surface_transform.preConcat(anchor_mapping);
        }
      }

      static auto builder =
          resources::RuntimeEffectBuilder(embedded::assets_glitch_rt_sksl.content);
      builder->uniform("surfaceResolution") = Vec2(surface->width(), surface->height());
      builder->uniform("surfaceTransform") = surface_transform;
      float time = fmod(time::SteadyNow().time_since_epoch().count(), 1.0);
      builder->uniform("time") = time;
      SkSamplingOptions sampling;
      builder->child("surface") = SkSurfaces::AsImage(surface)->makeShader(
          SkTileMode::kMirror, SkTileMode::kMirror, sampling);
      auto shader = builder->makeShader();
      SkPaint paint;
      paint.setShader(shader);
      canvas->drawRect(*pack_frame_texture_bounds, paint);

      if constexpr (kDebugRendering) {
        SkPaint surface_bounds_paint;
        constexpr int kNumColors = 10;
        SkColor colors[kNumColors];
        float pos[kNumColors];
        double integer_ignored;
        double fraction = modf(last_tick_time.time_since_epoch().count() / 4, &integer_ignored);
        SkMatrix shader_matrix = SkMatrix::RotateDeg(fraction * -360.0f, surface_size.center());
        for (int i = 0; i < kNumColors; ++i) {
          float hsv[] = {i * 360.0f / kNumColors, 1.0f, 1.0f};
          colors[i] = SkHSVToColor((kNumColors - i) * 255 / kNumColors, hsv);
          pos[i] = (float)i / (kNumColors - 1);
        }
        surface_bounds_paint.setShader(
            SkGradientShader::MakeSweep(surface_size.centerX(), surface_size.centerY(), colors, pos,
                                        kNumColors, 0, &shader_matrix));
        surface_bounds_paint.setStyle(SkPaint::kStroke_Style);
        surface_bounds_paint.setStrokeWidth(2.0f);
        canvas->concat(SkMatrix::RectToRect(surface_size, surface_bounds_local.sk));
        canvas->drawRect(surface_size.makeInset(1, 1), surface_bounds_paint);
      }
      canvas->restore();
    }

    if constexpr (kDebugRendering) {
      auto& font = GetFont();
      SkPaint text_paint;
      canvas->translate(pack_frame_texture_bounds->left(),
                        min(pack_frame_texture_bounds->top(), pack_frame_texture_bounds->bottom()));
      auto text = f("%.1f", average_draw_millis);
      font.DrawText(*canvas, text, text_paint);
    }
  }
}

void Widget::FixParents() {
  for (auto& child : Children()) {
    if (child->parent.get() != this) {
      // TODO: uncomment this and fix all instances of this error
      // ERROR << "Widget " << child->Name() << " has parent " << f("%p", child->parent.get())
      //       << " but should have " << this->Name() << f(" (%p)", this);
      child->parent = this->SharedPtr();
    }
    child->FixParents();
  }
}
void Widget::ForgetParents() {
  parent = nullptr;
  for (auto& child : Children()) {
    child->ForgetParents();
  }
}

std::shared_ptr<Widget> Widget::ForObject(Object& object, const Widget& parent) {
  return parent.FindRootWidget().widgets.For(object, parent);
}

void Widget::ConnectionPositions(maf::Vec<Vec2AndDir>& out_positions) const {
  // By default just one position on the top of the bounding box.
  auto shape = Shape();
  Rect bounds = shape.getBounds();
  out_positions.push_back(Vec2AndDir{
      .pos = bounds.TopCenter(),
      .dir = -90_deg,
  });
}

Vec2AndDir Widget::ArgStart(const Argument& arg) {
  SkPath shape;
  if (arg.field) {
    shape = FieldShape(*arg.field);
  }
  if (shape.isEmpty()) {
    shape = Shape();
  }
  Rect bounds = shape.getBounds();
  return Vec2AndDir{
      .pos = bounds.BottomCenter(),
      .dir = -90_deg,
  };
}

RootWidget& Widget::FindRootWidget() const {
  Widget* w = const_cast<Widget*>(this);
  while (w->parent.get()) {
    w = w->parent.get();
  }
  auto* root = dynamic_cast<struct RootWidget*>(w);
  assert(root);
  return *root;
}
}  // namespace automat::gui