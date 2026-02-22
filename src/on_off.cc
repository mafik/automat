// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "on_off.hh"

#include "svg.hh"
#include "ui_shape_widget.hh"

namespace automat {

bool OnOff::Table::DefaultCanSync(Syncable, Syncable other) {
  return other.table->kind >= Interface::kOnOff && other.table->kind <= Interface::kLastOnOff;
}

std::unique_ptr<ui::Widget> OnOff::Table::DefaultMakeIcon(Argument, ui::Widget* parent) {
  return ui::MakeShapeWidget(parent, kPowerSVG, 0);
}

}  // namespace automat
