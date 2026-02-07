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

// A type of Widget that represents a Part (Atom owned by a ReferenceCounted).
struct Toy : ui::Widget {
  using ui::Widget::Widget;

  // Get the default scale that this object would like to have.
  // Usually it's 1 but when it's iconified, it may want to shrink itself.
  virtual float GetBaseScale() const = 0;

  // Places where the connections to this widget may terminate.
  // Local (metric) coordinates.
  virtual void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const = 0;

  // Returns the start position of the given argument.
  // If coordinate_space is nullptr, returns local (metric) coordinates.
  // If coordinate_space is provided, returns coordinates in that widget's space.
  virtual Vec2AndDir ArgStart(const Argument&, ui::Widget* coordinate_space = nullptr);

  // Describes the area of the widget where the given atom is located.
  // Local (metric) coordinates.
  virtual SkPath AtomShape(Atom*) const { return Shape(); }
};

// ToyPart is a Part that can make toys
template <typename T>
concept ToyPart = requires(T t) {
  requires Part<T>;
  typename T::Toy;
  requires std::derived_from<typename T::Toy, Toy>;
  {
    t.MakeToy(std::declval<ui::Widget*>() /*parent*/)
  } -> std::convertible_to<std::unique_ptr<Toy>>;
};

// Mixin class for ToyParts. Provides some utilities for working with Toys.
struct ToyPartMixin {
  static void ForEachToyImpl(ReferenceCounted& owner, Atom& atom,
                             std::function<void(ui::RootWidget&, Toy&)> cb);

  template <ToyPart Self>
  void ForEachToy(this Self& self, std::function<void(ui::RootWidget&, typename Self::Toy&)> cb) {
    ForEachToyImpl(self.GetOwner(), self.GetAtom(), [&](ui::RootWidget& root, automat::Toy& toy) {
      cb(root, static_cast<Self::Toy&>(toy));
    });
  }

  template <ToyPart Self>
  void WakeToys(this Self& self) {
    self.ForEachToy([](ui::RootWidget&, typename Self::Toy& t) { t.WakeAnimation(); });
  }
};

// Interface for things that can create and manage widgets (Objects & some Atoms).
// Provides functionality for iterating over widgets and waking their animations.
struct ToyMaker : ToyPartMixin {
  using Toy = automat::Toy;
  // Identity for ToyStore keying.
  virtual ReferenceCounted& GetOwner() = 0;
  virtual Atom& GetAtom() = 0;

  // Produces a new Widget that can display this Atom.
  // The `parent` argument allows the Widget to be attached at the correct position in the Widget
  // tree.
  // If constructed Toy needs to access this Atom (almost always yes), then it should do
  // so through NestedWeakPtr, using the 2nd argument as the reference counter.
  virtual std::unique_ptr<Toy> MakeToy(ui::Widget* parent) = 0;
};

static_assert(ToyPart<ToyMaker>);

// ToyParts can create many widgets, to display themselves simultaneously in multiple contexts.
// Each context which can display widgets must maintain their lifetime. This class helps with that.
// TODO: delete widgets after some time
struct ToyStore {
  using Key = std::pair<WeakPtr<ReferenceCounted>, Atom*>;
  std::map<Key, std::unique_ptr<Toy>> container;

  template <Part T>
  static Key MakeKey(T& part) {
    return {WeakPtr<ReferenceCounted>(&part.GetOwner()), &part.GetAtom()};
  }

  template <ToyPart T>
  T::Toy* FindOrNull(T& maker) const {
    auto it = container.find(MakeKey(maker));
    if (it == container.end()) {
      return nullptr;
    }
    return static_cast<T::Toy*>(it->second.get());
  }

  template <ToyPart T>
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
