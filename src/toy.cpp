// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "toy.hpp"

#include "board.hpp"
#include "location.hpp"
#include "object.hpp"
#include "root_widget.hpp"

namespace automat {

Toy::Toy(ui::Widget* parent, Object& owner, Interface::Table* iface,
         const std::atomic<uint32_t>& monitor)
    : Widget(parent), owner(owner.AcquireWeakPtr()), iface(iface), monitor(monitor) {}

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
  auto key = ToyScope::Key(&owner, iface);
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
  uint32_t current = monitor.load(std::memory_order_relaxed);
  if (current != observed_monitor || owner.IsExpired()) {
    observed_monitor = current;
    WakeAnimationAt(timer.last);
    OnWake();
  }
  OnPoll(timer);
}

void ToyScope::Poll(time::Timer& timer) {
  // Remove dead toys
  std::erase_if(container, [](auto& entry) { return entry.second->dead; });

  for (auto& [key, toy] : container) {
    toy->Poll(timer);
  }
}
}  // namespace automat
