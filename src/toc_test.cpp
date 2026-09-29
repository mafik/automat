// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "basic_command.hpp"
#include "gtest.hpp"
#include "library_file.hpp"
#include "library_number.hpp"
#include "object.hpp"
#include "object_source.hpp"

using namespace automat;
using namespace automat::library;

namespace {

TEST(TocTest, StaticTocIsOneBased) {
  auto number = MAKE_PTR(Number);
  EXPECT_EQ(number->IndexOf(&Number::text_tbl), 1);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(number->IndexOf(&kDigits[i]), 2 + i);
  EXPECT_EQ(number->IndexOf(&kDot), 12);
  EXPECT_EQ(number->IndexOf(&kBackspace), 13);
  EXPECT_EQ(number->InterfaceFromIdx(1).table_ptr, &Number::text_tbl);
  EXPECT_EQ(number->InterfaceFromIdx(13).table_ptr, &kBackspace);
}

TEST(TocTest, ZeroIsTheObject) {
  auto number = MAKE_PTR(Number);
  EXPECT_EQ(number->IndexOf(nullptr), 0);
  Interface whole = number->InterfaceFromIdx(0);
  EXPECT_EQ(whole.object_ptr, number.Get());
  EXPECT_EQ(whole.table_ptr, nullptr);
}

TEST(TocTest, GlobalInterfacesAreNegative) {
  auto number = MAKE_PTR(Number);
  EXPECT_EQ(number->IndexOf(&kMakeObject), -1);
  EXPECT_EQ(number->IndexOf(&kThisIsFine), -2);
  Interface make_object = number->InterfaceFromIdx(-1);
  EXPECT_EQ(make_object.object_ptr, number.Get());
  EXPECT_EQ(make_object.table_ptr, &kMakeObject);
  EXPECT_EQ(number->InterfaceFromIdx(-2).table_ptr, &kThisIsFine);
}

TEST(TocTest, IndicesRoundTrip) {
  auto number = MAKE_PTR(Number);
  for (int i = -global_toc.count; i <= number->toc.size(); ++i) {
    EXPECT_EQ(number->IndexOf(number->InterfaceFromIdx(i).table_ptr), i);
  }
}

TEST(TocTest, TypedAccessCasts) {
  auto number = MAKE_PTR(Number);
  Text text = number->InterfaceFromIdx<Text>(1);
  EXPECT_EQ(text.object_ptr, number.Get());
  EXPECT_EQ(text.table_ptr, &Number::text_tbl);
  EXPECT_EQ(number->InterfaceFromIdx<BasicCommand>(-2).table_ptr, &kThisIsFine);
}

TEST(TocTest, HeapTocKeepsNullSlots) {
  auto file = MAKE_PTR(File);
  EXPECT_EQ(file->IndexOf(&File::toggle_filename_tbl), 2);
  EXPECT_EQ(file->InterfaceFromIdx(2).table_ptr, &File::toggle_filename_tbl);
  Interface image = file->InterfaceFromIdx(1);
  EXPECT_FALSE(image.has_object());
  EXPECT_FALSE(image.has_table());
  EXPECT_EQ(file->IndexOf(&kMakeObject), -1);
  EXPECT_EQ(file->InterfaceFromIdx(-2).table_ptr, &kThisIsFine);
}

}  // namespace
