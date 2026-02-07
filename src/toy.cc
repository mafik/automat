// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "toy.hh"

#include "root_widget.hh"

namespace automat {

void ToyPartMixin::ForEachToyImpl(ReferenceCounted& owner, Atom& atom,
                                  std::function<void(ui::RootWidget&, Toy&)> cb) {
  for (auto* root_widget : ui::root_widgets) {
    auto it = root_widget->toys.container.find(ToyStore::Key(owner.AcquireWeakPtr(), &atom));
    if (it != root_widget->toys.container.end()) {
      cb(*root_widget, *it->second);
    }
  }
}

}  // namespace automat
