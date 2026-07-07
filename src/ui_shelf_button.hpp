#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "mortal.hpp"
#include "pointer.hpp"
#include "ptr.hpp"
#include "widget.hpp"

namespace automat {
struct Object;
}

namespace automat::ui {

// A shelf entry: hosts a prototype's own widget and mints a draggable clone
// when touched, per docs/parrots/Clone Pile.md.
struct ShelfButton : Widget {
  Ptr<Object> proto;
  MortalPtr<Widget> proto_widget;

  ShelfButton(Widget* parent, Ptr<Object> proto);

  // Creates the hosted widget; call once, after `this` is parented.
  void Init();

  StrView Name() const override { return "ShelfButton"; }
  SkPath Shape() const override;
  RRect CoarseBounds() const override;
  Optional<Rect> TextureBounds() const override { return std::nullopt; }
  bool AllowChildPointerEvents(Widget&) const override { return false; }

  void PointerEnter(Pointer&) override;
  void PointerLeave(Pointer&) override;
  Optional<Pointer::IconOverride> hand_icon;

  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger btn) override;
};

}  // namespace automat::ui
