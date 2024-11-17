// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "widget.hh"

#include <include/core/SkColorSpace.h>
#include <include/core/SkDrawable.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkRect.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/GrBackendSurface.h>
#include <include/gpu/GrDirectContext.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>

#include <ranges>

#include "animation.hh"
#include "control_flow.hh"
#include "font.hh"
#include "time.hh"

using namespace automat;
using namespace maf;
using namespace std;

namespace automat::gui {

animation::Phase Widget::PreDrawChildren(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  auto phase = animation::Finished;
  Visitor visitor = [&](Span<shared_ptr<Widget>> widgets) {
    std::ranges::reverse_view rv{widgets};
    for (auto& widget : rv) {
      canvas.save();
      const SkMatrix down = this->TransformToChild(*widget);
      SkMatrix up;
      if (down.invert(&up)) {
        canvas.concat(up);
      }
      phase |= widget->PreDraw(ctx);
      canvas.restore();
    }
    return ControlFlow::Continue;
  };
  const_cast<Widget*>(this)->VisitChildren(visitor);
  return phase;
}

animation::Phase Widget::DrawCached(DrawContext& ctx) const {
  auto texture_bounds = TextureBounds(&ctx.display);
  if (texture_bounds == nullopt) {
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

SkMatrix TransformDown(const Widget& to, const Widget* from) {
  if (to.parent) {
    if (to.parent.get() == from) {
      return to.parent->TransformToChild(to);
    } else {
      SkMatrix ret = TransformDown(*to.parent, from);
      ret.postConcat(to.parent->TransformToChild(to));
      return ret;
    }
  } else {
    return SkMatrix::I();
  }
}

SkMatrix TransformUp(const Widget& from, const Widget* to, animation::Display* display) {
  SkMatrix down = TransformDown(from, to);
  SkMatrix up;
  if (down.invert(&up)) {
    return up;
  } else {
    return SkMatrix::I();
  }
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
  render_started = time::SteadyNow();
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
            float render_time = (float)(time::SteadyNow() - w->render_started).count();
            next_frame_request.render_results.push_back({id, render_time});
          },
      .fFinishedContext = this,
  };

  direct_ctx->flush(surface.get(), flush_info);

  window_to_local.mapRect(&surface_bounds_local, SkRect::Make(surface_bounds_root));
}

// Lifetime of the frame (from the Widget's perspective):
// - Update - includes the logic / animation update for the widget
// - Draw - records drawing commands into SkDrawable
// - RenderToSurface - Widget renders its SkSurface using the recorded commands
// - ComposeSurface - compose the drawn SkSurface onto the canvas

void Widget::ComposeSurface(SkCanvas* canvas) const {
#ifdef DEBUG_RENDERING
  SkPaint texture_bounds_paint;  // translucent black
  texture_bounds_paint.setStyle(SkPaint::kStroke_Style);
  texture_bounds_paint.setColor(SkColorSetARGB(128, 0, 0, 0));
  canvas->drawRect(*texture_bounds, texture_bounds_paint);
#endif

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
#ifdef DEBUG_RENDERING
    auto matrix_backup = canvas->getTotalMatrix();
#endif
    canvas->concat(SkMatrix::RectToRect(surface_size, surface_bounds_local));
    surface->draw(canvas, 0, 0);
#ifdef DEBUG_RENDERING
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
    surface_bounds_paint.setShader(SkGradientShader::MakeSweep(surface_size.centerX(),
                                                               surface_size.centerY(), colors, pos,
                                                               kNumColors, 0, &shader_matrix));
    surface_bounds_paint.setStyle(SkPaint::kStroke_Style);
    surface_bounds_paint.setStrokeWidth(2.0f);
    canvas->drawRect(surface_size.makeInset(1, 1), surface_bounds_paint);

    auto& font = GetFont();
    SkPaint text_paint;
    canvas->setMatrix(matrix_backup);
    canvas->translate(texture_bounds->left(), min(texture_bounds->top(), texture_bounds->bottom()));
    auto text = f("%.1f", average_draw_millis);
    font.DrawText(*canvas, text, text_paint);
#endif
  }
}

}  // namespace automat::gui