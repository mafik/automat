// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "tasks.hh"

#include "argument.hh"
#include "audio.hh"
#include "automat.hh"
#include "base.hh"
#include "time.hh"

using namespace maf;

namespace automat {

int log_executed_tasks = 0;

LogTasksGuard::LogTasksGuard() { ++log_executed_tasks; }
LogTasksGuard::~LogTasksGuard() { --log_executed_tasks; }

std::vector<Task*> global_successors;

NextGuard::NextGuard(std::vector<Task*>&& successors) : successors(std::move(successors)) {
  old_global_successors = global_successors;
  global_successors = this->successors;
}
NextGuard::~NextGuard() {
  assert(global_successors == successors);
  global_successors = old_global_successors;
  for (Task* successor : successors) {
    auto& pred = successor->predecessors;
    if (pred.empty()) {
      successor->Schedule();
    }
  }
}

std::unordered_set<Location*> no_scheduling;

static bool NoScheduling(Location* location) {
  return no_scheduling.find(location) != no_scheduling.end();
}

Task::Task(std::weak_ptr<Location> target)
    : target(target), predecessors(), successors(global_successors) {
  for (Task* successor : successors) {
    successor->predecessors.push_back(this);
  }
}

void Task::Schedule() {
  if (NoScheduling(target.lock().get())) {
    return;
  }
  if (log_executed_tasks) {
    LOG << "Scheduling " << Format();
  }
  if (scheduled) {
    ERROR << "Task for " << *target.lock() << " already scheduled!";
    return;
  }
  scheduled = true;
  EnqueueTask(this);
}

void Task::PreExecute() {
  scheduled = false;
  if (log_executed_tasks) {
    LOG << Format();
    LOG_Indent();
  }
  if (!successors.empty()) {
    global_successors = successors;
  }
}

void Task::PostExecute() {
  if (!global_successors.empty()) {
    assert(global_successors == successors);
    global_successors.clear();
    for (Task* successor : successors) {
      auto& pred = successor->predecessors;
      auto it = std::find(pred.begin(), pred.end(), this);
      assert(it != pred.end());
      pred.erase(it);
      if (pred.empty()) {
        successor->Schedule();
      }
    }
  }
  if (log_executed_tasks) {
    LOG_Unindent();
  }
}

std::string Task::TargetName() {
  if (auto s = target.lock()) {
    return s->ToStr();
  } else {
    return "Invalid";
  }
}

std::string Task::Format() { return "Task()"; }

std::string RunTask::Format() { return f("RunTask(%s)", TargetName().c_str()); }

void ScheduleNext(Location& source) { ScheduleArgumentTargets(source, next_arg); }

void ScheduleArgumentTargets(Location& source, Argument& arg) {
  audio::Play(source.object->NextSound());
  source.last_finished = time::SteadyClock::now();
  // TODO: maybe there is a better way to do this...
  arg.InvalidateConnectionWidgets(source);  // so that the "next" connection flashes

  arg.LoopLocations<bool>(source, [](Location& next) {
    next.ScheduleRun();
    return false;
  });
}

void RunTask::Execute() {
  PreExecute();
  if (auto s = target.lock()) {
    s->Run();
  }
  PostExecute();
}

std::string CancelTask::Format() { return f("CancelTask(%s)", TargetName().c_str()); }

void CancelTask::Execute() {
  if (auto s = target.lock()) {
    if (s->long_running) {
      s->long_running->Cancel();
      s->long_running = nullptr;
    }
  }
  delete this;
}

std::string UpdateTask::Format() {
  std::string updated_str = updated.lock() ? updated.lock()->ToStr() : "Invalid";
  return f("UpdateTask(%s, %s)", TargetName().c_str(), updated_str.c_str());
}

void UpdateTask::Execute() {
  PreExecute();
  if (auto t = target.lock()) {
    if (auto u = updated.lock()) {
      t->Updated(*u);
    }
  }
  PostExecute();
  delete this;
}

std::string FunctionTask::Format() { return f("FunctionTask(%s)", TargetName().c_str()); }

void FunctionTask::Execute() {
  PreExecute();
  if (auto t = target.lock()) {
    function(*t);
  }
  PostExecute();
  delete this;
}

std::string ErroredTask::Format() {
  std::string errored_str = errored.lock() ? errored.lock()->ToStr() : "Invalid";
  return f("ErroredTask(%s, %s)", TargetName().c_str(), errored_str.c_str());
}

void ErroredTask::Execute() {
  PreExecute();
  if (auto t = target.lock()) {
    if (auto e = errored.lock()) {
      t->Errored(*e);
    }
  }
  PostExecute();
  delete this;
}

NoSchedulingGuard::NoSchedulingGuard(Location& location) : location(location) {
  no_scheduling.insert(&location);
}
NoSchedulingGuard::~NoSchedulingGuard() { no_scheduling.erase(&location); }
}  // namespace automat