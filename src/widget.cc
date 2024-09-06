#include "widget.hh"

#include <include/core/SkColorSpace.h>
#include <include/core/SkMatrix.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/GrBackendSurface.h>
#include <include/gpu/GrDirectContext.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>

#include <ranges>

#include "animation.hh"
#include "control_flow.hh"
#include "window.hh"

using namespace automat;
using namespace maf;

namespace automat::gui {

void Widget::PreDrawChildren(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  Visitor visitor = [&](Span<Widget*> widgets) {
    std::ranges::reverse_view rv{widgets};
    for (Widget* widget : rv) {
      canvas.save();
      const SkMatrix down = this->TransformToChild(*widget, &ctx.display);
      SkMatrix up;
      if (down.invert(&up)) {
        canvas.concat(up);
      }
      ctx.path.push_back(widget);
      widget->PreDraw(ctx);
      ctx.path.pop_back();
      canvas.restore();
    }
    return ControlFlow::Continue;
  };
  const_cast<Widget*>(this)->VisitChildren(visitor);
}

struct CacheEntry {};

void Widget::DrawCached(DrawContext& ctx) const {
  if (ChildrenOutside()) {
    Draw(ctx);
    return;
  }
  auto& canvas = ctx.canvas;
  SkMatrix m = canvas.getTotalMatrix();
  auto shape = Shape(&ctx.display);
  auto bounds = shape.getBounds();
  SkRect root_bounds;
  m.mapRect(&root_bounds, bounds);
  auto canvas_bounds = SkRect::Make(canvas.getBaseLayerSize());

  bool intersects;
  if (root_bounds.width() * root_bounds.height() < 512 * 512) {
    // Render small objects without clipping
    intersects = SkRect::Intersects(root_bounds, canvas_bounds);
  } else {
    // This mutates the `root_bounds` - they're clipped to `canvas_bounds`!
    intersects = root_bounds.intersect(canvas_bounds);
  }
  if (!intersects) {
    return;
  }

  SkIRect root_bounds_rounded;
  root_bounds.roundOut(&root_bounds_rounded);

  if (root_bounds.width() < 1 || root_bounds.height() < 1) {
    return;
  }

  DrawCache::Entry& entry = ctx.draw_cache[ctx.path];
  bool needs_refresh = false;

  if (entry.surface.get() == nullptr) {
    needs_refresh = true;
  } else if (m.getScaleX() != entry.matrix.getScaleX() ||
             m.getScaleY() != entry.matrix.getScaleY() || m.getSkewX() != entry.matrix.getSkewX() ||
             m.getSkewY() != entry.matrix.getSkewY()) {
    needs_refresh = true;
  } else {
    SkMatrix old_inverse;
    (void)entry.matrix.invert(&old_inverse);
    SkMatrix m_inverse;
    (void)m.invert(&m_inverse);

    SkRect old_bounds, new_bounds;  // Local coordinates
    old_inverse.mapRect(&old_bounds, SkRect::Make(entry.root_bounds));
    m_inverse.mapRect(&new_bounds, root_bounds);

    if (!old_bounds.contains(new_bounds)) {
      needs_refresh = true;
    }
  }

  if (needs_refresh) {
    entry.surface =
        canvas.getSurface()->makeSurface(root_bounds_rounded.width(), root_bounds_rounded.height());
    entry.matrix = m;
    entry.root_bounds = root_bounds_rounded;

    DrawContext fake_ctx(ctx.display, *entry.surface->getCanvas(), ctx.draw_cache);
    fake_ctx.path = ctx.path;
    fake_ctx.canvas.clear(SK_ColorTRANSPARENT);
    fake_ctx.canvas.translate(-root_bounds_rounded.left(), -root_bounds_rounded.top());
    fake_ctx.canvas.concat(m);

    Draw(fake_ctx);
  }
  entry.last_used = ctx.display.timer.steady_now;

  // Inside entry we have a cached surface that was renderd with old matrix. Now we want to
  // draw this surface using canvas.getTotalMatrix(). We do this by appending the inverse of
  // the old matrix to the current canvas. When the surface is drawn, its hardcoded matrix
  // will cancel the inverse and leave us with canvas.getTotalMatrix().
  SkMatrix old_inverse;
  (void)entry.matrix.invert(&old_inverse);
  canvas.concat(old_inverse);

  entry.surface->draw(&canvas, entry.root_bounds.left(), entry.root_bounds.top());
}

void Widget::InvalidateDrawCache() const {
  for (auto& window : windows) {
    for (int i = 0; i < window->draw_cache.entries.size(); ++i) {
      auto& e = window->draw_cache.entries[i];
      bool invalidate = false;
      for (auto widget : e->path) {
        if (widget == this) {
          invalidate = true;
          break;
        }
      }
      if (invalidate) {
        // TODO: maybe find a cleaner way to invalidate entries
        // because matrix prevents us from reusing this surface
        e->matrix = SkMatrix();
      }
    }
  }
}

void Widget::DrawChildren(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  Visitor visitor = [&](Span<Widget*> widgets) {
    std::ranges::reverse_view rv{widgets};
    for (Widget* widget : rv) {
      ctx.path.push_back(widget);
      canvas.save();

      const SkMatrix down = this->TransformToChild(*widget, &ctx.display);
      SkMatrix up;
      if (down.invert(&up)) {
        canvas.concat(up);
      }

      widget->DrawCached(ctx);

      canvas.restore();
      ctx.path.pop_back();
    }  // for each Widget
    return ControlFlow::Continue;
  };
  const_cast<Widget*>(this)->VisitChildren(visitor);
}

SkMatrix TransformDown(const Path& path, animation::Display* display) {
  SkMatrix ret = SkMatrix::I();
  for (int i = 1; i < path.size(); ++i) {
    Widget& parent = *path[i - 1];
    Widget& child = *path[i];
    ret.postConcat(parent.TransformToChild(child, display));
  }
  return ret;
}

SkMatrix TransformUp(const Path& path, animation::Display* display) {
  SkMatrix down = TransformDown(path, display);
  SkMatrix up;
  if (down.invert(&up)) {
    return up;
  } else {
    return SkMatrix::I();
  }
}

maf::Str ToStr(const Path& path) {
  maf::Str ret;
  for (Widget* widget : path) {
    if (!ret.empty()) {
      ret += " -> ";
    }
    ret += widget->Name();
  }
  return ret;
}

Widget::~Widget() {
  // TODO: design a better "PointerLeave" API so that this is not necessary.
  for (auto window : windows) {
    for (auto pointer : window->pointers) {
      for (auto& widget : pointer->path) {
        if (widget == this) {
          widget = nullptr;
        }
      }
    }
  }
}

}  // namespace automat::gui