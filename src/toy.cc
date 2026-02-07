// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "toy.hh"

#include "root_widget.hh"

namespace automat {

void ToyMaker::ForEachToy(std::function<void(ui::RootWidget&, Toy&)> cb) {
  for (auto* root_widget : ui::root_widgets) {
    auto it = root_widget->toys.container.find(ui::ToyStore::MakeKey(GetOwner(), GetAtom()));
    if (it != root_widget->toys.container.end()) {
      cb(*root_widget, *it->second);
    }
  }
}

void ToyMaker::WakeToys() {
  ForEachToy([](ui::RootWidget&, Toy& widget) { widget.WakeAnimation(); });
}

}  // namespace automat
