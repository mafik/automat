#pragma once

#include "base.hh"
#include "pointer.hh"

namespace automat::library {

struct MouseClick : Object, Runnable {
  gui::PointerButton button;
  bool down;
  MouseClick(gui::PointerButton, bool down);
  static const MouseClick lmb_down;
  static const MouseClick lmb_up;
  static const MouseClick rmb_down;
  static const MouseClick rmb_up;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape(animation::Display*) const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
  void Args(std::function<void(Argument&)> cb) override;
  LongRunning* OnRun(Location&) override;
};

}  // namespace automat::library