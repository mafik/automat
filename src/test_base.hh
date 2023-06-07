#pragma once

#include <gtest/gtest.h>

#include "backtrace.hh"
#include "base.hh"

namespace automat {

struct TestBase : ::testing::Test {
  Location root = Location(nullptr);
  Machine& machine = *root.Create<Machine>();

  TestBase() {
    EnableBacktraceOnSIGSEGV();
    machine.name = "Root Machine";
  }
};

}  // namespace automat
