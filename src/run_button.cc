// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "run_button.hh"

#include <include/core/SkColor.h>

#include "on_off.hh"
#include "pointer.hh"
#include "svg.hh"
#include "ui_button.hh"

using namespace std;

namespace automat::ui {

PowerButton::PowerButton(Widget* parent, NestedWeakPtr<OnOff> target, SkColor fg, SkColor bg)
    : ToggleButton(parent), target(std::move(target)) {
  on = make_unique<ColoredButton>(
      this, kPowerSVG,
      ColoredButtonArgs{.fg = bg, .bg = fg, .on_click = [this](ui::Pointer& p) { Activate(p); }});
  off = make_unique<ColoredButton>(
      this, kPowerSVG,
      ColoredButtonArgs{.fg = fg, .bg = bg, .on_click = [this](ui::Pointer& p) { Activate(p); }});
}

void PowerButton::Activate(ui::Pointer& p) {
  if (auto locked = target.Lock()) {
    locked->Toggle();
  }
  WakeAnimation();
}
bool PowerButton::Filled() const {
  if (auto locked = target.Lock()) {
    return locked->IsOn();
  }
  return false;
}
}  // namespace automat::ui
