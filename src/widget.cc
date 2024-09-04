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

      if (widget->ChildrenOutside()) {
        widget->Draw(ctx);
      } else {
        auto surface_label = ToStr(ctx.path);
        auto shape = widget->Shape(&ctx.display);
        auto bounds = shape.getBounds();
        SkRect root_bounds;
        canvas.getTotalMatrix().mapRect(&root_bounds, bounds);
        auto baseLayerSize = canvas.getBaseLayerSize();
        bool intersects =
            root_bounds.intersect(Rect::MakeZeroWH(baseLayerSize.width(), baseLayerSize.height()));
        root_bounds.fBottom = ceil(root_bounds.fBottom);
        root_bounds.fRight = ceil(root_bounds.fRight);
        root_bounds.fLeft = floor(root_bounds.fLeft);
        root_bounds.fTop = floor(root_bounds.fTop);

        if (intersects && root_bounds.width() >= 1 && root_bounds.height() >= 1) {
          // LOG << "Drawing " << surface_label << " at " << Rect(root_bounds);

          // DONE Create a new surface with size clipped to screen & widget size.
          // DONE Create a new canvas for the surface.
          // DONE Draw the widget.

          // TODO: Store the surfafce in the cache:
          // - key: (widget path)
          // - value: (surface, clip, matrix, last used time)

          // TODO: mark more objects as ChildrenOutside

          // TODO: A bunch of invalidate calls.
          // - "invalidate" function could just clear cache entries with the given widget

          // TODO: Periodically check all cache entries and remove the ones that were not used in
          // the last X seconds.

          auto surf = canvas.getSurface()->makeSurface(root_bounds.width(), root_bounds.height());

          DrawContext fake_ctx(*surf->getCanvas(), ctx.display);
          fake_ctx.path = ctx.path;
          fake_ctx.canvas.translate(-root_bounds.left(), -root_bounds.top());
          fake_ctx.canvas.concat(canvas.getTotalMatrix());

          widget->Draw(fake_ctx);

          SkPaint debug_paint;
          debug_paint.setColor(SK_ColorRED);
          debug_paint.setStyle(SkPaint::kStroke_Style);
          canvas.drawRect(bounds, debug_paint);

          canvas.resetMatrix();
          surf->draw(&canvas, root_bounds.left(), root_bounds.top());

        } else {
          // LOG << "Skipping " << surface_label;
        }
      }

      canvas.restore();
      ctx.path.pop_back();
    }
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