// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "ui_shelf_button.hpp"

#include "automat.hpp"  // vm
#include "drag_action.hpp"
#include "location.hpp"
#include "menu.hpp"
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

struct ShelfButtonOption : TextOption {
  ShelfButton& button;
  ShelfButtonOption(ShelfButton& button) : TextOption("New"), button(button) {}
  Ptr<Option> Clone() const override { return MAKE_PTR(ShelfButtonOption, button); }
  Span<const ActionTrigger> Triggers() const override { return kLeftButton; }
  Pointer::Cursor Cursor() const override { return Pointer::Cursor::Hand; }
  std::unique_ptr<Action> Activate(Pointer& p) override {
    auto obj = button.proto->Clone();
    p.root_widget.toys.FindOrMake(*obj, &button);
    auto loc = MAKE_PTR(Location);
    loc->InsertHere(std::move(obj));
    return std::make_unique<DragLocationAction>(p, std::move(loc));
  }
};

void ShelfButton::Options(Pointer&, OptionVisitor& visit) { visit(ShelfButtonOption(*this)); }

}  // namespace automat::ui
