// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "widget_source.hh"

#include "object.hh"
#include "root_widget.hh"

namespace automat {

void WidgetSource::ForEachWidget(std::function<void(ui::RootWidget&, ui::Widget&)> cb) {
  // WidgetSource is a mixin for Object, so we can safely cast to Object
  auto& self = static_cast<Object&>(*this);
  for (auto* root_widget : ui::root_widgets) {
    if (auto widget = root_widget->widgets.FindOrNull(self)) {
      cb(*root_widget, *widget);
    }
  }
}

void WidgetSource::WakeWidgetsAnimation() {
  ForEachWidget([](ui::RootWidget&, ui::Widget& widget) { widget.WakeAnimation(); });
}

}  // namespace automat
