// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "interface.hpp"

#include "object.hpp"
#include "pointer.hpp"
#include "text_widget.hpp"
#include "toy.hpp"

namespace automat {

Interface::Interface(const NestedPtr<Table>& locked)
    : object_ptr(locked.Owner<Object>()), table_ptr(object_ptr ? locked.Get() : nullptr) {}

Interface::Interface(const NestedWeakPtr<Table>& weak)
    : object_ptr(weak.OwnerUnsafe<Object>()), table_ptr(weak.GetUnsafe()) {}

Interface::operator NestedPtr<Table>() const {
  if (!object_ptr) return {};
  return {object_ptr->AcquirePtr(), table_ptr};
}
Interface::operator NestedWeakPtr<Table>() const {
  if (!object_ptr) return {};
  return {object_ptr->AcquireWeakPtr(), table_ptr};
}

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
  if (table_ptr->activate == nullptr) return nullptr;
  return table_ptr->activate(*this, pointer, toy);
}

std::unique_ptr<ui::Widget> Interface::MakeIcon(ui::Widget* parent) const {
  if (table_ptr == nullptr) return std::make_unique<TextWidget>(parent, Str(object_ptr->Name()));
  return table_ptr->make_icon(*this, parent);
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
