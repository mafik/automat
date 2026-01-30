// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>

#include "ptr.hh"
#include "widget.hh"

namespace automat::ui {
struct RootWidget;
}  // namespace automat::ui

namespace automat {

// Widget interface for objects - defines the contract for widgets that represent objects.
struct ObjectWidget : ui::Widget {
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

  // Describes the area of the widget where the given part is located.
  // Local (metric) coordinates.
  virtual SkPath PartShape(Part*) const { return Shape(); }
};

// Mixin class for things that can create and manage widgets (Objects & some Parts).
// Provides functionality for iterating over widgets and waking their animations.
struct WidgetSource : virtual Part {
  // Produces a new Widget that can display this Part.
  // The `parent` argument allows the Widget to be attached at the correct position in the Widget
  // tree.
  // The `object` argument is a reference to the Object - that manages the lifetime of this part.
  // For most Objects the `object` will be the same as `this` (except for Objects which lifetime is
  // bound to other Objects). For parts the `object` will point to the object that owns the part.
  virtual std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent,
                                                   WeakPtr<ReferenceCounted> object) = 0;

  void ForEachWidget(std::function<void(ui::RootWidget&, ui::Widget&)> cb);
  void WakeWidgetsAnimation();
};

}  // namespace automat
