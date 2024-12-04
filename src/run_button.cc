// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "run_button.hh"

#include <include/core/SkColor.h>

#include "gui_button.hh"
#include "pointer.hh"
#include "svg.hh"

using namespace std;

namespace automat::gui {

PowerButton::PowerButton(OnOff* target, SkColor fg, SkColor bg)
    : ToggleButton(
          make_shared<ColoredButton>(
              kPowerSVG,
              ColoredButtonArgs{
                  .fg = bg, .bg = fg, .on_click = [this](gui::Pointer& p) { Activate(p); }}),
          make_shared<ColoredButton>(
              kPowerSVG,
              ColoredButtonArgs{
                  .fg = fg, .bg = bg, .on_click = [this](gui::Pointer& p) { Activate(p); }})),
      target(target) {}

void PowerButton::Activate(gui::Pointer& p) {
  target->Toggle();
  WakeAnimation();
}
bool PowerButton::Filled() const { return target->IsOn(); }
}  // namespace automat::gui