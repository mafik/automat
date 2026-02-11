#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "on_off.hh"

#include "svg.hh"
#include "ui_shape_widget.hh"

namespace automat {

std::unique_ptr<ui::Widget> OnOff::MakeIcon(ui::Widget* parent) {
  return ui::MakeShapeWidget(parent, kPowerSVG, 0);
}

}  // namespace automat
