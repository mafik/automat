#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <memory>

#include "action.hpp"
#include "pointer.hpp"

namespace automat {

std::unique_ptr<Action> MakeMenuAction(ui::Pointer&, OptionsProvider::MiniMenuMode,
                                       const Interface (&options)[ui::kDirCount], Toy* toy);

}  // namespace automat
