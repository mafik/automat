#include "gui_align.h"

#include <include/core/SkMatrix.h>

namespace automat::gui {

AlignCenter::AlignCenter(std::unique_ptr<Widget>&& child) : child(std::move(child)) {
  ReparentableWidget::TryReparent(this->child.get(), this);
}

void AlignCenter::Draw(SkCanvas& c, animation::State& s) const { DrawChildren(c, s); }

SkPath AlignCenter::Shape() const { return SkPath(); }

VisitResult AlignCenter::VisitChildren(Visitor& visitor) {
  if (child) {
    return visitor(*child);
  }
  return VisitResult::kContinue;
}
SkMatrix AlignCenter::TransformToChild(const Widget* child_arg, animation::State* state) const {
  if (child_arg != this->child.get()) {
    return SkMatrix::I();
  }
  SkRect bounds = child->Shape().getBounds();
  SkPoint c = bounds.center();
  return SkMatrix::Translate(c);
}

}  // namespace automat::gui