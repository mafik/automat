// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "color.hh"
#include "interfaces.hh"
#include "ui_button.hh"

namespace automat {

struct Location;

}  // namespace automat

namespace automat::ui {

struct PowerButton : ToggleButton {
  OnOff* target;

  PowerButton(Widget* parent, OnOff* target, SkColor fg = "#fa2305"_color,
              SkColor bg = SK_ColorWHITE);

  void Activate(ui::Pointer& p);
  bool Filled() const override;
};

}  // namespace automat::ui
