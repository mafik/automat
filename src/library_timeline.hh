#pragma once

#include <memory>

#include "base.hh"
#include "run_button.hh"

namespace automat::library {

struct Timeline : LiveObject {
  static const Timeline proto;

  gui::RunButton run_button;

  Timeline();
  Timeline(const Timeline&);
  void Relocate(Location* new_here) override;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override;
};

}  // namespace automat::library