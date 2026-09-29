// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "command.hpp"
#include "gtest.hpp"
#include "library_file.hpp"
#include "library_number.hpp"
#include "library_switch.hpp"
#include "library_timeline.hpp"
#include "menu.hpp"
#include "object.hpp"
#include "object_source.hpp"

using namespace automat;
using namespace automat::library;

namespace {

TEST(TocTest, StaticTocIsOneBased) {
  auto number = MAKE_PTR(Number);
  EXPECT_EQ(number->InterfaceIdx(&Number::text_tbl), 1);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(number->InterfaceIdx(&kDigits[i]), 2 + i);
  EXPECT_EQ(number->InterfaceIdx(&kDot), 12);
  EXPECT_EQ(number->InterfaceIdx(&kBackspace), 13);
  EXPECT_EQ(number->InterfaceFromIdx(1).table_ptr, &Number::text_tbl);
  EXPECT_EQ(number->InterfaceFromIdx(13).table_ptr, &kBackspace);
}

TEST(TocTest, ZeroIsTheObject) {
  auto number = MAKE_PTR(Number);
  EXPECT_EQ(number->InterfaceIdx(nullptr), 0);
  Interface whole = number->InterfaceFromIdx(0);
  EXPECT_EQ(whole.object_ptr, number.Get());
  EXPECT_EQ(whole.table_ptr, nullptr);
}

TEST(TocTest, GlobalInterfacesAreNegative) {
  auto number = MAKE_PTR(Number);
  EXPECT_EQ(number->InterfaceIdx(&kMakeObject), -1);
  EXPECT_EQ(number->InterfaceIdx(&kThisIsFine), -2);
  Interface make_object = number->InterfaceFromIdx(-1);
  EXPECT_EQ(make_object.object_ptr, number.Get());
  EXPECT_EQ(make_object.table_ptr, &kMakeObject);
  EXPECT_EQ(number->InterfaceFromIdx(-2).table_ptr, &kThisIsFine);
}

TEST(TocTest, IndicesRoundTrip) {
  auto number = MAKE_PTR(Number);
  for (int i = -global_toc.count; i <= number->toc.size(); ++i) {
    EXPECT_EQ(number->InterfaceIdx(number->InterfaceFromIdx(i).table_ptr), i);
  }
}

TEST(TocTest, TypedAccessCasts) {
  auto number = MAKE_PTR(Number);
  Text text = number->InterfaceFromIdx<Text>(1);
  EXPECT_EQ(text.object_ptr, number.Get());
  EXPECT_EQ(text.table_ptr, &Number::text_tbl);
  EXPECT_EQ(number->InterfaceFromIdx<Command>(-2).table_ptr, &kThisIsFine);
}

TEST(TocTest, StaticSlotOfMatchesToC) {
  static_assert(Number::SlotOf(Number::text_tbl) == 1);
  static_assert(Number::SlotOf(kBackspace) == 13);
  static_assert(Number::SlotOf(kThisIsFine) == 0);
  auto number = MAKE_PTR(Number);
  for (auto* table : number->toc) {
    EXPECT_EQ(number->toc.SlotOf(table), Number::SlotOf(*table));
    EXPECT_EQ(number->InterfaceIdx(table), Number::SlotOf(*table));
  }
}

TEST(TocTest, OnOffRunsItsCommands) {
  auto sw = MAKE_PTR(Switch);
  EXPECT_EQ(Switch::enabled_tbl.turn_on_idx, sw->InterfaceIdx(&Switch::switch_on_tbl));
  EXPECT_EQ(Switch::enabled_tbl.turn_off_idx, sw->InterfaceIdx(&Switch::switch_off_tbl));
  EXPECT_EQ(sw->InterfaceFromIdx<Command>(Switch::enabled_tbl.turn_on_idx).table_ptr,
            &Switch::switch_on_tbl);
  EXPECT_EQ(sw->InterfaceFromIdx<Command>(Switch::enabled_tbl.turn_off_idx).table_ptr,
            &Switch::switch_off_tbl);
  sw->enabled->TurnOn();
  EXPECT_TRUE(sw->enabled->IsOn());
  sw->enabled->TurnOff();
  EXPECT_FALSE(sw->enabled->IsOn());
}

Interface OptionAt(const Menu& menu, SinCos angle) {
  for (auto& slot : menu.slots) {
    if (slot.angle == angle) return slot.option;
  }
  return {};
}

TEST(TocTest, OnOffMenuPlacesItsCommandAtNorth) {
  auto sw = MAKE_PTR(Switch);
  Menu off_menu;
  sw->enabled.Bind().FillMenu(off_menu);
  EXPECT_EQ(OptionAt(off_menu, 90_deg).table_ptr, &Switch::switch_on_tbl);
  sw->enabled->TurnOn();
  Menu on_menu;
  sw->enabled.Bind().FillMenu(on_menu);
  EXPECT_EQ(OptionAt(on_menu, 90_deg).table_ptr, &Switch::switch_off_tbl);
}

TEST(TocTest, OnOffTrackSlotsMatchItsHeapToC) {
  auto track = MAKE_PTR(OnOffTrack, "Track");
  EXPECT_EQ(track->InterfaceIdx(&OnOffTrack::turn_on_tbl), OnOffTrack::kTurnOnSlot);
  EXPECT_EQ(track->InterfaceIdx(&OnOffTrack::turn_off_tbl), OnOffTrack::kTurnOffSlot);
}

TEST(TocTest, HeapTocKeepsNullSlots) {
  auto file = MAKE_PTR(File);
  EXPECT_EQ(file->InterfaceIdx(&File::toggle_filename_tbl), 2);
  EXPECT_EQ(file->InterfaceFromIdx(2).table_ptr, &File::toggle_filename_tbl);
  Interface image = file->InterfaceFromIdx(1);
  EXPECT_FALSE(image.has_object());
  EXPECT_FALSE(image.has_table());
  EXPECT_EQ(file->InterfaceIdx(&kMakeObject), -1);
  EXPECT_EQ(file->InterfaceFromIdx(-2).table_ptr, &kThisIsFine);
}

}  // namespace
