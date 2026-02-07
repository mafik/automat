// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "toy.hh"

#include "log.hh"
#include "root_widget.hh"

namespace automat {

void ToyMaker::ForEachToy(std::function<void(ui::RootWidget&, Toy&)> cb) {
  for (auto* root_widget : ui::root_widgets) {
    auto it = root_widget->toys.container.find(ToyStore::MakeKey(GetOwner(), GetAtom()));
    if (it != root_widget->toys.container.end()) {
      cb(*root_widget, *it->second);
    }
  }
}

void ToyMaker::WakeToys() {
  ForEachToy([](ui::RootWidget&, Toy& widget) { widget.WakeAnimation(); });
}

Toy& ToyStore::FindOrMake(ToyMaker& maker, ui::Widget* parent) {
  auto key = MakeKey(maker.GetOwner(), maker.GetAtom());
  auto it = container.find(key);
  if (it == container.end()) {
    auto widget = maker.MakeToy(parent);
    it = container.emplace(std::move(key), std::move(widget)).first;
  } else if (it->second->parent != parent) {
    if (it->second->parent == nullptr) {
      it->second->parent = parent->AcquireTrackedPtr();
    } else {
      LOG << parent->Name() << " is asking for a widget for " << maker.GetAtom().Name()
          << " but it's already owned by " << it->second->parent->Name()
          << ". TODO: figure out what to do in this situation";
    }
  }
  return *it->second;
}

}  // namespace automat
