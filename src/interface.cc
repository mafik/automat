// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "interface.hh"

#include "object.hh"

namespace automat {

Interface::operator NestedPtr<Table>() {
  if (!object_ptr) return {};
  return {object_ptr->AcquirePtr(), table_ptr};
}
Interface::operator NestedWeakPtr<Table>() {
  if (!object_ptr) return {};
  return {object_ptr->AcquireWeakPtr(), table_ptr};
}

Str ToStr(Interface iface) {
  auto obj_name = iface.has_object() ? Str(iface.object_ptr->Name()) : "null";
  auto iface_name = iface.has_table() ? Str(iface.Name()) : "null";
  return obj_name + "." + iface_name;
}

}  // namespace automat
