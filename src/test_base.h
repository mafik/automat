#pragma once

#include "backtrace.h"
#include "base.h"
#include <gtest/gtest.h>

namespace automat {

struct TestBase : ::testing::Test {
  Location root = Location(nullptr);
  Machine &machine = *root.Create<Machine>();

  TestBase() {
    EnableBacktraceOnSIGSEGV();
    machine.name = "Root Machine";
  }
};

} // namespace automat
