#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "base.hpp"

namespace automat {

struct TestBase : ::testing::Test {
  Location root = Location(nullptr);
  Board& board = *root.Create<Board>();
};

}  // namespace automat
