// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "sync.hpp"

#include <semaphore>
#include <stop_token>

#include "base.hpp"
#include "gtest.hpp"
#include "tasks.hpp"

using namespace automat;
using namespace std::chrono_literals;

namespace {

struct Lamp : Object {
  std::binary_semaphore turned_on{0};
  std::binary_semaphore turned_off{0};
  bool on = false;

  DEF_INTERFACE(Lamp, OnOff, on_off, "On/Off")
  bool IsOn() const { return obj->on; }
  static constexpr int TurnOnIdx() { return SlotOf(switch_on_tbl); }
  static constexpr int TurnOffIdx() { return SlotOf(switch_off_tbl); }
  DEF_END(on_off);

  DEF_INTERFACE(Lamp, Command, switch_on, "Switch on")
  void OnRun() {
    obj->on = true;
    obj->turned_on.release();
  }
  DEF_END(switch_on);

  DEF_INTERFACE(Lamp, Command, switch_off, "Switch off")
  void OnRun() {
    obj->on = false;
    obj->turned_off.release();
  }
  DEF_END(switch_off);

  INTERFACES(on_off_tbl, switch_on_tbl, switch_off_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Lamp); }
};

struct SyncTest : testing::Test {
  std::stop_source stop_source;

  void SetUp() override { StartWorkerThreads(stop_source.get_token()); }

  void TearDown() override {
    stop_source.request_stop();
    JoinWorkerThreads();
  }
};

TEST_F(SyncTest, SyncedLampsStayOn) {
  auto a = MAKE_PTR(Lamp);
  auto b = MAKE_PTR(Lamp);
  auto gear = MAKE_PTR(Gear);
  gear->FullSync(a->on_off.Bind());
  gear->FullSync(b->on_off.Bind());

  a->on_off->TurnOn();
  ASSERT_TRUE(b->turned_on.try_acquire_for(5s));
  EXPECT_FALSE(b->turned_off.try_acquire_for(100ms));
  EXPECT_TRUE(a->on);
  EXPECT_TRUE(b->on);
}

}  // namespace
