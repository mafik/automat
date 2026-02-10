// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

namespace automat {

enum class LoopControl : char {
  Continue = 0,  // Continue iterating.
  Break = 1,     // Stop iteration immediately.
};

// Mechanism for controlling various search loops.
enum class ControlFlow : char {
  Continue = 0,  // Continue the search but don't visit the children of the current element.
  Break = 1,     // Stop the search immediately.
  Enter = 2,     // Continue the search including children of the current element.
};

}  // namespace automat
