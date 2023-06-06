#pragma once

#include "widget.h"

namespace automat::gui {

struct AlignCenter : Widget {
  std::unique_ptr<Widget> child;

  AlignCenter(std::unique_ptr<Widget>&& child);
  void Draw(DrawContext&) const override;
  SkPath Shape() const override;
  VisitResult VisitChildren(Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override;
};

std::unique_ptr<Widget> MakeAlignCenter(std::unique_ptr<Widget>&& child);

}  // namespace automat::gui