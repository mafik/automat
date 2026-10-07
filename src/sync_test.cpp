// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "sync.hpp"

#include <rapidjson/document.h>

#include <atomic>
#include <semaphore>
#include <stop_token>
#include <vector>

#include "base.hpp"
#include "gtest.hpp"
#include "menu.hpp"
#include "syncable.hpp"
#include "tasks.hpp"

using namespace automat;
using namespace std::chrono_literals;

namespace {

struct Lamp : Object {
  bool on = false;
  int switched_on = 0;
  int switched_off = 0;
  int synced = 0;
  int unsynced = 0;
  std::vector<Lamp*>* order = nullptr;

  DEF_INTERFACE(Lamp, OnOff, on_off, "On/Off")
  bool IsOn() const { return obj->on; }
  static constexpr int TurnOnIdx() { return SlotOf(switch_on_tbl); }
  static constexpr int TurnOffIdx() { return SlotOf(switch_off_tbl); }
  static constexpr int SyncableIdx() { return SlotOf(syncable_tbl); }
  DEF_END(on_off);

  DEF_INTERFACE(Lamp, Command, switch_on, "Switch on")
  void OnRun() {
    if (obj->suspended) return;
    obj->on = true;
    ++obj->switched_on;
    if (obj->order) obj->order->push_back(&*obj);
  }
  DEF_END(switch_on);

  DEF_INTERFACE(Lamp, Command, switch_off, "Switch off")
  void OnRun() {
    if (obj->suspended) return;
    obj->on = false;
    ++obj->switched_off;
  }
  DEF_END(switch_off);

  DEF_INTERFACE(Lamp, Syncable, syncable, "Syncable")
  void OnSync() { ++obj->synced; }
  void OnUnsync() { ++obj->unsynced; }
  DEF_END(syncable);

  INTERFACES(on_off_tbl, switch_on_tbl, switch_off_tbl, syncable_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Lamp, *this); }
};

struct Job : Object {
  std::atomic<bool> is_running = false;
  std::binary_semaphore began{0};
  std::binary_semaphore cancelled{0};

  DEF_INTERFACE(Job, Command, run, "Run")
  static constexpr int NextIdx() { return SlotOf(next_tbl); }
  static constexpr int CancelIdx() { return SlotOf(cancel_tbl); }
  Locked<Command> OnRun() {
    obj->is_running.store(true, std::memory_order_relaxed);
    obj->began.release();
    return {};
  }
  DEF_END(run);

  DEF_INTERFACE(Job, Command, cancel, "Cancel")
  void OnRun() {
    if (obj->is_running.exchange(false, std::memory_order_relaxed)) obj->cancelled.release();
  }
  DEF_END(cancel);

  DEF_INTERFACE(Job, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(run_tbl, cancel_tbl, next_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Job); }

  void Finish() {
    is_running.store(false, std::memory_order_relaxed);
    run->Done();
  }
};

struct Step : Object {
  std::counting_semaphore<> ran{0};

  DEF_INTERFACE(Step, Command, run, "Run")
  static constexpr int NextIdx() { return SlotOf(next_tbl); }
  void OnRun() { obj->ran.release(); }
  DEF_END(run);

  DEF_INTERFACE(Step, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(run_tbl, next_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Step); }
};

Sync::Table* SyncTableOf(Object& object, int idx) {
  for (auto* entry : *object.toc.load(std::memory_order_acquire)) {
    auto* sync = dyn_cast_if_present<Sync::Table>(entry);
    if (sync && sync->attached_toc_idx == idx) return sync;
  }
  return nullptr;
}

Interface OptionAt(const Menu& menu, SinCos angle) {
  for (auto& slot : menu.slots) {
    if (slot.angle == angle) return slot.option;
  }
  return {};
}

struct SyncTest : testing::Test {
  std::stop_source stop_source;

  void SetUp() override { StartWorkerThreads(stop_source.get_token()); }

  void TearDown() override {
    stop_source.request_stop();
    JoinWorkerThreads();
  }
};

