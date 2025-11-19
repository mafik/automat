// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "tasks.hh"

#include <tracy/Tracy.hpp>

#include "argument.hh"
#include "automat.hh"
#include "base.hh"
#include "ui_connection_widget.hh"

namespace automat {

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

Task::Task(WeakPtr<Location> target)
    : target(target), predecessors(), successors(global_successors) {
  for (Task* successor : successors) {
    successor->predecessors.push_back(this);
  }
}

void Task::Schedule() {
  ZoneScopedN("Schedule");
  if (NoScheduling(target.lock().get())) {
    return;
  }
  if (scheduled) {
    ERROR << "Task for " << *target.lock() << " already scheduled!";
    return;
  }
  scheduled = true;
  EnqueueTask(this);
}

void Task::Execute() {
  ZoneScopedN("Execute");
  scheduled = false;
  if (!successors.empty()) {
    global_successors = successors;
  }
  OnExecute();
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
  if (!keep_alive) {
    delete this;
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

std::string RunTask::Format() { return f("RunTask({})", TargetName()); }

void ScheduleNext(Location& source) { ScheduleArgumentTargets(source, next_arg); }

void ScheduleArgumentTargets(Location& source, Argument& arg) {
  // audio::Play(source.object->NextSound());
  for (auto& w : ui::ConnectionWidgetRange(source, arg)) {
    if (w.state) {
      w.state->stabilized = false;
      w.state->lightness_pct = 100;
    }
    w.WakeAnimation();
  }

  arg.LoopLocations<bool>(source, [](Location& next) {
    next.ScheduleRun();
    return false;
  });
}

void RunTask::OnExecute() {
  ZoneScopedN("RunTask");
  schedule_next = true;
  if (auto s = target.lock()) {
    if (Runnable* runnable = s->As<Runnable>()) {
      runnable->Run(*s, *this);
    }
  }
}

std::string CancelTask::Format() { return f("CancelTask({})", TargetName()); }

void CancelTask::OnExecute() {
  ZoneScopedN("CancelTask");
  if (auto s = target.lock()) {
    if (LongRunning* long_running = s->object->AsLongRunning()) {
      long_running->Cancel();
    }
  }
}

std::string UpdateTask::Format() {
  std::string updated_str = updated.lock() ? updated.lock()->ToStr() : "Invalid";
  return f("UpdateTask({}, {})", TargetName(), updated_str);
}

void UpdateTask::OnExecute() {
  ZoneScopedN("UpdateTask");
  if (auto t = target.lock()) {
    if (auto u = updated.lock()) {
      t->Updated(*u);
    }
  }
}

std::string FunctionTask::Format() { return f("FunctionTask({})", TargetName()); }

void FunctionTask::OnExecute() {
  ZoneScopedN("FunctionTask");
  if (auto t = target.lock()) {
    function(*t);
  }
}

NoSchedulingGuard::NoSchedulingGuard(Location& location) : location(location) {
  no_scheduling.insert(&location);
}
NoSchedulingGuard::~NoSchedulingGuard() { no_scheduling.erase(&location); }
}  // namespace automat
