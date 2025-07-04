// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <gtest/gtest.h>

#include "base.hh"

namespace automat {

struct TestBase : ::testing::Test {
  Location root = Location(nullptr);
  Machine& machine = *root.Create<Machine>();

  TestBase() { machine.name = "Root Machine"; }
};

}  // namespace automat