TEST_F(SyncTest, TurnOnAndTurnOffReachEveryMember) {
  auto gear = MAKE_PTR(Gear);
  auto a = MAKE_PTR(Lamp);
  auto b = MAKE_PTR(Lamp);
  auto c = MAKE_PTR(Lamp);
  gear->FullSync(a->on_off.Bind());
  gear->FullSync(b->on_off.Bind());
  gear->FullSync(c->on_off.Bind());

  a->on_off->TurnOn();
  EXPECT_TRUE(a->on);
  EXPECT_TRUE(b->on);
  EXPECT_TRUE(c->on);

  b->on_off->TurnOff();
  EXPECT_FALSE(a->on);
  EXPECT_FALSE(b->on);
  EXPECT_FALSE(c->on);
  EXPECT_EQ(a->switched_on, 1);
  EXPECT_EQ(c->switched_off, 1);
}

TEST_F(SyncTest, ToggleForwardsTheTransitionItMakes) {
  auto gear = MAKE_PTR(Gear);
  auto a = MAKE_PTR(Lamp);
  auto b = MAKE_PTR(Lamp);
  gear->FullSync(a->on_off.Bind());
  gear->FullSync(b->on_off.Bind());

  a->on_off->Toggle();
  EXPECT_TRUE(a->on);
  EXPECT_TRUE(b->on);
  a->on_off->Toggle();
  EXPECT_FALSE(a->on);
  EXPECT_FALSE(b->on);
}

TEST_F(SyncTest, NotificationSkipsTheOwnCommand) {
  auto gear = MAKE_PTR(Gear);
  auto a = MAKE_PTR(Lamp);
  auto b = MAKE_PTR(Lamp);
  gear->FullSync(a->on_off.Bind());
  gear->FullSync(b->on_off.Bind());

  a->on = true;
  a->on_off->NotifyTurnedOn();
  EXPECT_TRUE(b->on);
  EXPECT_EQ(a->switched_on, 0);
  EXPECT_FALSE(a->suspended);

  a->on = false;
  a->on_off->NotifyTurnedOff();
  EXPECT_FALSE(b->on);
  EXPECT_EQ(a->switched_off, 0);
}

TEST_F(SyncTest, NotificationOfASuspendedObjectDoesNothing) {
  auto gear = MAKE_PTR(Gear);
  auto a = MAKE_PTR(Lamp);
  auto b = MAKE_PTR(Lamp);
  gear->FullSync(a->on_off.Bind());
  gear->FullSync(b->on_off.Bind());

  auto suspend = Suspend(*a);
  a->on_off->NotifyTurnedOn();
  EXPECT_FALSE(b->on);
}

TEST_F(SyncTest, OneWaySyncForwardsOnlyFromSourcesToSinks) {
  auto gear = MAKE_PTR(Gear);
  auto source = MAKE_PTR(Lamp);
  auto sink = MAKE_PTR(Lamp);
  gear->AddSource(source->on_off.Bind());
  gear->AddSink(sink->on_off.Bind());

  sink->on_off->TurnOn();
  EXPECT_TRUE(sink->on);
  EXPECT_FALSE(source->on);

  sink->on_off->TurnOff();
  source->on_off->TurnOn();
  EXPECT_TRUE(source->on);
  EXPECT_TRUE(sink->on);
}

TEST_F(SyncTest, MembersAreCalledInRingOrderEndingWithTheCaller) {
  std::vector<Lamp*> order;
  auto gear = MAKE_PTR(Gear);
  auto a = MAKE_PTR(Lamp);
  auto b = MAKE_PTR(Lamp);
  auto c = MAKE_PTR(Lamp);
  for (auto* lamp : {a.Get(), b.Get(), c.Get()}) {
    lamp->order = &order;
    gear->FullSync(lamp->on_off.Bind());
  }

  b->on_off->TurnOn();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], c.Get());
  EXPECT_EQ(order[1], a.Get());
  EXPECT_EQ(order[2], b.Get());
}

