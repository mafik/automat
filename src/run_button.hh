#pragma once

#include "gui_button.hh"
#include "on_off.hh"

namespace automat {

struct Location;

}  // namespace automat

namespace automat::gui {

// Make sure to set the `location` or the button won't work.
struct RunLocationMixin : virtual ToggleButton {
  Location* location;
  RunLocationMixin(Location* parent = nullptr);
  void Activate(Pointer&) override;
  bool Filled() const override;
};

struct RunOnOffMixin : virtual ToggleButton {
  OnOff* target;
  RunOnOffMixin(OnOff* target);
  void Activate(gui::Pointer&) override { target->Toggle(); }
  bool Filled() const override { return target->IsOn(); }
};

struct RunButton : RunLocationMixin, CircularButtonMixin {
  std::unique_ptr<Widget> child;
  RunButton(Location* parent = nullptr, float radius = kMinimalTouchableSize / 2);
  Widget* Child() const override { return child.get(); }
};

struct PowerButton : RunOnOffMixin {
  std::unique_ptr<Widget> child;
  PowerButton(OnOff* target);
  SkColor ForegroundColor() const override { return "#fa2305"_color; }
  Widget* Child() const override { return child.get(); }
};

}  // namespace automat::gui