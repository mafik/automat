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

  template <ToyMaker Self>
  void ForEachToy(this Self& self, std::function<void(ui::RootWidget&, typename Self::Toy&)> cb) {
    ForEachToyImpl(self.GetOwner(), self.GetAtom(), [&](ui::RootWidget& root, automat::Toy& toy) {
      cb(root, static_cast<Self::Toy&>(toy));
    });
  }

  template <ToyMaker Self>
  void WakeToys(this Self& self) {
    self.ForEachToy([](ui::RootWidget&, typename Self::Toy& t) { t.WakeAnimation(); });
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

  template <ToyMaker T>
  T::Toy& FindOrMake(T& maker, ui::Widget* parent) {
    auto key = MakeKey(maker);
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
    return static_cast<T::Toy&>(*it->second);
  }
};

}  // namespace automat
