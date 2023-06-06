#include "widget.h"

#include <include/core/SkMatrix.h>
#include <include/effects/SkRuntimeEffect.h>

#include <bitset>
#include <condition_variable>
#include <vector>

#include "animation.h"
#include "root.h"
#include "time.h"

using namespace automat;

namespace automat::gui {

void Widget::DrawChildren(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  Visitor visitor = [&](Widget& widget) {
    canvas.save();
    const SkMatrix down = this->TransformToChild(widget, ctx.animation_context);
    SkMatrix up;
    if (down.invert(&up)) {
      canvas.concat(up);
    }
    ctx.path.push_back(&widget);
    widget.Draw(ctx);
    ctx.path.pop_back();
    canvas.restore();
    return VisitResult::kContinue;
  };
  const_cast<Widget*>(this)->VisitChildren(visitor);
}

SkMatrix TransformDown(const Path& path, animation::Context& actx) {
  SkMatrix ret = SkMatrix::I();
  for (int i = 1; i < path.size(); ++i) {
    Widget& parent = *path[i - 1];
    Widget& child = *path[i];
    ret.postConcat(parent.TransformToChild(child, actx));
  }
  return ret;
}

SkMatrix TransformUp(const Path& path, animation::Context& actx) {
  SkMatrix down = TransformDown(path, actx);
  SkMatrix up;
  if (down.invert(&up)) {
    return up;
  } else {
    return SkMatrix::I();
  }
}

}  // namespace automat::gui