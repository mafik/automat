// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "on_off.hh"

#include "svg.hh"
#include "ui_shape_widget.hh"

namespace automat {

static std::unique_ptr<ui::Widget> OnOff_MakeIcon(const Argument&, ui::Widget* parent) {
  return ui::MakeShapeWidget(parent, kPowerSVG, 0);
}

OnOff::OnOff(StrView name, Kind kind) : Syncable(name, kind) {
  can_sync = [](const Syncable&, const Syncable& other) -> bool {
    return other.kind >= Interface::kOnOff && other.kind <= Interface::kLastOnOff;
  };
  make_icon = OnOff_MakeIcon;
}

}  // namespace automat
