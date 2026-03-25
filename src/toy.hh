// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <ankerl/unordered_dense.h>

#include <concepts>
#include <functional>
#include <type_traits>

#include "interface.hh"
#include "part.hh"
#include "ptr.hh"
#include "time.hh"
#include "widget.hh"

namespace automat::ui {
struct RootWidget;
}  // namespace automat::ui

namespace automat {

struct Object;

// A type of Widget that represents a memory-managed entity.
//
// Notable subclasses are:
// * ObjectToy (+ its subclasses for specific Objects)
// * ConnectionWidget
// * LocationWidget
struct Toy : ui::Widget {
  WeakPtr<Object> owner;
  Interface::Table* iface;
  uint32_t observed_notify_counter = 0;  // UI-thread only — last seen notify_counter

  Toy(ui::Widget* parent, Object& owner, Interface::Table* iface);

  template <typename T = Object>
  Ptr<T> LockOwner() const {
    return owner.Lock().template Cast<T>();
  }

  template <typename T = Interface>
  T Bind(Object& owner) const {
    if (!iface) return nullptr;
    return T(owner, *static_cast<typename T::Table*>(iface));
  }

  // Alternative to Bind that keeps the owner locked.
  template <typename T = Interface>
  Locked<T> LockBind() const {
    if (auto obj = LockOwner<Object>()) {
      if (iface) {
        // transfer ownership into Locked
        T bound(*obj.Release(), *static_cast<typename T::Table*>(iface));
        return AdoptLocked(bound);
      }
    }
    return {};
  }

  // Walk the parent chain up to LocationWidget, then return the last Toy before that.
  Toy* BaseToy() const;
};

// ToyMaker is a Part that can make toys
template <typename T>
concept ToyMaker = requires(T t) {
  requires Part<T>;
  typename std::remove_reference_t<T>::Toy;
  requires std::derived_from<typename std::remove_reference_t<T>::Toy, Toy>;
  {
    t.MakeToy(std::declval<ui::Widget*>() /*parent*/)
  } -> std::convertible_to<std::unique_ptr<Toy>>;
};

// Mixin class for ToyMakers. Provides some utilities for working with Toys.
struct ToyMakerMixin {
  static void ForEachToyImpl(Object& owner, Interface::Table* iface,
                             std::function<void(ui::RootWidget&, Toy&)> cb);

  // DEPRECATED: This is not thread-safe. Update this Object's local state & call WakeToys instead.
  template <ToyMaker Self>
  void ForEachToy(this Self& self, std::function<void(ui::RootWidget&, typename Self::Toy&)> cb) {
    ForEachToyImpl(
        self.GetOwner(), self.GetInterface(),
        [&](ui::RootWidget& root, automat::Toy& toy) { cb(root, static_cast<Self::Toy&>(toy)); });
  }
};

// ToyMakers can create many toys to display themselves simultaneously in multiple contexts.
// Each context which can display widgets must maintain their lifetime. This class helps with that.
// TODO: delete widgets after some time
struct ToyStore {
  using Key = std::pair<Object*, Interface::Table*>;
  using Map = ankerl::unordered_dense::map<Key, std::unique_ptr<Toy>>;
  Map container;

  template <Part T>
  static Key MakeKey(T& part) {
    return {&part.GetOwner(), part.GetInterface()};
  }

  template <ToyMaker T>
  std::remove_reference_t<T>::Toy* FindOrNull(T&& maker) const {
    auto it = container.find(MakeKey(maker));
    if (it == container.end()) {
      return nullptr;
    }
    return static_cast<std::remove_reference_t<T>::Toy*>(it->second.get());
  }

  // Scan all toys for owners whose generation has changed. Wake those toys.
  // Called once per frame on the UI thread.
  void WakeUpdatedToys(time::SteadyPoint last_wake);

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
        LOG << "Reparenting " << toy.Name() << " from " << toy.parent->Name() << " to "
            << parent->Name();
        toy.Reparent(*parent);
      }
    }
    return static_cast<T::Toy&>(*it->second);
  }
};

}  // namespace automat
