#include "widget.h"

#include <bitset>
#include <condition_variable>
#include <vector>

#include <include/core/SkMatrix.h>
#include <include/effects/SkRuntimeEffect.h>

#include "animation.h"
#include "root.h"
#include "time.h"

using namespace automaton;

namespace automaton::gui {

void Widget::VisitAll(WidgetVisitor &visitor) {

  struct RecursiveVisitor : WidgetVisitor {
    WidgetVisitor &orig_visitor;
    SkMatrix transform_accumulator;

    RecursiveVisitor(WidgetVisitor &orig_visitor)
        : orig_visitor(orig_visitor), transform_accumulator() {}

    VisitResult operator()(Widget &widget, const SkMatrix &transform) override {

      SkMatrix backup = transform_accumulator;
      transform_accumulator.postConcat(transform);

      if (widget.VisitImmediateChildren(*this) == VisitResult::kStop)
        return VisitResult::kStop;

      if (orig_visitor(widget, transform_accumulator) == VisitResult::kStop)
        return VisitResult::kStop;

      transform_accumulator = backup;

      return VisitResult::kContinue;
    }
  };

  RecursiveVisitor recursive_visitor(visitor);

  recursive_visitor(*this, SkMatrix::I());
}

void Widget::VisitAtPoint(vec2 point, WidgetVisitor &visitor) {

  struct PointVisitor : WidgetVisitor {
    vec2 point;
    WidgetVisitor &orig_visitor;

    PointVisitor(vec2 point, WidgetVisitor &orig_visitor)
        : point(point), orig_visitor(orig_visitor) {}

    VisitResult operator()(Widget &widget, const SkMatrix &transform) override {
      auto shape = widget.Shape();
      SkPoint mapped_point = transform.mapXY(point.X, point.Y);
      if (shape.isEmpty() || shape.contains(mapped_point.fX, mapped_point.fY)) {
        if (orig_visitor(widget, transform) == VisitResult::kStop)
          return VisitResult::kStop;
      }
      return VisitResult::kContinue;
    }
  };

  PointVisitor point_visitor(point, visitor);

  VisitAll(point_visitor);
}

struct FunctionWidgetVisitor : WidgetVisitor {
  FunctionWidgetVisitor(WidgetVisitorFunc visitor) : func(visitor) {}
  VisitResult operator()(Widget &widget, const SkMatrix &transform) override {
    return func(widget, transform);
  }
  WidgetVisitorFunc func;
};

void Widget::VisitAll(WidgetVisitorFunc visitor) {
  FunctionWidgetVisitor function_visitor(visitor);
  VisitAll(function_visitor);
}

void Widget::VisitAtPoint(vec2 point, WidgetVisitorFunc visitor) {
  FunctionWidgetVisitor function_visitor(visitor);
  VisitAtPoint(point, function_visitor);
}

SkMatrix Widget::TransformFromParent() {
  SkMatrix ret = SkMatrix::I();
  if (Widget *parent = ParentWidget()) {
    FunctionWidgetVisitor visitor(
        [&](Widget &widget, const SkMatrix &transform) {
          if (&widget == this) {
            ret = transform;
            return VisitResult::kStop;
          }
          return VisitResult::kContinue;
        });
    parent->VisitImmediateChildren(visitor);
  }
  return ret;
}

static vec2 Vec2(SkPoint p) { return vec2{p.fX, p.fY}; }

SkMatrix Widget::TransformToChild(Widget *child) {
  SkMatrix transform;
  while (child && child != this) {
    transform.preConcat(child->TransformFromParent());
    child = child->ParentWidget();
  }
  if (child != this) {
    return SkMatrix::I();
  }
  return transform;
}

void Widget::DrawChildren(SkCanvas &canvas,
                          animation::State &animation_state) const {
  FunctionWidgetVisitor visitor([&](Widget &widget, const SkMatrix &transform) {
    canvas.save();
    SkMatrix inverse;
    if (transform.invert(&inverse)) {
      canvas.concat(inverse);
    }
    widget.Draw(canvas, animation_state);
    canvas.restore();
    return VisitResult::kContinue;
  });
  const_cast<Widget *>(this)->VisitImmediateChildren(visitor);
}

} // namespace automaton::gui