// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

namespace automat {

// Mechanism for controlling various search loops.
enum class ControlFlow : char {
  VisitChildren = 0,  // Continue the search including children of the current element.
  SkipChildren = 1,   // Continue the search but don't visit the children of the current element.
  StopSearching = 2,  // Stop the search immediately.
};

}  // namespace automat