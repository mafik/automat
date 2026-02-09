// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <concepts>
#include <functional>
#include <map>

#include "part.hh"
#include "ptr.hh"
#include "widget.hh"

namespace automat::ui {
struct RootWidget;
}  // namespace automat::ui

namespace automat {

struct Object;

// A type of Widget that represents a memory-managed entity.
//
// Notable subclasses are:
// * Object::Toy (+ its subclasses for specific Objects)
// * ConnectionWidget
// * LocationWidget
struct Toy : ui::Widget {
  WeakPtr<ReferenceCounted> owner;
  Atom* atom;
  uint32_t observed_notify_counter = 0;  // UI-thread only — last seen notify_counter

  Toy(ui::Widget* parent, ReferenceCounted& owner, Atom& atom)
      : Widget(parent), owner(owner.AcquireWeakPtr()), atom(&atom) {}

  template <typename T = ReferenceCounted>
  Ptr<T> LockOwner() const {
    return owner.Lock().template Cast<T>();
  }
};

// ToyMaker is a Part that can make toys
template <typename T>
concept ToyMaker = requires(T t) {
  requires Part<T>;
  typename T::Toy;
  requires std::derived_from<typename T::Toy, Toy>;
  {
    t.MakeToy(std::declval<ui::Widget*>() /*parent*/)
  } -> std::convertible_to<std::unique_ptr<Toy>>;
};

// Mixin class for ToyMakers. Provides some utilities for working with Toys.
struct ToyMakerMixin {
  static void ForEachToyImpl(ReferenceCounted& owner, Atom& atom,
                             std::function<void(ui::RootWidget&, Toy&)> cb);

  // DEPRECATED: This is not thread-safe. Update this Object's local state & call WakeToys instead.
  template <ToyMaker Self>
  void ForEachToy(this Self& self, std::function<void(ui::RootWidget&, typename Self::Toy&)> cb) {
    ForEachToyImpl(self.GetOwner(), self.GetAtom(), [&](ui::RootWidget& root, automat::Toy& toy) {
      cb(root, static_cast<Self::Toy&>(toy));
    });
  }
};

// ToyMakers can create many toys to display themselves simultaneously in multiple contexts.
// Each context which can display widgets must maintain their lifetime. This class helps with that.
// TODO: delete widgets after some time
struct ToyStore {
  using Key = std::pair<WeakPtr<ReferenceCounted>, Atom*>;
  std::map<Key, std::unique_ptr<Toy>> container;

  template <Part T>
  static Key MakeKey(T& part) {
    return {WeakPtr<ReferenceCounted>(&part.GetOwner()), &part.GetAtom()};
  }

  template <ToyMaker T>
  T::Toy* FindOrNull(T& maker) const {
    auto it = container.find(MakeKey(maker));
    if (it == container.end()) {
      return nullptr;
    }
    return static_cast<T::Toy*>(it->second.get());
  }

  // Scan all toys for owners whose generation has changed. Wake those toys.
  // Called once per frame on the UI thread.
  void WakeUpdatedToys() {
    for (auto& [key, toy] : container) {
      auto& [owner_weak, atom] = key;
      // Safe to read through WeakPtr: memory survives until weak_refs hits 0.
      auto* rc = owner_weak.GetUnsafe<ReferenceCounted>();
      if (rc == nullptr) continue;
      uint32_t current = rc->wake_counter.load(std::memory_order_relaxed);
      if (current != toy->observed_notify_counter) {
        toy->observed_notify_counter = current;
        toy->WakeAnimation();
      }
    }
  }

  template <ToyMaker T>
  T::Toy& FindOrMake(T& maker, ui::Widget* parent) {
    auto key = MakeKey(maker);
    auto it = container.find(key);
    if (it == container.end()) {
      auto widget = maker.MakeToy(parent);
      it = container.emplace(std::move(key), std::move(widget)).first;
    } else if (auto& toy = *it->second; toy.parent != parent) {
      if (toy.parent == nullptr) {
        toy.parent = parent->AcquireTrackedPtr();
      } else {
        toy.Reparent(*parent);
      }
    }
    return static_cast<T::Toy&>(*it->second);
  }
};

}  // namespace automat
