// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "ui_shelf_button.hpp"

#include "automat.hpp"  // vm
#include "drag_action.hpp"
#include "location.hpp"
#include "object.hpp"
#include "root_widget.hpp"

namespace automat::ui {

ShelfButton::ShelfButton(Widget* parent, Ptr<Object> proto)
    : Widget(parent), proto(std::move(proto)) {}

void ShelfButton::Init() {
  proto_widget = &ToyStore().FindOrMake(*proto, this);
  layers.OrderInside(proto_widget);
}

SkPath ShelfButton::Shape() const { return proto_widget->Shape(); }

RRect ShelfButton::CoarseBounds() const { return proto_widget->CoarseBounds(); }

void ShelfButton::PointerEnter(Pointer& p) { hand_icon.emplace(p, Pointer::kIconHand); }

void ShelfButton::PointerLeave(Pointer&) { hand_icon.reset(); }

std::unique_ptr<Action> ShelfButton::FindAction(Pointer& p, ActionTrigger btn) {
  if (btn != PointerButton::Left) return nullptr;
  auto obj = proto->Clone();
  p.root_widget.toys.FindOrMake(*obj, this);
  auto loc = MAKE_PTR(Location);
  loc->InsertHere(std::move(obj));
  return std::make_unique<DragLocationAction>(p, std::move(loc));
}

}  // namespace automat::ui
