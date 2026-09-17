#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <memory>

#include "action.hpp"
#include "interface.hpp"
#include "pointer.hpp"
#include "sincos.hpp"
#include "vec.hpp"

namespace automat {

struct Menu {
  struct Slot {
    SinCos angle;
    Interface option;
  };
  SmallVec<Slot, 8> slots;
  void Place(SinCos angle, Interface option);
  void Place(ui::Dir dir, Interface option);
};

std::unique_ptr<Action> MakeMenuAction(ui::Pointer&, const Menu&, Toy* toy);

}  // namespace automat
