// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "on_off.hpp"

namespace automat {

bool OnOff::Table::DefaultCanSync(Syncable, Syncable other) {
  return other.table->kind >= Interface::kOnOff && other.table->kind <= Interface::kLastOnOff;
}

}  // namespace automat
