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

struct Bell : Object {
  std::binary_semaphore rang{0};

  DEF_INTERFACE(Bell, Command, ring, "Ring")
  void OnRun() {
    obj->rang.release();
    Done();
  }
  DEF_END(ring);

  INTERFACES(ring_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Bell); }
};

struct Step : Object {
  std::binary_semaphore began{0};
  std::binary_semaphore may_finish{1};

  DEF_INTERFACE(Step, Command, run, "Run")
  void OnRun() {
    obj->began.release();
    obj->may_finish.acquire();
    obj->may_finish.release();
    Done();
  }
  DEF_END(run);

  DEF_INTERFACE(Step, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(run_tbl, next_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Step); }
};

struct Job : Object {
  std::binary_semaphore began{0};
  int cancel_count = 0;

  DEF_INTERFACE(Job, Command, running, "Running")
  void OnRun() { obj->began.release(); }
  void OnCancel() { ++obj->cancel_count; }
  DEF_END(running);

  DEF_INTERFACE(Job, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(running_tbl, next_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Job); }
};

struct Lamp : Object {
  std::binary_semaphore turned_on{0};
  std::binary_semaphore turned_off{0};
  bool on = false;

  DEF_INTERFACE(Lamp, OnOff, on_off, "On/Off")
  bool IsOn() const { return obj->on; }
  static constexpr int TurnOnIdx() { return SlotOf(switch_on_tbl); }
  static constexpr int TurnOffIdx() { return SlotOf(switch_off_tbl); }
  DEF_END(on_off);

  DEF_INTERFACE(Lamp, BasicCommand, switch_on, "Switch on")
  void OnRun() {
    obj->on = true;
    obj->turned_on.release();
  }
  DEF_END(switch_on);

  DEF_INTERFACE(Lamp, BasicCommand, switch_off, "Switch off")
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

struct SyncedJobsTest : SyncTest {
  Ptr<Job> a = MAKE_PTR(Job);
  Ptr<Job> b = MAKE_PTR(Job);
  Ptr<Bell> a_next = MAKE_PTR(Bell);
  Ptr<Bell> b_next = MAKE_PTR(Bell);
  Ptr<Lamp> lamp = MAKE_PTR(Lamp);
  Ptr<Gear> gear = MAKE_PTR(Gear);

  void SetUp() override {
    SyncTest::SetUp();
    a->next->Connect(a_next->ring.Bind());
    b->next->Connect(b_next->ring.Bind());
    gear->FullSync(a->running.Bind());
    gear->FullSync(b->running.Bind());
    gear->FullSync(lamp->on_off.Bind());
  }

  void ExpectBothBegan() {
    ASSERT_TRUE(a->began.try_acquire_for(5s));
    ASSERT_TRUE(b->began.try_acquire_for(5s));
    ASSERT_TRUE(lamp->turned_on.try_acquire_for(5s));
  }

