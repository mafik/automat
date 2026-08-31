// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "source_location.hpp"

#include "gtest.hpp"

using namespace automat;

TEST(SourceLocation, OwnedConvertsToStdSourceLocation) {
  SourceLocation owned("some/file.cpp", "SomeFunction", 12, 34);
  std::source_location converted = owned;
  EXPECT_STREQ(converted.file_name(), "some/file.cpp");
  EXPECT_STREQ(converted.function_name(), "SomeFunction");
  EXPECT_EQ(converted.line(), 12u);
  EXPECT_EQ(converted.column(), 34u);
}

TEST(SourceLocation, ViewRoundTripsThroughStdSourceLocation) {
  std::source_location here = std::source_location::current();
  SourceLocation view(here);
  std::source_location back = view;
  EXPECT_STREQ(back.file_name(), here.file_name());
  EXPECT_STREQ(back.function_name(), here.function_name());
  EXPECT_EQ(back.line(), here.line());
  EXPECT_EQ(back.column(), here.column());
}
