// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "on_off.hh"

#include "svg.hh"
#include "ui_shape_widget.hh"

namespace automat {

static std::unique_ptr<ui::Widget> OnOff_MakeIcon(Argument, ui::Widget* parent) {
  return ui::MakeShapeWidget(parent, kPowerSVG, 0);
}

OnOff::Table::Table(StrView name, Kind kind) : Syncable::Table(name, kind) {
  can_sync = [](Syncable, Syncable other) -> bool {
    return other.table->kind >= Interface::kOnOff && other.table->kind <= Interface::kLastOnOff;
  };
  make_icon = OnOff_MakeIcon;
}

}  // namespace automat
