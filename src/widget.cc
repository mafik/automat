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
  auto& animation_state = ctx.animation_state;
  Visitor visitor = [&](Widget& widget) {
    canvas.save();
    SkMatrix transform_up = this->TransformFromChild(&widget, &animation_state);
    canvas.concat(transform_up);
    widget.Draw(ctx);
    canvas.restore();
    return VisitResult::kContinue;
  };
  const_cast<Widget*>(this)->VisitChildren(visitor);
}

SkMatrix TransformDown(const Path& path, animation::State* state) {
  SkMatrix ret = SkMatrix::I();
  for (int i = 1; i < path.size(); ++i) {
    Widget& parent = *path[i - 1];
    Widget& child = *path[i];
    ret.postConcat(parent.TransformToChild(&child, state));
  }
  return ret;
}

SkMatrix TransformUp(const Path& path, animation::State* state) {
  SkMatrix ret = SkMatrix::I();
  for (int i = 1; i < path.size(); ++i) {
    Widget& parent = *path[i - 1];
    Widget& child = *path[i];
    ret.postConcat(parent.TransformFromChild(&child, state));
  }
  return ret;
}

}  // namespace automat::gui