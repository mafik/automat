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
    SkMatrix transform_down_accumulator;
    SkMatrix transform_up_accumulator;

    RecursiveVisitor(WidgetVisitor &orig_visitor)
        : orig_visitor(orig_visitor), transform_down_accumulator() {}

    VisitResult operator()(Widget &widget, const SkMatrix &transform_down,
                           const SkMatrix &transform_up) override {

      SkMatrix backup_down = transform_down_accumulator;
      SkMatrix backup_up = transform_up_accumulator;
      transform_down_accumulator.postConcat(transform_down);
      transform_up_accumulator.postConcat(transform_up);

      if (widget.VisitImmediateChildren(*this) == VisitResult::kStop)
        return VisitResult::kStop;

      if (orig_visitor(widget, transform_down_accumulator,
                       transform_up_accumulator) == VisitResult::kStop)
        return VisitResult::kStop;

      transform_down_accumulator = backup_down;
      transform_up_accumulator = backup_up;

      return VisitResult::kContinue;
    }
  };

  RecursiveVisitor recursive_visitor(visitor);

  recursive_visitor(*this, SkMatrix::I(), SkMatrix::I());
}

void Widget::VisitAtPoint(vec2 point, WidgetVisitor &visitor) {

  struct PointVisitor : WidgetVisitor {
    vec2 point;
    WidgetVisitor &orig_visitor;

    PointVisitor(vec2 point, WidgetVisitor &orig_visitor)
        : point(point), orig_visitor(orig_visitor) {}

    VisitResult operator()(Widget &widget, const SkMatrix &transform_down,
                           const SkMatrix &transform_up) override {
      auto shape = widget.Shape();
      SkPoint mapped_point = transform_down.mapXY(point.X, point.Y);
      if (shape.isEmpty() || shape.contains(mapped_point.fX, mapped_point.fY)) {
        if (orig_visitor(widget, transform_down, transform_up) ==
            VisitResult::kStop)
          return VisitResult::kStop;
      }
      return VisitResult::kContinue;
    }
  };

  PointVisitor point_visitor(point, visitor);

  VisitAll(point_visitor);
}

struct FunctionWidgetVisitor : WidgetVisitor {
  WidgetVisitorFunc func;
  animation::State *state;
  FunctionWidgetVisitor(WidgetVisitorFunc visitor,
                        animation::State *state = nullptr)
      : func(visitor), state(state) {}
  VisitResult operator()(Widget &widget, const SkMatrix &transform_down,
                         const SkMatrix &transform_up) override {
    return func(widget, transform_down, transform_up);
  }
  animation::State *AnimationState() override { return state; }
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
    FunctionWidgetVisitor visitor([&](Widget &widget,
                                      const SkMatrix &transform_down,
                                      const SkMatrix &transform_up) {
      if (&widget == this) {
        ret = transform_down;
        return VisitResult::kStop;
      }
      return VisitResult::kContinue;
    });
    parent->VisitImmediateChildren(visitor);
  }
  return ret;
}

SkMatrix Widget::TransformToParent() {
  SkMatrix ret = SkMatrix::I();
  if (Widget *parent = ParentWidget()) {
    FunctionWidgetVisitor visitor([&](Widget &widget,
                                      const SkMatrix &transform_down,
                                      const SkMatrix &transform_up) {
      if (&widget == this) {
        ret = transform_up;
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

SkMatrix Widget::TransformFromChild(Widget *child) {
  SkMatrix transform;
  while (child && child != this) {
    transform.preConcat(child->TransformToParent());
    child = child->ParentWidget();
  }
  if (child != this) {
    return SkMatrix::I();
  }
  return transform;
}

void Widget::DrawChildren(SkCanvas &canvas,
                          animation::State &animation_state) const {
  FunctionWidgetVisitor visitor(
      [&](Widget &widget, const SkMatrix &transform_down,
          const SkMatrix &transform_up) {
        canvas.save();
        canvas.concat(transform_up);
        widget.Draw(canvas, animation_state);
        canvas.restore();
        return VisitResult::kContinue;
      },
      &animation_state);
  const_cast<Widget *>(this)->VisitImmediateChildren(visitor);
}

} // namespace automaton::gui