// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "drag_action.hh"
#include "units.hh"
#include "widget.hh"

namespace automat::ui {

struct RootWidget;

// A child of RootWidget that can be used to drop unneeded objects.
struct BlackHole : Widget, DropTarget {
  constexpr static float kMaxRadius = 3_cm;

  float radius;

  BlackHole(RootWidget* parent);
  RootWidget& ParentRootWidget() const;
  SkPath Shape() const override;
  animation::Phase Tick(time::Timer&) override;
  Optional<Rect> TextureBounds() const override { return std::nullopt; }
  void Draw(SkCanvas&) const override;

  // DropTarget interface
  DropTarget* AsDropTarget() override { return this; }

  bool CanDrop(Location&) const override;
  void SnapPosition(Vec2& position, float& scale, Location&, Vec2* fixed_point = nullptr) override;

  // When a location is being dragged around, its still owned by its original Machine. Only when
  // this method is called, the location may be re-parented into the new drop target.
  // The drop target is responsible for re-parenting the location!
  void DropLocation(Ptr<Location>&&) override;
};

}  // namespace automat::ui