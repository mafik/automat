// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "interface.hh"

#include "object.hh"

namespace automat {

Interface::operator NestedPtr<Table>() { return {object_ptr->AcquirePtr(), table_ptr}; }
Interface::operator NestedWeakPtr<Table>() { return {object_ptr->AcquireWeakPtr(), table_ptr}; }

}  // namespace automat
