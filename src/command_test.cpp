// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "command.hpp"

#include <atomic>
#include <semaphore>
#include <stop_token>
#include <thread>

#include "base.hpp"
#include "error.hpp"
#include "gtest.hpp"
#include "tasks.hpp"

using namespace automat;
using namespace std::chrono_literals;

namespace {

struct Step : Object {
  std::counting_semaphore<> ran{0};
  std::thread::id ran_on;
  bool fail = false;

  DEF_INTERFACE(Step, Command, run, "Run")
  static constexpr int NextIdx() { return SlotOf(next_tbl); }
  void OnRun() {
    obj->ran_on = std::this_thread::get_id();
    if (obj->fail) obj->ReportError("Step failed");
    obj->ran.release();
  }
  DEF_END(run);

  DEF_INTERFACE(Step, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(run_tbl, next_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Step); }
};

struct Job : Object {
  std::atomic<bool> is_running = false;
  std::binary_semaphore began{0};

  DEF_INTERFACE(Job, OnOff, running, "Running")
  bool IsOn() const { return obj->is_running; }
  static constexpr int TurnOnIdx() { return SlotOf(run_tbl); }
  static constexpr int TurnOffIdx() { return SlotOf(cancel_tbl); }
  DEF_END(running);

  DEF_INTERFACE(Job, Command, run, "Run")
  static constexpr int NextIdx() { return SlotOf(next_tbl); }
  static constexpr int CancelIdx() { return SlotOf(cancel_tbl); }
  Locked<Command> OnRun() {
    obj->is_running = true;
    obj->began.release();
    return {};
  }
  DEF_END(run);

  DEF_INTERFACE(Job, Command, cancel, "Cancel")
  void OnRun() { obj->is_running = false; }
  DEF_END(cancel);

  DEF_INTERFACE(Job, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(running_tbl, run_tbl, cancel_tbl, next_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Job); }
};

struct Lamp : Object {
  bool on = false;

  DEF_INTERFACE(Lamp, OnOff, on_off, "On/Off")
  bool IsOn() const { return obj->on; }
  static constexpr int TurnOnIdx() { return SlotOf(switch_on_tbl); }
  static constexpr int TurnOffIdx() { return SlotOf(switch_off_tbl); }
  DEF_END(on_off);

  DEF_INTERFACE(Lamp, Command, switch_on, "Switch on")
  static constexpr int NextIdx() { return SlotOf(next_tbl); }
  void OnRun() { obj->on = true; }
  DEF_END(switch_on);

  DEF_INTERFACE(Lamp, Command, switch_off, "Switch off")
  void OnRun() { obj->on = false; }
  DEF_END(switch_off);

  DEF_INTERFACE(Lamp, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(on_off_tbl, switch_on_tbl, switch_off_tbl, next_tbl);
  Ptr<Object> Clone() const override { return MAKE_PTR(Lamp); }
};

void Execute(Object& object, Command::Table& command) {
  auto* task = new RunTask(object.AcquireWeakPtr(), &command);
  task->Execute(std::unique_ptr<Task>(task));
}

struct CommandTest : testing::Test {
  std::stop_source stop_source;

  void SetUp() override { StartWorkerThreads(stop_source.get_token()); }

  void TearDown() override {
    stop_source.request_stop();
    JoinWorkerThreads();
  }
};

TEST_F(CommandTest, SynchronousChainRunsWithinOneTask) {
  auto a = MAKE_PTR(Step);
  auto b = MAKE_PTR(Step);
  a->next->Connect(b->run.Bind());

  Execute(*a, Step::run_tbl);
  ASSERT_TRUE(b->ran.try_acquire_for(0s));
  EXPECT_EQ(a->ran_on, std::this_thread::get_id());
  EXPECT_EQ(b->ran_on, std::this_thread::get_id());
}

TEST_F(CommandTest, ErrorDuringRunStopsTheChain) {
  auto a = MAKE_PTR(Step);
  auto b = MAKE_PTR(Step);
  a->next->Connect(b->run.Bind());

  a->fail = true;
  Execute(*a, Step::run_tbl);
  EXPECT_TRUE(HasError(*a));
  EXPECT_FALSE(b->ran.try_acquire_for(100ms));

  a->fail = false;
  a->ClearOwnError();
  Execute(*a, Step::run_tbl);
  EXPECT_FALSE(HasError(*a));
  EXPECT_TRUE(b->ran.try_acquire_for(0s));
}

TEST_F(CommandTest, AsynchronousRunSchedulesTheNextCommandWhenDone) {
  auto job = MAKE_PTR(Job);
  auto step = MAKE_PTR(Step);
  job->next->Connect(step->run.Bind());

  job->run->ScheduleRun();
  ASSERT_TRUE(job->began.try_acquire_for(5s));
  EXPECT_FALSE(step->ran.try_acquire_for(100ms));

  job->run->Done();
  EXPECT_TRUE(step->ran.try_acquire_for(5s));
}

TEST_F(CommandTest, CancelAndNextIndicesResolveThroughTheToC) {
  auto job = MAKE_PTR(Job);
  auto step = MAKE_PTR(Step);
  job->next->Connect(step->run.Bind());

  EXPECT_EQ(job->InterfaceFromIdx(Job::run_tbl.cancel_idx).table_ptr, &Job::cancel_tbl);
  EXPECT_EQ(job->InterfaceFromIdx(Job::run_tbl.next_idx).table_ptr, &Job::next_tbl);
  EXPECT_EQ(Job::cancel_tbl.cancel_idx, 0);
  EXPECT_EQ(Job::cancel_tbl.next_idx, 0);

  Locked<Command> next = job->run->NextCommand();
  EXPECT_EQ(next.object_ptr, step.Get());
  EXPECT_EQ(next.table_ptr, &Step::run_tbl);
  EXPECT_EQ(job->next->LastActivity()->load(), 1);
  EXPECT_FALSE(job->cancel->NextCommand());
}

TEST_F(CommandTest, OnOffOverRunAndCancelTogglesTheRunningFlag) {
  auto job = MAKE_PTR(Job);

  job->running->Toggle();
  EXPECT_TRUE(job->is_running);
  EXPECT_TRUE(job->running->IsOn());

  job->running->Toggle();
  EXPECT_FALSE(job->is_running);
  EXPECT_FALSE(job->running->IsOn());
}

TEST_F(CommandTest, TurnOnRunsTheCommandAndItsNextOnTheCallingThread) {
  auto lamp = MAKE_PTR(Lamp);
  auto step = MAKE_PTR(Step);
  lamp->next->Connect(step->run.Bind());

  lamp->on_off->TurnOn();
  EXPECT_TRUE(lamp->on);
  ASSERT_TRUE(step->ran.try_acquire_for(0s));
  EXPECT_EQ(step->ran_on, std::this_thread::get_id());
}

TEST_F(CommandTest, ScheduledOnOffTogglesOnAWorker) {
  auto lamp = MAKE_PTR(Lamp);
  auto step = MAKE_PTR(Step);
  lamp->next->Connect(step->run.Bind());

  lamp->on_off->ScheduleRun();
  ASSERT_TRUE(step->ran.try_acquire_for(5s));
  EXPECT_TRUE(lamp->on);
  EXPECT_NE(step->ran_on, std::this_thread::get_id());
}

}  // namespace
