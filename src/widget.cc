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
    SkMatrix transform_up = this->TransformFromChild(&widget, &ctx.animation_context);
    canvas.concat(transform_up);
    ctx.path.push_back(&widget);
    widget.Draw(ctx);
    ctx.path.pop_back();
    canvas.restore();
    return VisitResult::kContinue;
  };
  const_cast<Widget*>(this)->VisitChildren(visitor);
}

SkMatrix TransformDown(const Path& path, animation::Context* actx) {
  SkMatrix ret = SkMatrix::I();
  for (int i = 1; i < path.size(); ++i) {
    Widget& parent = *path[i - 1];
    Widget& child = *path[i];
    ret.postConcat(parent.TransformToChild(&child, actx));
  }
  return ret;
}

SkMatrix TransformUp(const Path& path, animation::Context* actx) {
  SkMatrix ret = SkMatrix::I();
  for (int i = 1; i < path.size(); ++i) {
    Widget& parent = *path[i - 1];
    Widget& child = *path[i];
    ret.postConcat(parent.TransformFromChild(&child, actx));
  }
  return ret;
}

}  // namespace automat::gui