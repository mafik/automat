#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>

#include <functional>

#include "action.hh"
#include "animation.hh"
#include "keyboard.hh"
#include "pointer.hh"
#include "stop.hh"

namespace automat::gui {

struct Widget;

using Path = std::vector<Widget*>;

using Visitor = std::function<MaybeStop(Widget&)>;

SkMatrix TransformDown(const Path& path, animation::Context&);
SkMatrix TransformUp(const Path& path, animation::Context&);

struct DrawContext {
  SkCanvas& canvas;
  animation::Context& animation_context;
  Path path;
  DrawContext(SkCanvas& canvas, animation::Context& actx)
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

  virtual void PointerOver(Pointer&, animation::Context&) {}
  virtual void PointerLeave(Pointer&, animation::Context&) {}
  virtual void Draw(DrawContext&) const = 0;
  virtual SkPath Shape() const = 0;
  virtual std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) { return nullptr; }
  // Return true if the widget should be highlighted as draggable.
  virtual bool CanDrag() { return false; }
  // Iterate over direct child widgets in front-to-back order.
  virtual MaybeStop VisitChildren(Visitor& visitor) { return std::nullopt; }

  virtual SkMatrix TransformToChild(const Widget& child, animation::Context&) const {
    return SkMatrix::I();
  }

  void DrawChildren(DrawContext&) const;
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