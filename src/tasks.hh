// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace automat {

struct Location;
struct Argument;

// Schedules all of the Locations pointed by the "next" argument from the "source" Location.
void ScheduleNext(Location& source);
void ScheduleArgumentTargets(Location& source, Argument&);

struct Task {
  std::weak_ptr<Location> target;
  std::vector<Task*> predecessors;
  std::vector<Task*> successors;
  bool scheduled = false;
  Task(std::weak_ptr<Location> target);
  virtual ~Task() {}
  // Add this task to the task queue.
  void Schedule();
  void PreExecute();
  void PostExecute();
  virtual std::string Format();
  virtual void Execute() = 0;
  std::string TargetName();
};

struct RunTask : Task {
  RunTask(std::weak_ptr<Location> target) : Task(target) {}
  std::string Format() override;
  void Execute() override;
};

struct CancelTask : Task {
  CancelTask(std::weak_ptr<Location> target) : Task(target) {}
  std::string Format() override;
  void Execute() override;
};

struct UpdateTask : Task {
  std::weak_ptr<Location> updated;
  UpdateTask(std::weak_ptr<Location> target, std::weak_ptr<Location> updated)
      : Task(target), updated(updated) {}
  std::string Format() override;
  void Execute() override;
};

struct FunctionTask : Task {
  std::function<void(Location&)> function;
  FunctionTask(std::weak_ptr<Location> target, std::function<void(Location&)> function)
      : Task(target), function(function) {}
  std::string Format() override;
  void Execute() override;
};

struct ErroredTask : Task {
  std::weak_ptr<Location> errored;
  ErroredTask(std::weak_ptr<Location> target, std::weak_ptr<Location> errored)
      : Task(target), errored(errored) {}
  std::string Format() override;
  void Execute() override;
};

struct NextGuard {
  std::vector<Task*> successors;
  std::vector<Task*> old_global_successors;
  NextGuard(std::vector<Task*>&& successors);
  ~NextGuard();
};

// Sometimes objects are updated automatically (for example by their LiveArguments). This class
// allows such objects to block auto-scheduling and enable them to alter the values of their
// arguments without triggering re-runs.
struct NoSchedulingGuard {
  Location& location;
  NoSchedulingGuard(Location& location);
  ~NoSchedulingGuard();
};

struct LogTasksGuard {
  LogTasksGuard();
  ~LogTasksGuard();
};

}  // namespace automat