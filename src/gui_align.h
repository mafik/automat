#pragma once

#include "widget.h"

namespace automaton::gui {

struct AlignCenter : ReparentableWidget {
  std::unique_ptr<Widget> child;

  AlignCenter(std::unique_ptr<Widget> &&child);
  void Draw(SkCanvas &c, animation::State &s) const override;
  SkPath Shape() const override;
  VisitResult VisitImmediateChildren(WidgetVisitor &visitor) override;
};

std::unique_ptr<Widget> MakeAlignCenter(std::unique_ptr<Widget> &&child);

} // namespace automaton::gui