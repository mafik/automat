// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace automat {

struct Location;

// Schedules all of the Locations pointed by the "next" argument from the "source" Location.
void ScheduleNext(Location& source);

struct Task {
  Location* target;
  std::vector<Task*> predecessors;
  std::vector<Task*> successors;
  bool scheduled = false;
  Task(Location* target);
  virtual ~Task() {}
  // Add this task to the task queue.
  void Schedule();
  void PreExecute();
  void PostExecute();
  virtual std::string Format();
  virtual void Execute() = 0;
};

struct RunTask : Task {
  RunTask(Location* target) : Task(target) {}
  std::string Format() override;
  void Execute() override;
};

struct CancelTask : Task {
  CancelTask(Location* target) : Task(target) {}
  std::string Format() override;
  void Execute() override;
};

struct UpdateTask : Task {
  Location* updated;
  UpdateTask(Location* target, Location* updated) : Task(target), updated(updated) {}
  std::string Format() override;
  void Execute() override;
};

struct FunctionTask : Task {
  std::function<void(Location&)> function;
  FunctionTask(Location* target, std::function<void(Location&)> function)
      : Task(target), function(function) {}
  std::string Format() override;
  void Execute() override;
};

struct ErroredTask : Task {
  Location* errored;
  ErroredTask(Location* target, Location* errored) : Task(target), errored(errored) {}
  std::string Format() override;
  void Execute() override;
};

}  // namespace automat