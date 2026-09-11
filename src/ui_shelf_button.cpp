// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "ui_shelf_button.hpp"

#include "object.hpp"
#include "object_source.hpp"

namespace automat::ui {

ShelfButton::ShelfButton(Widget* parent, Ptr<Object> proto)
    : Toy(parent, *proto, nullptr, proto->wake_counter), proto(std::move(proto)) {}

void ShelfButton::Init() {
  proto_widget = &ToyStore().FindOrMake(*proto, this);
  layers.OrderInside(proto_widget);
}

SkPath ShelfButton::Shape() const { return proto_widget->Shape(); }

RRect ShelfButton::CoarseBounds() const { return proto_widget->CoarseBounds(); }

Interface ShelfButton::FindOption(Pointer&, ActionTrigger trigger) {
  if (trigger == PointerButton::Left) return Interface(*proto, kMakeObject);
  return {};
}

}  // namespace automat::ui
