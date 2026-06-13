// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "color.hpp"
#include "on_off.hpp"
#include "ptr.hpp"
#include "ui_button.hpp"

namespace automat::ui {

struct PowerButton : ToggleButton {
  NestedWeakPtr<OnOff::Table> target;

  PowerButton(Widget* parent, NestedWeakPtr<OnOff::Table> target, SkColor4f fg = "#fa2305"_color4f,
              SkColor4f bg = SkColors::kWhite);

  void Activate(ui::Pointer& p);
  bool Filled() const override;
};

}  // namespace automat::ui
