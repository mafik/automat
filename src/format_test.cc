#include "format.h"

#include <gtest/gtest.h>

TEST(FormatTest, BasicTest) {
  std::string result = f("Hello, %s!", "world");
  EXPECT_EQ(result, "Hello, world!");
}
