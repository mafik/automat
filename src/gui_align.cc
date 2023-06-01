#include "gui_align.h"

#include <include/core/SkMatrix.h>

namespace automat::gui {

AlignCenter::AlignCenter(std::unique_ptr<Widget> &&child)
    : child(std::move(child)) {
  ReparentableWidget::TryReparent(this->child.get(), this);
}

void AlignCenter::Draw(SkCanvas &c, animation::State &s) const {
  DrawChildren(c, s);
}

SkPath AlignCenter::Shape() const { return SkPath(); }

VisitResult AlignCenter::VisitImmediateChildren(WidgetVisitor &visitor) {
  if (child) {
    SkRect bounds = child->Shape().getBounds();
    SkPoint c = bounds.center();
    SkMatrix down = SkMatrix::Translate(c);
    SkMatrix up = SkMatrix::Translate(-c);
    return visitor(*child, down, up);
  }
  return VisitResult::kContinue;
}

} // namespace automat::gui