TEST_F(SyncTest, CompletionCancelsCommandsTurnsOffOnOffsAndContinues) {
  auto gear = MAKE_PTR(Gear);
  auto job = MAKE_PTR(Job);
  auto other = MAKE_PTR(Job);
  auto lamp = MAKE_PTR(Lamp);
  auto step = MAKE_PTR(Step);
  job->next->Connect(step->run.Bind());
  gear->FullSync(job->run.Bind());
  gear->FullSync(other->run.Bind());
  gear->FullSync(lamp->on_off.Bind());

  RunCascade(AcquireLocked(job->run.Bind()));
  EXPECT_TRUE(job->is_running);
  EXPECT_TRUE(other->is_running);
  EXPECT_TRUE(lamp->on);

  job->Finish();
  ASSERT_TRUE(other->cancelled.try_acquire_for(5s));
  ASSERT_TRUE(step->ran.try_acquire_for(5s));
  EXPECT_FALSE(lamp->on);
}

TEST_F(SyncTest, InstantCommandCancelsTheJobItStarts) {
  auto gear = MAKE_PTR(Gear);
  auto step = MAKE_PTR(Step);
  auto job = MAKE_PTR(Job);
  gear->FullSync(step->run.Bind());
  gear->FullSync(job->run.Bind());

  RunCascade(AcquireLocked(step->run.Bind()));
  EXPECT_TRUE(step->ran.try_acquire_for(0s));
  EXPECT_TRUE(job->began.try_acquire_for(0s));
  EXPECT_TRUE(job->cancelled.try_acquire_for(0s));
  EXPECT_FALSE(job->is_running);
}

TEST_F(SyncTest, RemovingAHookFromTheMiddleKeepsTheOthers) {
  auto lamp = MAKE_PTR(Lamp);
  auto a = MAKE_PTR(Lamp);
  auto b = MAKE_PTR(Lamp);
  auto c = MAKE_PTR(Lamp);
  auto on_off_gear = MAKE_PTR(Gear);
  auto switch_on_gear = MAKE_PTR(Gear);
  auto switch_off_gear = MAKE_PTR(Gear);
  on_off_gear->FullSync(lamp->on_off.Bind());
  on_off_gear->FullSync(a->on_off.Bind());
  switch_on_gear->FullSync(lamp->switch_on.Bind());
  switch_on_gear->FullSync(b->switch_on.Bind());
  switch_off_gear->FullSync(lamp->switch_off.Bind());
  switch_off_gear->FullSync(c->switch_off.Bind());
  EXPECT_EQ(lamp->hook_count, 3);

  auto* middle = SyncTableOf(*lamp, Lamp::SlotOf(Lamp::switch_on_tbl));
  ASSERT_NE(middle, nullptr);
  Sync(*lamp, *middle).Disconnect();
  lamp->RemoveHook(*middle);
  EXPECT_EQ(lamp->hook_count, 2);
  EXPECT_EQ(SyncTableOf(*lamp, Lamp::SlotOf(Lamp::switch_on_tbl)), nullptr);

  lamp->on_off->TurnOn();
  EXPECT_TRUE(lamp->on);
  EXPECT_TRUE(a->on);
  EXPECT_FALSE(b->on);

  c->on = true;
  lamp->on_off->TurnOff();
  EXPECT_FALSE(lamp->on);
  EXPECT_FALSE(a->on);
  EXPECT_FALSE(c->on);
}

TEST_F(SyncTest, CopyStartsFromTheBaseToC) {
  auto gear = MAKE_PTR(Gear);
  auto lamp = MAKE_PTR(Lamp);
  auto partner = MAKE_PTR(Lamp);
  gear->FullSync(lamp->on_off.Bind());
  gear->FullSync(partner->on_off.Bind());

  auto copy = lamp->Clone();
  EXPECT_EQ(copy->toc.load(std::memory_order_relaxed), copy->BaseToc());
  EXPECT_EQ(copy->hook_count, 0);
  static_cast<Lamp&>(*copy).on_off->TurnOn();
  EXPECT_TRUE(static_cast<Lamp&>(*copy).on);
  EXPECT_FALSE(partner->on);
}

TEST_F(SyncTest, DestroyedMemberLeavesTheGear) {
  auto gear = MAKE_PTR(Gear);
  auto a = MAKE_PTR(Lamp);
  {
    auto b = MAKE_PTR(Lamp);
    gear->FullSync(a->on_off.Bind());
    gear->FullSync(b->on_off.Bind());
  }
  ASSERT_NE(gear->members, nullptr);
  EXPECT_EQ(gear->members->next, gear->members);
  a->on_off->TurnOn();
  EXPECT_TRUE(a->on);
}

