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

}  // namespace automat
