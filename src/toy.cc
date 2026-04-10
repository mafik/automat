// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "toy.hh"

#include "location.hh"
#include "root_widget.hh"

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
  for (auto* root_widget : ui::root_widgets) {
    auto it = root_widget->toys.container.find(ToyStore::Key(&owner, iface));
    if (it != root_widget->toys.container.end()) {
      cb(*root_widget, *it->second);
    }
  }
}

void ToyStore::Tick(time::Timer& timer) {
  // Remove dead toys
  std::erase_if(container, [](auto& entry) { return entry.second->dead; });

  // Wake toys whose owners have updated state
  for (auto& [key, toy] : container) {
    auto [rc, iface] = key;
    uint32_t current;
    if (iface == nullptr) {  // Object toys
      // Safe to read through `rc`: WeakPtr in the Toy keeps memory alive.
      // Counter at `wake_counter` is valid even after ~Object.
      current = rc->wake_counter.load(std::memory_order_relaxed);
    } else if (auto ptr = toy->LockOwner()) {  // True interface toys
      Interface interface(static_cast<Object&>(*ptr), *iface);
      if (auto arg = dyn_cast<Argument>(interface)) {
        current = arg.state->wake_counter.load(std::memory_order_relaxed);
      } else {
        continue;  // Not an Argument toy, no wake_counter to check
      }
    } else {
      // Interface owner has been destroyed
      current = ~toy->observed_notify_counter;
    }
    if (current != toy->observed_notify_counter) {
      toy->observed_notify_counter = current;
      toy->WakeAnimationAt(timer.last);
    }
  }
}
}  // namespace automat
