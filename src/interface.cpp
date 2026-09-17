// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "interface.hpp"

#include "library_switch.hpp"
#include "menu.hpp"
#include "object.hpp"
#include "object_source.hpp"
#include "pointer.hpp"
#include "text_widget.hpp"
#include "toy.hpp"

namespace automat {

std::unique_ptr<Action> Interface::Activate(ui::Pointer& pointer, Toy* toy) const {
  if (table_ptr == nullptr) {
    for (ui::Widget* widget = toy; widget; widget = widget->parent) {
      auto* candidate = dynamic_cast<Toy*>(widget);
      if (candidate && candidate->owner.GetUnsafe() == object_ptr) {
        return candidate->OpenMenu(pointer);
      }
    }
    return nullptr;
  }
  if (table_ptr->activate) return table_ptr->activate(*this, pointer, toy);
  return OpenMenu(pointer, toy);
}

void Interface::FillMenu(Menu& menu) const {
  if (table_ptr == nullptr || table_ptr->fill_menu == nullptr) return;
  table_ptr->fill_menu(*this, menu);
}

std::unique_ptr<Action> Interface::OpenMenu(ui::Pointer& pointer, Toy* toy) const {
  if (table_ptr == nullptr || table_ptr->fill_menu == nullptr) return nullptr;
  Menu menu;
  table_ptr->fill_menu(*this, menu);
  return MakeMenuAction(pointer, menu, toy);
}

std::unique_ptr<ui::Widget> Interface::MakeIcon(ui::Widget* parent) const {
  if (table_ptr == nullptr) return std::make_unique<TextWidget>(parent, Str(object_ptr->Name()));
  return table_ptr->make_icon(*this, parent);
}

Ptr<Object> Interface::MakeController() const {
  if (auto on_off = dyn_cast<OnOff>(*this)) {
    return MAKE_PTR(library::LinkedSwitch, cast<OnOff>(*this));
  }
  return nullptr;
}

std::unique_ptr<Action> Interface::DragNewController(ui::Pointer& pointer,
                                                     ui::Widget& birthplace) const {
  auto controller = MakeController();
  if (!controller) return nullptr;
  auto toy = controller->MakeToy(&birthplace);
  return DragNew(pointer, std::move(controller), std::move(toy));
}

std::unique_ptr<ui::Widget> Interface::Table::DefaultMakeIcon(Interface self, ui::Widget* parent) {
  return std::make_unique<TextWidget>(parent, Str(self.Name()));
}

Str ToStr(Interface iface) {
  auto obj_name = iface.has_object() ? Str(iface.object_ptr->Name()) : "null";
  auto iface_name = iface.has_table() ? Str(iface.Name()) : "null";
  return obj_name + "." + iface_name;
}

}  // namespace automat
