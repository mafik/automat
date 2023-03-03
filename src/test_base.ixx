export module test_base;

import <gtest/gtest.h>;
import backtrace;
import base;

export namespace automaton {

struct TestBase : ::testing::Test {
  Location root = Location(nullptr);
  Machine &machine = *root.Create<Machine>();

  TestBase() {
    EnableBacktraceOnSIGSEGV();
    machine.name = "Root Machine";
  }
};

} // namespace automaton
