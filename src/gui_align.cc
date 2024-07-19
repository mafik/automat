#include "gui_align.hh"

#include <include/core/SkMatrix.h>

#include "control_flow.hh"

namespace automat::gui {

AlignCenter::AlignCenter(std::unique_ptr<Widget>&& child) : child(std::move(child)) {}

void AlignCenter::Draw(DrawContext& ctx) const { DrawChildren(ctx); }

SkPath AlignCenter::Shape(animation::Display*) const { return SkPath(); }

ControlFlow AlignCenter::VisitChildren(Visitor& visitor) {
  if (child) {
    Widget* arr[] = {this->child.get()};
    return visitor(arr);
  }
  return ControlFlow::Continue;
}
SkMatrix AlignCenter::TransformToChild(const Widget& child_arg, animation::Display*) const {
  if (&child_arg != this->child.get()) {
    return SkMatrix::I();
  }
  SkRect bounds = child->Shape(nullptr).getBounds();
  Vec2 c = bounds.center();
  return SkMatrix::Translate(c);
}

}  // namespace automat::gui