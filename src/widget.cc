// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkDrawable.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/GrBackendSurface.h>
#include <include/gpu/GrDirectContext.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>

#include <ranges>

#include "../build/generated/embedded.hh"
#include "animation.hh"
#include "control_flow.hh"
#include "font.hh"
#include "global_resources.hh"
#include "log.hh"
#include "renderer.hh"
#include "time.hh"
#include "units.hh"

using namespace automat;
using namespace maf;
using namespace std;

namespace automat::gui {

animation::Phase Widget::PreDrawChildren(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  auto phase = animation::Finished;
  for (auto& widget : ranges::reverse_view(Children())) {
    canvas.save();
    const SkMatrix down = this->TransformToChild(*widget);
    SkMatrix up;
    if (down.invert(&up)) {
      canvas.concat(up);
    }
    phase |= widget->PreDraw(ctx);
    canvas.restore();
  }
  return phase;
}

animation::Phase Widget::DrawCached(DrawContext& ctx) const {
  if (pack_frame_texture_bounds == nullopt) {
    return Draw(ctx);
  }

  ctx.canvas.save();
  ctx.canvas.drawDrawable(compose_surface_drawable);
  ctx.canvas.restore();
  return invalidated == time::SteadyPoint::max() ? animation::Finished : animation::Animating;
}

void Widget::InvalidateDrawCache() const {
  if (invalidated == time::SteadyPoint::max()) {
    // When a widget is invalidated after a long sleep, we assume that it was just rendered. This
    // prevents the animation from thinking that the initial frame took a very long time.
    draw_time = time::SteadyNow();
  }
  invalidated = min(invalidated, time::SteadyNow());
}

animation::Phase Widget::DrawChildCachced(DrawContext& ctx, const Widget& child) const {
  const SkMatrix down = this->TransformToChild(child);
  SkMatrix up;
  if (down.invert(&up)) {
    ctx.canvas.concat(up);
  }
  return child.DrawCached(ctx);
}

animation::Phase Widget::DrawChildrenSpan(DrawContext& ctx,
                                          Span<shared_ptr<Widget>> widgets) const {
  auto phase = animation::Finished;
  auto& canvas = ctx.canvas;
  std::ranges::reverse_view rv{widgets};
  for (auto& widget : rv) {
    canvas.save();
    phase |= DrawChildCachced(ctx, *widget);
    canvas.restore();
  }  // for each Widget
  return phase;
}

animation::Phase Widget::DrawChildren(DrawContext& ctx) const {
  auto phase = PreDrawChildren(ctx);
  Visitor visitor = [&](Span<shared_ptr<Widget>> widgets) {
    phase |= DrawChildrenSpan(ctx, widgets);
    return ControlFlow::Continue;
  };
  const_cast<Widget*>(this)->VisitChildren(visitor);
  return phase;
}

SkMatrix TransformDown(const Widget& to) {
  if (to.parent) {
    SkMatrix ret = TransformDown(*to.parent);
    ret.postConcat(to.parent->TransformToChild(to));
    return ret;
  } else {
    return SkMatrix::I();
  }
}

SkMatrix TransformUp(const Widget& from) {
  SkMatrix down = TransformDown(from);
  SkMatrix up;
  if (down.invert(&up)) {
    return up;
  } else {
    return SkMatrix::I();
  }
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
  auto cpu_started = time::SteadyNow();
  auto direct_ctx = root_canvas.recordingContext()->asDirectContext();
  surface = root_canvas.getSurface()->makeSurface(surface_bounds_root.width(),
                                                  surface_bounds_root.height());

  auto fake_canvas = surface->getCanvas();
  fake_canvas->clear(SK_ColorTRANSPARENT);
  recording->draw(fake_canvas, -surface_bounds_root.left(),
                  -surface_bounds_root.top());  // execute the draw
                                                // commands immediately

  GrFlushInfo flush_info = {
      .fFinishedProc =
          [](GrGpuFinishedContext context) {
            Widget* w = static_cast<Widget*>(context);
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
          },
      .fFinishedContext = this,
      .fSubmittedProc =
          [](GrGpuSubmittedContext context, bool success) {
            Widget* w = static_cast<Widget*>(context);
            if (w->gpu_started == time::SteadyPoint::min()) {
              w->gpu_started = time::SteadyNow();
            } else if (w->gpu_started == time::SteadyPoint::max()) {
              // Sometimes fFinishedProc is called before fSubmittedProc.
              // When this happens, fFinishedProc sets the gpu_started to a guard value (max).
              // When we see this value in SubmittedProc, this means that we have been reordered
              // and we shouldn't record the time.
              w->gpu_started = time::SteadyPoint::min();
            } else {
              ERROR << "SubmittedProc for " << w->Name()
                    << " was called multiple times without FinishedProc in between. Current "
                       "submitted success = "
                    << success;
            }
          },
      .fSubmittedContext = this,
  };

  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "BeginFlush(";
    debug_render_events += Name();
    debug_render_events += ") ";
  }
  direct_ctx->flush(surface.get(), flush_info);
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
    // LOG << "Drawing choppy drawable " << entry->path << " (no surface!)";
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

