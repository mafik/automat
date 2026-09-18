// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "tasks.hpp"

#include <semaphore>
#include <stop_token>
#include <thread>
#include <vector>

#include "gtest.hpp"

using namespace automat;
using namespace std::chrono_literals;

namespace {

struct IdleTask : Task {
  IdleTask() : Task(nullptr) {}
  void OnExecute(std::unique_ptr<Task>&) override {}
};

struct SignalTask : Task {
  std::counting_semaphore<>& ran;
  SignalTask(std::counting_semaphore<>& ran) : Task(nullptr), ran(ran) {}
  void OnExecute(std::unique_ptr<Task>&) override { ran.release(); }
};

struct TaskParentTest : testing::Test {
  std::stop_source stop_source;
  std::counting_semaphore<> parent_ran{0};

  void SetUp() override { StartWorkerThreads(stop_source.get_token()); }

  void TearDown() override {
    stop_source.request_stop();
    JoinWorkerThreads();
  }
};

TEST_F(TaskParentTest, RunsAfterItsLastChild) {
  auto* parent = new SignalTask(parent_ran);
  auto kept_child = std::make_unique<IdleTask>();
  kept_child->SetParent(*parent);
  auto* executed_child = new IdleTask();
  executed_child->SetParent(*parent);
  parent->Schedule();
  executed_child->Schedule();
  EXPECT_FALSE(parent_ran.try_acquire_for(100ms));
  kept_child.reset();
  EXPECT_TRUE(parent_ran.try_acquire_for(5s));
}

TEST_F(TaskParentTest, WaitsForScheduleAfterItsChildrenFinished) {
  auto* parent = new SignalTask(parent_ran);
  auto child = std::make_unique<IdleTask>();
  child->SetParent(*parent);
  child.reset();
  EXPECT_FALSE(parent_ran.try_acquire_for(100ms));
  parent->Schedule();
  EXPECT_TRUE(parent_ran.try_acquire_for(5s));
}

TEST_F(TaskParentTest, RunsOnceWhenChildrenFinishOnManyThreads) {
  auto* parent = new SignalTask(parent_ran);
  std::vector<std::unique_ptr<Task>> children;
  for (int i = 0; i < 16; ++i) {
    children.push_back(std::make_unique<IdleTask>());
    children.back()->SetParent(*parent);
  }
  parent->Schedule();
  std::vector<std::jthread> threads;
  for (auto& child : children) {
    threads.emplace_back([child = std::move(child)]() mutable { child.reset(); });
  }
  threads.clear();
  EXPECT_TRUE(parent_ran.try_acquire_for(5s));
  EXPECT_FALSE(parent_ran.try_acquire_for(100ms));
}

}  // namespace
