// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>

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

}  // namespace automat
