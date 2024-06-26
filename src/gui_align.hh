#pragma once

#include "widget.hh"

namespace automat::gui {

struct AlignCenter : Widget {
  std::unique_ptr<Widget> child;

  AlignCenter(std::unique_ptr<Widget>&& child);
  void Draw(DrawContext&) const override;
  SkPath Shape() const override;
  ControlFlow VisitChildren(Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;
};

std::unique_ptr<Widget> MakeAlignCenter(std::unique_ptr<Widget>&& child);

}  // namespace automat::gui