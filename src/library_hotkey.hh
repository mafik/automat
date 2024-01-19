#pragma once

#include "base.hh"
#include "keyboard.hh"

namespace automat::library {

struct PowerButton : gui::Button {
  Object* object;
  PowerButton(Object* object);
  void Activate(gui::Pointer&) override;
  bool Filled() const override;
};

struct HotKey : Object {
  static const HotKey proto;

  gui::AnsiKey key = gui::AnsiKey::F11;
  bool ctrl = true;
  bool alt = false;
  bool shift = false;
  bool windows = false;

  PowerButton power_button;

  enum class State {
    Recording,
    Inactive,
    Active,
    Pressed,
  } state = State::Inactive;

  HotKey();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
  void Args(std::function<void(Argument&)> cb) override;
  void Run(Location&) override;

  ControlFlow VisitChildren(gui::Visitor& visitor) override;

  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override;
};

}  // namespace automat::library