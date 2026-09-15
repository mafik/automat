#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "color.hpp"
#include "on_off.hpp"
#include "ptr.hpp"
#include "ui_button.hpp"

namespace automat::ui {

struct PowerButton : ToggleButton {
  Linked<OnOff> target;

  PowerButton(Widget* parent, Linked<OnOff> target, SkColor4f fg = "#fa2305"_color4f,
              SkColor4f bg = SkColors::kWhite);

  Interface FindOption(ui::Pointer&, ui::ActionTrigger) override;
  bool Filled() const override;
};

}  // namespace automat::ui
