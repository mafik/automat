#include "widget.h"

#include <bitset>
#include <condition_variable>
#include <vector>

#include <include/core/SkMatrix.h>
#include <include/effects/SkRuntimeEffect.h>

#include "animation.h"
#include "root.h"
#include "time.h"

using namespace automat;

namespace automat::gui {

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

      auto result = widget.VisitImmediateChildren(*this);
      if (result == VisitResult::kStop) {
        return VisitResult::kStop;
      }

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

SkMatrix Widget::TransformFromParent(animation::State *state) const {
  if (Widget *parent = ParentWidget()) {
    SkMatrix ret;
    FunctionWidgetVisitor visitor(
        [&](Widget &widget, const SkMatrix &transform_down,
            const SkMatrix &transform_up) {
          if (&widget == this) {
            ret = transform_down;
            return VisitResult::kStop;
          }
          return VisitResult::kContinue;
        },
        state);
    auto result = parent->VisitImmediateChildren(visitor);
    if (result == VisitResult::kStop) {
      return ret;
    } else {
      EVERY_N_SEC(10) {
        ERROR() << "Widget::TransformFromParent() failed because "
                << parent->Name() << " (the parent of " << Name()
                << ") didn't return it in its list of children.";
      }
      return SkMatrix::I();
    }
  } else {
    return SkMatrix::I();
  }
}

SkMatrix Widget::TransformToParent(animation::State *state) const {
  SkMatrix ret = SkMatrix::I();
  if (Widget *parent = ParentWidget()) {
    FunctionWidgetVisitor visitor(
        [&](Widget &widget, const SkMatrix &transform_down,
            const SkMatrix &transform_up) {
          if (&widget == this) {
            ret = transform_up;
            return VisitResult::kStop;
          }
          return VisitResult::kContinue;
        },
        state);
    parent->VisitImmediateChildren(visitor);
  }
  return ret;
}

static vec2 Vec2(SkPoint p) { return vec2{p.fX, p.fY}; }

SkMatrix Widget::TransformToChild(const Widget *child,
                                  animation::State *state) const {
  SkMatrix transform;
  while (child && child != this) {
    transform.preConcat(child->TransformFromParent(state));
    child = child->ParentWidget();
  }
  if (child != this) {
    EVERY_N_SEC(10) {
      std::string parents = "";
      for (const Widget *it = child->ParentWidget(); it;
           it = it->ParentWidget()) {
        parents += it->Name();
        parents += " → ";
      }
      std::string children = "";
      FunctionWidgetVisitor visitor([&](Widget &widget,
                                        const SkMatrix &transform_down,
                                        const SkMatrix &transform_up) {
        if (!children.empty()) {
          children += ", ";
        }
        children += widget.Name();
        return VisitResult::kContinue;
      });
      const_cast<Widget *>(this)->VisitImmediateChildren(visitor);
      ERROR() << "Widget::TransformToChild() failed because " << child->Name()
              << " is not a child of " << Name() << ". Parents of "
              << child->Name() << " are: " << parents << ". Children of "
              << Name() << " are: " << children << ".";
    }
    return SkMatrix::I();
  }
  return transform;
}

SkMatrix Widget::TransformFromChild(const Widget *child,
                                    animation::State *state) const {
  SkMatrix transform;
  while (child && child != this) {
    transform.preConcat(child->TransformToParent(state));
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

ReparentableWidget::ReparentableWidget(Widget *parent) : parent(parent) {}

Widget *ReparentableWidget::ParentWidget() const { return parent; }

void ReparentableWidget::TryReparent(Widget *child, Widget *parent) {
  if (auto reparentable_child = dynamic_cast<ReparentableWidget *>(child)) {
    reparentable_child->parent = parent;
  }
}

void VisitPath(const Path &path, WidgetVisitor &visitor) {
  if (path.empty()) {
    return;
  }
  if (visitor(*path[0], SkMatrix::I(), SkMatrix::I()) == VisitResult::kStop) {
    return;
  }
  for (int i = 1; i < path.size(); ++i) {
    Widget *parent = path[i - 1];
    Widget *expected_child = path[i];

    std::optional<VisitResult> result;
    FunctionWidgetVisitor selective_visitor(
        [&](Widget &child, const SkMatrix &transform_down,
            const SkMatrix &transform_up) {
          if (&child == expected_child) {
            result = visitor(child, transform_down, transform_up);
            return VisitResult::kStop;
          }
          return VisitResult::kContinue;
        },
        visitor.AnimationState());

    parent->VisitImmediateChildren(selective_visitor);

    if (!result.has_value()) {
      result = visitor(*expected_child, SkMatrix::I(), SkMatrix::I());
    }

    if (*result == VisitResult::kStop) {
      return;
    }
  }
}

SkMatrix TransformDown(const Path &path, animation::State *state) {
  SkMatrix ret = SkMatrix::I();
  FunctionWidgetVisitor visitor(
      [&](Widget &widget, const SkMatrix &transform_down,
          const SkMatrix &transform_up) {
        ret.postConcat(transform_down);
        return VisitResult::kContinue;
      },
      state);
  VisitPath(path, visitor);
  return ret;
}

SkMatrix TransformUp(const Path &path, animation::State *state) {
  SkMatrix ret = SkMatrix::I();
  FunctionWidgetVisitor visitor(
      [&](Widget &widget, const SkMatrix &transform_down,
          const SkMatrix &transform_up) {
        ret.postConcat(transform_up);
        return VisitResult::kContinue;
      },
      state);
  VisitPath(path, visitor);
  return ret;
}

} // namespace automat::gui