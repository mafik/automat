// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "color.hh"
#include "gui_button.hh"
#include "on_off.hh"

namespace automat {

struct Location;

}  // namespace automat

namespace automat::gui {

struct PowerButton : ToggleButton {
  OnOff* target;

  PowerButton(OnOff* target, SkColor fg = "#fa2305"_color, SkColor bg = SK_ColorWHITE);

  void Activate(gui::Pointer& p);
  bool Filled() const override;
};

}  // namespace automat::gui