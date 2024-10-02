// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "tasks.hh"

#include "audio.hh"
#include "base.hh"
#include "time.hh"

using namespace maf;

namespace automat {

Task::Task(Location* target) : target(target), predecessors(), successors(global_successors) {
  for (Task* successor : successors) {
    successor->predecessors.push_back(this);
  }
}

void Task::Schedule() {
  if (NoScheduling(target)) {
    return;
  }
  if (log_executed_tasks) {
    LOG << "Scheduling " << Format();
  }
  assert(!scheduled);
  scheduled = true;
  queue.emplace_back(this);
}

void Task::PreExecute() {
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

std::string Task::Format() { return "Task()"; }

std::string RunTask::Format() { return f("RunTask(%s)", target->ToStr().c_str()); }

void ScheduleNext(Location& source) {
  audio::Play(source.object->NextSound());
  source.last_finished = time::SteadyClock::now();
  // TODO: maybe there is a better way to do this...
  next_arg.InvalidateConnectionWidgets(source);  // so that the "next" connection flashes

  next_arg.LoopLocations<bool>(source, [](Location& next) {
    next.ScheduleRun();
    return false;
  });
}

void RunTask::Execute() {
  PreExecute();
  target->Run();
  PostExecute();
}

std::string CancelTask::Format() { return f("CancelTask(%s)", target->ToStr().c_str()); }

void CancelTask::Execute() {
  if (target->long_running) {
    target->long_running->Cancel();
    target->long_running = nullptr;
  }
  delete this;
}

std::string UpdateTask::Format() {
  return f("UpdateTask(%s, %s)", target->ToStr().c_str(), updated->ToStr().c_str());
}

void UpdateTask::Execute() {
  PreExecute();
  target->Updated(*updated);
  PostExecute();
  delete this;
}

std::string FunctionTask::Format() { return f("FunctionTask(%s)", target->ToStr().c_str()); }

void FunctionTask::Execute() {
  PreExecute();
  function(*target);
  PostExecute();
}

std::string ErroredTask::Format() {
  return f("ErroredTask(%s, %s)", target->ToStr().c_str(), errored->ToStr().c_str());
}

void ErroredTask::Execute() {
  PreExecute();
  target->Errored(*errored);
  PostExecute();
  delete this;
}

}  // namespace automat