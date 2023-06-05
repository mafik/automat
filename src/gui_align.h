#pragma once

#include "widget.h"

namespace automat::gui {

struct AlignCenter : ReparentableWidget {
  std::unique_ptr<Widget> child;

  AlignCenter(std::unique_ptr<Widget>&& child);
  void Draw(SkCanvas& c, animation::State& s) const override;
  SkPath Shape() const override;
  VisitResult VisitChildren(Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget* child, animation::State* state = nullptr) const override;
};

std::unique_ptr<Widget> MakeAlignCenter(std::unique_ptr<Widget>&& child);

}  // namespace automat::gui