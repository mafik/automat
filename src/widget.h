#pragma once

#include <functional>

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>

#include "action.h"
#include "animation.h"
#include "keyboard.h"
#include "pointer.h"

namespace automaton::gui {

struct Widget;

enum class VisitResult { kContinue, kStop };

struct WidgetVisitor {
  virtual ~WidgetVisitor() {}
  virtual VisitResult operator()(Widget &, const SkMatrix &transform_down,
                                 const SkMatrix &transform_up) = 0;
  virtual animation::State *AnimationState() { return nullptr; }
};

using WidgetVisitorFunc =
    std::function<VisitResult(Widget &, const SkMatrix &, const SkMatrix &)>;

// Base class for widgets.
struct Widget {
  Widget() {}
  virtual ~Widget() {}
  virtual Widget *ParentWidget() const { return nullptr; }

  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual std::string_view Name() const {
    const std::type_info &info = typeid(*this);
    return info.name();
  }

  virtual void PointerOver(Pointer &, animation::State &) {}
  virtual void PointerLeave(Pointer &, animation::State &) {}
  virtual void Draw(SkCanvas &, animation::State &) const = 0;
  virtual void DrawColored(SkCanvas &canvas, animation::State &state,
                           const SkPaint &) const {
    // Default implementation just calls Draw().
    Draw(canvas, state);
  }
  virtual SkPath Shape() const = 0;
  virtual std::unique_ptr<Action> ButtonDownAction(Pointer &, PointerButton) {
    return nullptr;
  }
  // Return true if the widget should be highlighted as draggable.
  virtual bool CanDrag() { return false; }
  // Iterate over direct child widgets in front-to-back order.
  virtual VisitResult VisitImmediateChildren(WidgetVisitor &visitor) {
    return VisitResult::kContinue;
  }
  void VisitAll(WidgetVisitor &visitor);
  void VisitAll(WidgetVisitorFunc visitor);
  void VisitAtPoint(vec2 point, WidgetVisitor &visitor);
  void VisitAtPoint(vec2 point, WidgetVisitorFunc visitor);

  // Transform matrix is controlled by the Widget's parent.
  // This function asks the parent for the transform of this Widget.
  SkMatrix TransformFromParent(animation::State *state = nullptr) const;
  SkMatrix TransformToParent(animation::State *state = nullptr) const;

  // The child doesn't have to be an immediate child.
  SkMatrix TransformToChild(const Widget *child,
                            animation::State *state = nullptr) const;
  SkMatrix TransformFromChild(const Widget *child,
                              animation::State *state = nullptr) const;

  void DrawChildren(SkCanvas &, animation::State &) const;
};

struct ReparentableWidget : Widget {
  Widget *parent;
  ReparentableWidget(Widget *parent = nullptr);
  Widget *ParentWidget() const override;
  static void TryReparent(Widget *child, Widget *parent);
};

} // namespace automaton::gui