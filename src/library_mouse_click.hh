#pragma once

#include "base.hh"

namespace automat::library {

struct MouseLeftClick : Object {
  MouseLeftClick();
  static const MouseLeftClick proto;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
  void Args(std::function<void(Argument&)> cb) override;
};

}  // namespace automat::library