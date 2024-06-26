#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>

#include <functional>

#include "action.hh"
#include "animation.hh"
#include "control_flow.hh"
#include "keyboard.hh"
#include "pointer.hh"
#include "span.hh"

namespace automat::gui {

struct Widget;

using Path = std::vector<Widget*>;

using Visitor = std::function<ControlFlow(maf::Span<Widget*>)>;

SkMatrix TransformDown(const Path& path, animation::Display*);
SkMatrix TransformUp(const Path& path, animation::Display*);

struct DrawContext {
  SkCanvas& canvas;
  animation::Display& animation_context;
  Path path;
  DrawContext(SkCanvas& canvas, animation::Display& actx)
      : canvas(canvas), animation_context(actx), path() {}
};

// Base class for widgets.
struct Widget {
  Widget() {}
  virtual ~Widget() {}

  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual std::string_view Name() const {
    const std::type_info& info = typeid(*this);
    return info.name();
  }

  virtual void PointerOver(Pointer&, animation::Display&) {}
  virtual void PointerLeave(Pointer&, animation::Display&) {}
  virtual void Draw(DrawContext&) const = 0;
  virtual SkPath Shape() const = 0;
  virtual std::unique_ptr<Action> CaptureButtonDownAction(Pointer&, PointerButton) {
    return nullptr;
  }
  virtual std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) { return nullptr; }

  // Return true if the widget should be highlighted as draggable.
  virtual bool CanDrag() { return false; }

  // Used to visit the child widgets in a generic fashion.
  // Widgets are stored in front-to-back order.
  // The function stops once the visitor returns ControlFlow::Stop.
  virtual ControlFlow VisitChildren(Visitor& visitor) { return ControlFlow::Continue; }

  // Return true if the widget's children should be drawn outside of its bounds.
  virtual bool ChildrenOutside() const { return false; }

  virtual SkMatrix TransformToChild(const Widget& child, animation::Display*) const {
    return SkMatrix::I();
  }

  void DrawChildren(DrawContext&) const;
};

struct LabelMixin {
  virtual void SetLabel(maf::StrView label) = 0;
};

struct PaintMixin {
  SkPaint paint;
  virtual ~PaintMixin() {}
  static SkPaint* Get(Widget* widget) {
    if (auto p = dynamic_cast<PaintMixin*>(widget)) {
      return &p->paint;
    }
    return nullptr;
  }
};

}  // namespace automat::gui