TEST_F(SyncTest, MembersKeepSyncingWithoutAnotherReferenceToTheGear) {
  auto a = MAKE_PTR(Lamp);
  auto b = MAKE_PTR(Lamp);
  {
    auto gear = MAKE_PTR(Gear);
    gear->FullSync(a->on_off.Bind());
    gear->FullSync(b->on_off.Bind());
  }
  a->on_off->TurnOn();
  EXPECT_TRUE(b->on);
}

TEST_F(SyncTest, SyncableIsNotifiedWhileTheOnOffIsASource) {
  auto gear = MAKE_PTR(Gear);
  auto source = MAKE_PTR(Lamp);
  auto sink = MAKE_PTR(Lamp);
  gear->AddSink(sink->on_off.Bind());
  EXPECT_EQ(sink->synced, 0);
  gear->FullSync(source->on_off.Bind());
  EXPECT_EQ(source->synced, 1);

  auto* sync = SyncTableOf(*source, Lamp::SlotOf(Lamp::on_off_tbl));
  ASSERT_NE(sync, nullptr);
  Sync(*source, *sync).Disconnect();
  EXPECT_EQ(source->unsynced, 1);
  EXPECT_EQ(sink->unsynced, 0);
}

TEST_F(SyncTest, SyncedInterfaceOffersItsOwnSyncArgument) {
  auto gear = MAKE_PTR(Gear);
  auto lamp = MAKE_PTR(Lamp);
  auto partner = MAKE_PTR(Lamp);
  int slot = Lamp::SlotOf(Lamp::on_off_tbl);

  Menu before;
  lamp->InterfaceFromIdx(slot).FillMenu(before);
  EXPECT_EQ(OptionAt(before, 0_deg).table_ptr, &kSync);

  gear->FullSync(lamp->on_off.Bind());
  gear->FullSync(partner->on_off.Bind());
  Menu after;
  lamp->InterfaceFromIdx(slot).FillMenu(after);
  EXPECT_EQ(OptionAt(after, 0_deg).table_ptr, SyncTableOf(*lamp, Lamp::SlotOf(Lamp::on_off_tbl)));
  EXPECT_EQ(OptionAt(after, 90_deg).table_ptr, &kToggle);
  EXPECT_EQ(OptionAt(after, 180_deg).table_ptr,
            lamp->InterfaceFromIdx(Lamp::SlotOf(Lamp::switch_on_tbl)).table_ptr);
  EXPECT_EQ(OptionAt(after, 270_deg).table_ptr,
            lamp->InterfaceFromIdx(Lamp::SlotOf(Lamp::switch_off_tbl)).table_ptr);
}

TEST_F(SyncTest, SerializedHooksNameTheGearTheInterfaceAndTheDirections) {
  auto gear = MAKE_PTR(Gear);
  auto lamp = MAKE_PTR(Lamp);
  auto partner = MAKE_PTR(Lamp);
  gear->AddSource(lamp->on_off.Bind());
  gear->FullSync(partner->on_off.Bind());

  rapidjson::StringBuffer buffer;
  ObjectSerializer writer(buffer);
  writer.StartObject();
  writer.Serialize(*lamp);
  writer.EndObject();

  rapidjson::Document doc;
  doc.Parse(buffer.GetString());
  ASSERT_FALSE(doc.HasParseError());
  auto& entry = doc[writer.ResolveName(*lamp).c_str()];
  ASSERT_TRUE(entry.HasMember("hooks"));
  auto& hook = entry["hooks"][0];
  EXPECT_STREQ(hook["gear"].GetString(), writer.ResolveName(*gear).c_str());
  EXPECT_STREQ(hook["interface"].GetString(), "On/Off");
  EXPECT_FALSE(hook["sink"].GetBool());
  EXPECT_TRUE(hook["source"].GetBool());
  EXPECT_TRUE(doc.HasMember(writer.ResolveName(*gear).c_str()));
}

}  // namespace
