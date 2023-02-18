export module test_base;

import <gtest/gtest.h>;
import backtrace;
import base;

export namespace automaton {

struct TestBase : ::testing::Test {
  Handle root;
  Machine &machine;

  TestBase() : root(nullptr), machine(*root.Create<Machine>()) {
    EnableBacktraceOnSIGSEGV();
  }
};

} // namespace automaton
