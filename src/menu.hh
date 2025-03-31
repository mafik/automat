// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "action.hh"
#include "vec.hh"

namespace automat {

// Options:
// - Builder
// - Public type for the menu

// Requirements:
// - Must be able to create an Action-bound menu (ephemeral, until Action is finished)

std::unique_ptr<Action> OpenMenu(gui::Pointer& pointer,
                                 maf::Vec<std::unique_ptr<Option>>&& options);

}  // namespace automat