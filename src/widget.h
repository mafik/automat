#pragma once

#include <functional>

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>

#include "action.h"
#include "animation.h"
#include "keyboard.h"
#include "pointer.h"

namespace automat::gui {

struct Widget;

using Path = std::vector<Widget *>;

enum class VisitResult { kContinue, kStop };

using Visitor = std::function<VisitResult(Widget &)>;

SkMatrix TransformDown(const Path &path, animation::State *state = nullptr);
SkMatrix TransformUp(const Path &path, animation::State *state = nullptr);

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
  virtual VisitResult VisitChildren(Visitor &visitor) {
    return VisitResult::kContinue;
  }

  virtual SkMatrix TransformToChild(const Widget *child,
                                    animation::State *state = nullptr) const {
    return SkMatrix::I();
  }
  virtual SkMatrix TransformFromChild(const Widget *child,
                                      animation::State *state = nullptr) const {
    auto m = TransformToChild(child, state);
    SkMatrix ret = SkMatrix::I();
    bool ignore_failures = m.invert(&ret);
    return ret;
  }

  void DrawChildren(SkCanvas &, animation::State &) const;
};

struct ReparentableWidget : Widget {
  Widget *parent;
  ReparentableWidget(Widget *parent = nullptr);
  Widget *ParentWidget() const override;
  static void TryReparent(Widget *child, Widget *parent);
};

} // namespace automat::gui