  void ExpectNobodyContinues() {
    EXPECT_FALSE(a_next->rang.try_acquire_for(100ms));
    EXPECT_FALSE(b_next->rang.try_acquire_for(0ms));
  }
};

TEST_F(SyncedJobsTest, FinishedJobWaitsForItsPartner) {
  a->running->ScheduleRun();
  ExpectBothBegan();

  a->running->Done();
  EXPECT_FALSE(a->running->IsRunning());
  EXPECT_TRUE(b->running->IsRunning());
  EXPECT_EQ(b->cancel_count, 0);
  ExpectNobodyContinues();
  EXPECT_TRUE(lamp->on);

  b->running->Done();
  EXPECT_TRUE(a_next->rang.try_acquire_for(5s));
  EXPECT_TRUE(b_next->rang.try_acquire_for(5s));
  EXPECT_FALSE(lamp->on);
}

TEST_F(SyncedJobsTest, LampStartsBothJobs) {
  lamp->on_off->TurnOn();
  ExpectBothBegan();

  b->running->Done();
  ExpectNobodyContinues();
  EXPECT_TRUE(lamp->on);

  a->running->Done();
  EXPECT_TRUE(a_next->rang.try_acquire_for(5s));
  EXPECT_TRUE(b_next->rang.try_acquire_for(5s));
  EXPECT_FALSE(lamp->on);
}

TEST_F(SyncedJobsTest, CancelledJobCancelsItsPartner) {
  lamp->on_off->TurnOn();
  ExpectBothBegan();

  a->running->Cancel();
  EXPECT_FALSE(a->running->IsRunning());
  EXPECT_FALSE(b->running->IsRunning());
  EXPECT_EQ(a->cancel_count, 1);
  EXPECT_EQ(b->cancel_count, 1);
  EXPECT_FALSE(lamp->on);
  ExpectNobodyContinues();
}

TEST_F(SyncedJobsTest, NobodyContinuesAfterCancel) {
  a->running->ScheduleRun();
  ExpectBothBegan();

  a->running->Done();
  b->running->Cancel();
  EXPECT_EQ(a->cancel_count, 0);
  EXPECT_EQ(b->cancel_count, 1);
  EXPECT_FALSE(lamp->on);
  ExpectNobodyContinues();
}

TEST_F(SyncedJobsTest, WaitingJobIgnoresTurnOn) {
  a->running->ScheduleRun();
  ExpectBothBegan();

  a->running->Done();
  a->running->ScheduleRun();
  EXPECT_FALSE(a->began.try_acquire_for(100ms));
  EXPECT_FALSE(a->running->IsRunning());
}

TEST_F(SyncedJobsTest, WaitingJobLeavesItsGear) {
  lamp->on_off->TurnOn();
  ExpectBothBegan();

  a->running->Done();
  a->running->Unsync();
  EXPECT_TRUE(a_next->rang.try_acquire_for(5s));
  a->running->ScheduleRun();
  EXPECT_TRUE(a->began.try_acquire_for(5s));
  EXPECT_TRUE(b->running->IsRunning());
}

TEST_F(SyncedJobsTest, WaitingJobOutlivesItsGear) {
  lamp->on_off->TurnOn();
  ExpectBothBegan();

  a->running->Done();
  gear = nullptr;
  EXPECT_TRUE(a_next->rang.try_acquire_for(5s));
  b->running->Done();
  EXPECT_TRUE(b_next->rang.try_acquire_for(5s));
}

TEST_F(SyncedJobsTest, RunningJobLeavesItsGear) {
  lamp->on_off->TurnOn();
  ExpectBothBegan();

  b->running->Done();
  a->running->Unsync();
  EXPECT_TRUE(a->running->IsRunning());
  EXPECT_TRUE(b_next->rang.try_acquire_for(5s));
  EXPECT_FALSE(lamp->on);

  a->running->Done();
  EXPECT_TRUE(a_next->rang.try_acquire_for(5s));
}

TEST_F(SyncTest, RunningJobsKeepCountingAfterTheirGearsMerge) {
  auto a = MAKE_PTR(Job);
  auto b = MAKE_PTR(Job);
  auto a_next = MAKE_PTR(Bell);
  auto a_gear = MAKE_PTR(Gear);
  auto b_gear = MAKE_PTR(Gear);
  a->next->Connect(a_next->ring.Bind());
  a_gear->FullSync(a->running.Bind());
  b_gear->FullSync(b->running.Bind());
  a->running->ScheduleRun();
  b->running->ScheduleRun();
  ASSERT_TRUE(a->began.try_acquire_for(5s));
  ASSERT_TRUE(b->began.try_acquire_for(5s));

  a_gear->FullSync(b->running.Bind());

  a->running->Done();
  EXPECT_FALSE(a_next->rang.try_acquire_for(100ms));
  b->running->Done();
  EXPECT_TRUE(a_next->rang.try_acquire_for(5s));
}

TEST_F(SyncTest, StepWaitsForSyncedJob) {
  auto step = MAKE_PTR(Step);
  auto job = MAKE_PTR(Job);
  auto step_next = MAKE_PTR(Bell);
  auto gear = MAKE_PTR(Gear);
  step->next->Connect(step_next->ring.Bind());
  gear->FullSync(step->run.Bind());
  gear->FullSync(job->running.Bind());

  step->run->ScheduleRun();
  ASSERT_TRUE(job->began.try_acquire_for(5s));
  EXPECT_FALSE(step_next->rang.try_acquire_for(100ms));

  job->running->Done();
  EXPECT_TRUE(step_next->rang.try_acquire_for(5s));
}

TEST_F(SyncTest, UnsyncedStepContinuesRightAway) {
  auto step = MAKE_PTR(Step);
  auto step_next = MAKE_PTR(Bell);
  step->next->Connect(step_next->ring.Bind());

  step->run->ScheduleRun();
  EXPECT_TRUE(step_next->rang.try_acquire_for(5s));
}

TEST_F(SyncTest, StepCancelledDuringItsRunDoesNotContinue) {
  auto step = MAKE_PTR(Step);
  auto step_next = MAKE_PTR(Bell);
  step->next->Connect(step_next->ring.Bind());
  step->may_finish.acquire();

  step->run->ScheduleRun();
  ASSERT_TRUE(step->began.try_acquire_for(5s));
  step->run->Cancel();
  step->may_finish.release();
  EXPECT_FALSE(step_next->rang.try_acquire_for(100ms));
  EXPECT_FALSE(step->run->IsRunning());
}

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
