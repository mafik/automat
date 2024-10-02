// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "color.hh"
#include "gui_button.hh"
#include "gui_constants.hh"
#include "on_off.hh"

namespace automat {

struct Location;

}  // namespace automat

namespace automat::gui {

struct RunButton : ToggleButton {
  Location* location;

  RunButton(Location* parent = nullptr, float radius = kMinimalTouchableSize / 2);

  SkPath Shape(animation::Display*) const override {
    return SkPath::Circle(kMinimalTouchableSize / 2, kMinimalTouchableSize / 2,
                          kMinimalTouchableSize / 2);
  }
  bool Filled() const override;
};

struct PowerButton : ToggleButton {
  OnOff* target;

  PowerButton(OnOff* target, SkColor fg = "#fa2305"_color, SkColor bg = SK_ColorWHITE);

  void Activate(gui::Pointer& p);
  bool Filled() const override;
};

}  // namespace automat::gui