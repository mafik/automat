import <gtest/gtest.h>;
import test_base;
import base;
import library;

using namespace automaton;

struct StartsWithTestTest : TestBase {
  Handle& starts = machine.Create<Text>("starts");
  Handle& with = machine.Create<Text>("with");

  Handle& test = machine.Create<StartsWithTest>();
  StartsWithTestTest() {
    test.ConnectTo(starts, "starts");
    test.ConnectTo(with, "with");
    RunLoop();
  }
};

TEST_F(StartsWithTestTest, StartsWithTrue) {
  starts.SetText("Hello, world!");
  with.SetText("Hello");
  RunLoop();
  EXPECT_EQ("true", test.GetText());
}

TEST_F(StartsWithTestTest, StartsWithFalse) {
  starts.SetText("Hello, world!");
  with.SetText("world");
  RunLoop();
  EXPECT_EQ("false", test.GetText());
}
