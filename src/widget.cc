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
        canvas.getTotalMatrix().mapRect(&bounds);
        // LOG << "Bounds of " << surface_label << ": " << Rect(bounds);
        auto baseLayerSize = canvas.getBaseLayerSize();
        // LOG << "Screen bounds: " << Rect::MakeZeroWH(baseLayerSize.width(),
        // baseLayerSize.height());
        bool intersects =
            bounds.intersect(Rect::MakeZeroWH(baseLayerSize.width(), baseLayerSize.height()));
        bounds.fBottom = ceil(bounds.fBottom);
        bounds.fRight = ceil(bounds.fRight);
        bounds.fLeft = floor(bounds.fLeft);
        bounds.fTop = floor(bounds.fTop);

        if (intersects && bounds.width() >= 1 && bounds.height() >= 1) {
          // LOG << "Drawing " << surface_label << " at " << Rect(bounds);

          GrDirectContext* gr_ctx = ctx.canvas.recordingContext()->asDirectContext();
          auto image_info = canvas.imageInfo();
          GrBackendTexture tex = gr_ctx->createBackendTexture(
              bounds.width(), bounds.height(), image_info.colorType(), skgpu::Mipmapped::kNo,
              GrRenderable::kYes, GrProtected::kNo, surface_label);

          struct ReleaseContext {
            GrBackendTexture tex;
            GrDirectContext* gr_ctx;
          };
          ReleaseContext* release_ctx = new ReleaseContext{tex, gr_ctx};

          auto base_props = canvas.getBaseProps();

          auto surf = SkSurfaces::WrapBackendTexture(
              gr_ctx, tex, kBottomLeft_GrSurfaceOrigin, 1, image_info.colorType(),
              image_info.refColorSpace(), &base_props,
              [](void* ptr) {
                ReleaseContext* rctx = (ReleaseContext*)ptr;
                rctx->gr_ctx->deleteBackendTexture(rctx->tex);
                delete rctx;
              },
              release_ctx);

          surf->getCanvas();
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

          DrawContext fake_ctx(*surf->getCanvas(), ctx.display);
          fake_ctx.path = ctx.path;
          fake_ctx.canvas.translate(-bounds.left(), -bounds.top());
          fake_ctx.canvas.concat(canvas.getTotalMatrix());

          widget->Draw(fake_ctx);

          auto img = surf->makeImageSnapshot();

          SkMatrix total_inverse;
          (void)canvas.getTotalMatrix().invert(&total_inverse);
          total_inverse.mapRect(&bounds);
          SkPaint debug_paint;
          debug_paint.setColor(SK_ColorRED);
          debug_paint.setStyle(SkPaint::kStroke_Style);
          canvas.drawRect(bounds, debug_paint);

          canvas.drawImageRect(img, bounds, SkSamplingOptions(), nullptr);

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