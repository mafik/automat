#include "format.hh"

#include "gtest.hh"

using namespace maf;

TEST(FormatTest, BasicTest) {
  std::string result = f("Hello, %s!", "world");
  EXPECT_EQ(result, "Hello, world!");
}
