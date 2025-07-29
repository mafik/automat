// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "run_button.hh"

#include <include/core/SkColor.h>

#include "ui_button.hh"
#include "pointer.hh"
#include "svg.hh"

using namespace std;

namespace automat::ui {

PowerButton::PowerButton(Widget* parent, OnOff* target, SkColor fg, SkColor bg)
    : ToggleButton(
          parent,
          make_unique<ColoredButton>(
              this, kPowerSVG,
              ColoredButtonArgs{
                  .fg = bg, .bg = fg, .on_click = [this](ui::Pointer& p) { Activate(p); }}),
          make_unique<ColoredButton>(
              this, kPowerSVG,
              ColoredButtonArgs{
                  .fg = fg, .bg = bg, .on_click = [this](ui::Pointer& p) { Activate(p); }})),
      target(target) {}

void PowerButton::Activate(ui::Pointer& p) {
  target->Toggle();
  WakeAnimation();
}
bool PowerButton::Filled() const { return target->IsOn(); }
}  // namespace automat::ui