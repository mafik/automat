// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "toy.hh"

#include "object.hh"
#include "root_widget.hh"

namespace automat {

void ToyMaker::ForEachToy(std::function<void(ui::RootWidget&, Toy&)> cb) {
  // WidgetSource is a mixin for Object, so we can safely cast to Object
  auto& self = static_cast<Object&>(*this);
  for (auto* root_widget : ui::root_widgets) {
    if (auto toy = root_widget->toys.FindOrNull(self)) {
      cb(*root_widget, *toy);
    }
  }
}

void ToyMaker::WakeToys() {
  ForEachToy([](ui::RootWidget&, Toy& widget) { widget.WakeAnimation(); });
}

}  // namespace automat
