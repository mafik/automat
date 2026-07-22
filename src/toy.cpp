// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "toy.hpp"

#include "board.hpp"
#include "location.hpp"
#include "root_widget.hpp"

namespace automat {

Toy::Toy(ui::Widget* parent, Object& owner, Interface::Table* iface)
    : Widget(parent), owner(owner.AcquireWeakPtr()), iface(iface) {}

Toy* Toy::BaseToy() const {
  Widget* base = const_cast<Widget*>((const Widget*)this);
  for (Widget* w = parent; w; w = w->parent) {
    if (dynamic_cast<LocationWidget*>(w)) break;
    base = w;
  }
  return static_cast<Toy*>(base);
}

void ToyMakerMixin::ForEachToyImpl(Object& owner, Interface::Table* iface,
                                   std::function<void(ui::RootWidget&, Toy&)> cb) {
  auto key = ToyStore::Key(&owner, iface);
  for (auto* root_widget : ui::root_widgets) {
    auto it = root_widget->toys.container.find(key);
    if (it != root_widget->toys.container.end()) {
      cb(*root_widget, *it->second);
    }
    for (auto& [root_key, root_toy] : root_widget->toys.container) {
      if (auto* board_widget = dynamic_cast<BoardWidget*>(root_toy.get())) {
        auto board_it = board_widget->toys.container.find(key);
        if (board_it != board_widget->toys.container.end()) {
          cb(*root_widget, *board_it->second);
        }
      }
    }
  }
}

void Toy::Poll(time::Timer& timer) {
  uint32_t current;
  if (iface == nullptr) {  // Object toys
    // Safe to read without locking: memory survives until weak_refs hits 0.
    // Counter at `wake_counter` is valid even after ~Object.
    current = owner.GetUnsafe()->wake_counter.load(std::memory_order_relaxed);
  } else if (auto obj = LockOwner()) {  // True interface toys
    Interface interface(*obj, *iface);
    if (auto arg = dyn_cast<Argument>(interface)) {
      current = arg.state->wake_counter.load(std::memory_order_relaxed);
    } else {
      return;  // Not an Argument toy, no wake_counter to check
    }
  } else {
    // Interface owner has been destroyed
    current = ~observed_wake_counter;
  }
  if (current != observed_wake_counter) {
    observed_wake_counter = current;
    WakeAnimationAt(timer.last);
  }
  OnPoll(timer);
}

void ToyStore::Poll(time::Timer& timer) {
  // Remove dead toys
  std::erase_if(container, [](auto& entry) { return entry.second->dead; });

  for (auto& [key, toy] : container) {
    toy->Poll(timer);
  }
}
}  // namespace automat
