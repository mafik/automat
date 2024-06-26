#include "widget.hh"

#include <include/core/SkMatrix.h>
#include <include/effects/SkRuntimeEffect.h>

#include <ranges>

#include "animation.hh"
#include "control_flow.hh"

using namespace automat;
using namespace maf;

namespace automat::gui {

void Widget::DrawChildren(DrawContext& ctx) const {
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
      widget->Draw(ctx);
      ctx.path.pop_back();
      canvas.restore();
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

}  // namespace automat::gui