    auto anchors = TextureAnchors();

    auto anchor_count = min<int>(draw_texture_anchors.size(), anchors.size());
    anchor_count = min<int>(pack_frame_texture_anchors.size(), anchor_count);

    if (anchor_count == 2) {
      SkSamplingOptions sampling;
      static auto builder =
          resources::RuntimeEffectBuilder(embedded::assets_anchor_warp_rt_sksl.content);

      builder->uniform("surfaceOrigin") = Rect(surface_bounds_local).BottomLeftCorner();
      builder->uniform("surfaceSize") = Rect(surface_bounds_local).Size();
      builder->uniform("surfaceResolution") = Vec2(surface->width(), surface->height());
      builder->uniform("anchorsLast").set(&draw_texture_anchors[0], anchor_count);
      builder->uniform("anchorsCurr").set(&anchors[0], anchor_count);
      builder->child("surface") = surface->makeImageSnapshot()->makeShader(sampling);

      auto shader = builder->makeShader();
      SkPaint paint;
      paint.setShader(shader);

      // Heuristic for finding same texture bounds (guaranteed to contain the whole widget):
      // - for every anchor move the old texture bounds by its displacement
      // - compute a union of all the moved bounds
      Rect new_anchor_bounds = anchors[0];
      for (int i = 0; i < anchor_count; ++i) {
        Vec2 delta = anchors[i] - draw_texture_anchors[i];
        Rect offset_bounds = surface_bounds_local.sk.makeOffset(delta);
        new_anchor_bounds.ExpandToInclude(offset_bounds);
      }
      canvas->drawRect(new_anchor_bounds.sk, paint);

      if constexpr (kDebugRendering) {
        SkPaint anchor_paint;
        anchor_paint.setStyle(SkPaint::kFill_Style);
        anchor_paint.setColor(SkColorSetARGB(128, 0, 0, 0));
        for (auto& anchor : anchors) {
          canvas->drawCircle(anchor.sk, 1_mm, anchor_paint);
        }
      }
    } else {
      canvas->save();

      // Maps from the local coordinates to surface UV
      SkMatrix surface_transform;
      Rect unit = Rect::MakeZeroWH(1, 1);
      surface_transform.postConcat(SkMatrix::RectToRect(surface_bounds_local.sk, unit));
      if (anchor_count) {
        SkMatrix anchor_mapping;
        // Modify the current canvas by the movement of the anchors since the last PackFrame
        // This allows us to draw the most recent texture bounds (pack_frame_texture_bounds)
        // which cover the whole currently visible part of the widget.
        if (anchor_mapping.setPolyToPoly(&pack_frame_texture_anchors[0].sk, &anchors[0].sk,
                                         anchor_count)) {
          canvas->concat(anchor_mapping);
        }
        // Apply the inverse transform to the surface mapping - we want to get the original texture
        // position. Note that this transform uses `draw_texture_anchors` which have been saved
        // during the last RenderToSurface.
        if (anchor_mapping.setPolyToPoly(&anchors[0].sk, &draw_texture_anchors[0].sk,
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
      builder->child("surface") = surface->makeImageSnapshot()->makeShader(
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
        double fraction = modf(draw_time.time_since_epoch().count() / 4, &integer_ignored);
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
  Visitor visitor = [this](maf::Span<std::shared_ptr<Widget>> children) {
    for (auto& child : children) {
      if (child->parent.get() != this) {
        // TODO: uncomment this and fix all instances of this error
        // ERROR << "Widget " << child->Name() << " has parent " << f("%p", child->parent.get())
        //       << " but should have " << this->Name() << f(" (%p)", this);
        child->parent = this->SharedPtr();
      }
      child->FixParents();
    }
    return ControlFlow::Continue;
  };
  VisitChildren(visitor);
}
void Widget::ForgetParents() {
  parent = nullptr;
  Visitor visitor = [](Span<shared_ptr<Widget>> children) {
    for (auto& child : children) {
      child->ForgetParents();
    }
    return ControlFlow::Continue;
  };
  VisitChildren(visitor);
}
}  // namespace automat::gui