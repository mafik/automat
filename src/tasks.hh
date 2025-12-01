// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include "ptr.hh"
#include "status.hh"

namespace automat {

struct Location;
struct Argument;
struct Task;

void StartWorkerThreads(std::stop_token);

// If you already requested a stop, this function can be used to wait for all the worker threads to
// stop.
//
// Note that it doesn't request stop - you have to do that using the stop_token passed to
// StartWorkerThreads.
void JoinWorkerThreads();

// Schedules all of the Locations pointed by the "next" argument from the "source" Location.
void ScheduleNext(Location& source);
void ScheduleArgumentTargets(Location& source, Argument&);

struct Task {
  WeakPtr<Location> target;
  std::vector<Task*> predecessors;
  std::vector<Task*> successors;
  bool scheduled = false;  // only used for error detection
  Task(WeakPtr<Location> target);
  virtual ~Task() {}
  // Add this task to the task queue.
  //
  // Steals ownership of this object.
  void Schedule();
  virtual std::string Format();
  virtual void OnExecute(std::unique_ptr<Task>& self) = 0;
  void Execute(std::unique_ptr<Task> self);
  std::string TargetName();
};

struct RunTask : Task {
  RunTask(WeakPtr<Location> target) : Task(target) {}
  std::string Format() override;
  void OnExecute(std::unique_ptr<Task>& self) override;

  void DoneRunning(Location& here);
};

struct CancelTask : Task {
  CancelTask(WeakPtr<Location> target) : Task(target) {}
  std::string Format() override;
  void OnExecute(std::unique_ptr<Task>& self) override;
};

struct UpdateTask : Task {
  WeakPtr<Location> updated;
  UpdateTask(WeakPtr<Location> target, WeakPtr<Location> updated)
      : Task(target), updated(updated) {}
  std::string Format() override;
  void OnExecute(std::unique_ptr<Task>& self) override;
};

struct FunctionTask : Task {
  std::function<void(Location&)> function;
  FunctionTask(WeakPtr<Location> target, std::function<void(Location&)> function)
      : Task(target), function(function) {}
  std::string Format() override;
  void OnExecute(std::unique_ptr<Task>& self) override;
};

struct NextGuard {
  std::vector<Task*> successors;
  std::vector<Task*> old_global_successors;
  NextGuard(std::vector<Task*>&& successors);
  ~NextGuard();
};

}  // namespace automat
