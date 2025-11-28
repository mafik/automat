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

Task::Task(WeakPtr<Location> target)
    : target(target), predecessors(), successors(global_successors) {
  for (Task* successor : successors) {
    successor->predecessors.push_back(this);
  }
}

void Task::Schedule() {
  ZoneScopedN("Schedule");
  if (scheduled) {
    ERROR << "Task for " << *target.lock() << " already scheduled!";
    return;
  }
  scheduled = true;
  EnqueueTask(this);
}

void Task::Execute(std::unique_ptr<Task> self) {
  ZoneScopedN("Execute");
  scheduled = false;
  if (!successors.empty()) {
    global_successors = successors;
  }
  OnExecute(self);  // may steal self (lol)
  if (self == nullptr) {
    return;
  }
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

void RunTask::OnExecute(std::unique_ptr<Task>& self) {
  ZoneScopedN("RunTask");
  if (auto s = target.lock()) {
    if (Runnable* runnable = s->As<Runnable>()) {
      LongRunning* long_running = s->object->AsLongRunning();
      if (long_running && long_running->IsRunning()) {
        return;
      }
      // Cast the `self` to RunTask for the OnRun invocation
      std::unique_ptr<RunTask> self_as_run_task((RunTask*)self.release());
      runnable->OnRun(*s, self_as_run_task);
      // If OnRun didn't "steal" the ownership then we have to return it back.
      self.reset(self_as_run_task.release());

      if (self) {
        DoneRunning(*s);
      }
    }
  }
}

void RunTask::DoneRunning(Location& here) {
  if (!HasError(*here.object)) {
    ScheduleNext(here);
  }
}

std::string CancelTask::Format() { return f("CancelTask({})", TargetName()); }

void CancelTask::OnExecute(std::unique_ptr<Task>& self) {
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

void UpdateTask::OnExecute(std::unique_ptr<Task>& self) {
  ZoneScopedN("UpdateTask");
  if (auto t = target.lock()) {
    if (auto u = updated.lock()) {
      t->Updated(*u);
    }
  }
}

std::string FunctionTask::Format() { return f("FunctionTask({})", TargetName()); }

void FunctionTask::OnExecute(std::unique_ptr<Task>& self) {
  ZoneScopedN("FunctionTask");
  if (auto t = target.lock()) {
    function(*t);
  }
}
}  // namespace automat
