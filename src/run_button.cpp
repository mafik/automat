// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "run_button.hpp"

#include <include/core/SkColor.h>

#include "on_off.hpp"
#include "pointer.hpp"
#include "svg.hpp"
#include "ui_button.hpp"

using namespace std;

namespace automat::ui {

PowerButton::PowerButton(Widget* parent, NestedWeakPtr<OnOff::Table> target, SkColor4f fg,
                         SkColor4f bg)
    : ToggleButton(parent), target(std::move(target)) {
  on = make_unique<ColoredButton>(this, PathFromSVG(kPowerSVG),
                                  ColoredButtonArgs{.fg = bg, .bg = fg});
  off = make_unique<ColoredButton>(this, PathFromSVG(kPowerSVG),
                                   ColoredButtonArgs{.fg = fg, .bg = bg});
  layers.OrderInside(on.get());
  layers.OrderInside(off.get());
}

Interface PowerButton::FindOption(ui::Pointer&, ui::ActionTrigger trigger) {
  if (trigger != PointerButton::Left) return {};
  auto locked = target.Lock();
  if (!locked) return {};
  OnOff on_off(locked.Owner<Object>(), locked.Get());
  return Interface(on_off.object_ptr, on_off.IsOn() ? &locked->turn_off : &locked->turn_on);
}
bool PowerButton::Filled() const {
  if (auto locked = target.Lock()) {
    return OnOff(locked.Owner<Object>(), locked.Get()).IsOn();
  }
  return false;
}
}  // namespace automat::ui
