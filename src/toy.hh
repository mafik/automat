// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <map>

#include "part.hh"
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
  { t.MakeToy(std::declval<ui::Widget*>() /*parent*/) } -> std::same_as<std::unique_ptr<Toy>>;
};

// Mixin class for things that can create and manage widgets (Objects & some Atoms).
// Provides functionality for iterating over widgets and waking their animations.
struct ToyMaker {
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

  void ForEachToy(std::function<void(ui::RootWidget&, Toy&)> cb);
  void WakeToys();
};

static_assert(ToyPart<ToyMaker>);

// Objects can create many widgets, to display themselves simultaneously in multiple contexts.
// Each context which can display widgets must maintain their lifetime. This class helps with that.
// It can be used either as a mixin or as a member.
// TODO: delete widgets after some time
struct ToyStore {
  using Key = std::pair<WeakPtr<ReferenceCounted>, Atom*>;
  std::map<Key, std::unique_ptr<Toy>> container;

  static Key MakeKey(ReferenceCounted& rc, Atom& atom) {
    return {WeakPtr<ReferenceCounted>(&rc), &atom};
  }

  Toy* FindOrNull(ToyMaker& maker) const {
    auto it = container.find(MakeKey(maker.GetOwner(), maker.GetAtom()));
    if (it == container.end()) {
      return nullptr;
    }
    return it->second.get();
  }

  template <typename T>
  T* FindOrNull(ToyMaker& maker) const {
    return static_cast<T*>(FindOrNull(maker));
  }

  Toy& FindOrMake(ToyMaker& maker, ui::Widget* parent);

  template <typename T>
  T& FindOrMake(ToyMaker& maker, ui::Widget* parent) {
    return static_cast<T&>(FindOrMake(maker, parent));
  }
};

}  // namespace automat
