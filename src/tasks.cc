// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "tasks.hh"

#include <shared_mutex>
#include <stop_token>
#include <tracy/Tracy.hpp>

#include "argument.hh"
#include "automat.hh"
#include "base.hh"
#include "blockingconcurrentqueue.hh"
#include "thread_name.hh"
#include "ui_connection_widget.hh"

namespace automat {

std::vector<Task*> global_successors;
moodycamel::BlockingConcurrentQueue<Task*> queue;

struct NoopTask : Task {
  NoopTask() : Task(nullptr) {}
  void OnExecute(std::unique_ptr<Task>& self) override {}
};

static void AutomatLoop(std::stop_token stop_token) {
  SetThreadName("Automat Loop");
  auto stop_callback = std::stop_callback(stop_token, [&]() {
    // noop - wakes up the queue::wait_dequeue
    (new NoopTask())->Schedule();
  });
  while (!stop_token.stop_requested()) {
    Task* task;
    {
      ZoneScopedN("Dequeue");
      queue.wait_dequeue(task);
    }
    task->Execute(std::unique_ptr<Task>(task));
  }
}

std::vector<std::jthread> worker_threads;
std::shared_mutex worker_threads_mtx;

void StartWorkerThreads(std::stop_token stop_token) {
  auto lock = std::unique_lock(worker_threads_mtx);
  for (int i = 0; i < 1; ++i) {
    worker_threads.emplace_back(AutomatLoop, stop_token);
  }
}

void JoinWorkerThreads() {
  auto lock = std::unique_lock(worker_threads_mtx);
  // Explicit join included for readability only - jthread::~jthread will join the threads
  // automatically anyway.
  for (auto& thread : worker_threads) {
    thread.join();
  }
  worker_threads.clear();
}

static bool IsWorkerThread() {
  auto lock = std::shared_lock(worker_threads_mtx);
  for (auto& thread : worker_threads) {
    if (std::this_thread::get_id() == thread.get_id()) {
      return true;
    }
  }
  return false;
}

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

Task::Task(WeakPtr<Object> target) : target(target), predecessors(), successors(global_successors) {
  for (Task* successor : successors) {
    successor->predecessors.push_back(this);
  }
}

static StrView Name(WeakPtr<Object>& weak) {
  if (auto s = weak.Lock()) {
    return s->Name();
  } else {
    return "Invalid";
  }
}

void Task::Schedule() {
  ZoneScopedN("Schedule");
  if (scheduled) {
    ERROR << "Task for " << Name(target) << " already scheduled!";
    return;
  }
  scheduled = true;
  // TODO: moodycamel::concurrentqueue should run faster with explicit producer tokens
  queue.enqueue(this);
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

std::string Task::Format() { return "Task()"; }

std::string RunTask::Format() { return f("RunTask({})", Name(target)); }

void ScheduleNext(Object& source) { ScheduleArgumentTargets(source, next_arg); }

void ScheduleArgumentTargets(Object& source, Argument& arg) {
  // audio::Play(source.object->NextSound());
  for (auto& w : ui::ConnectionWidgetRange(&source, &arg)) {
    if (w.state) {
      w.state->stabilized = false;
      w.state->lightness_pct = 100;
    }
    w.WakeAnimation();
  }

  if (auto next = arg.ObjectOrNull(source)) {
    next->MyLocation()->ScheduleRun();
  }
}

void RunTask::OnExecute(std::unique_ptr<Task>& self) {
  ZoneScopedN("RunTask");
  if (auto s = target.lock()) {
    if (Runnable* runnable = dynamic_cast<Runnable*>(s.get())) {
      LongRunning* long_running = s->AsLongRunning();
      if (long_running && long_running->IsRunning()) {
        return;
      }
      // Cast the `self` to RunTask for the OnRun invocation
      std::unique_ptr<RunTask> self_as_run_task((RunTask*)self.release());
      runnable->OnRun(self_as_run_task);
      // If OnRun didn't "steal" the ownership then we have to return it back.
      self.reset(self_as_run_task.release());

      if (self) {
        DoneRunning(*s);
      }
    }
  }
}

void RunTask::DoneRunning(Object& object) {
  if (!HasError(object)) {
    ScheduleNext(object);
  }
}

std::string CancelTask::Format() { return f("CancelTask({})", Name(target)); }

void CancelTask::OnExecute(std::unique_ptr<Task>& self) {
  ZoneScopedN("CancelTask");
  if (auto s = target.lock()) {
    if (LongRunning* long_running = s->AsLongRunning()) {
      long_running->Cancel();
    }
  }
}

std::string UpdateTask::Format() { return f("UpdateTask({}, {})", Name(target), Name(updated)); }

void UpdateTask::OnExecute(std::unique_ptr<Task>& self) {
  ZoneScopedN("UpdateTask");
  if (auto t = target.lock()) {
    t->Updated(updated);
  }
}

std::string FunctionTask::Format() { return f("FunctionTask({})", Name(target)); }

void FunctionTask::OnExecute(std::unique_ptr<Task>& self) {
  ZoneScopedN("FunctionTask");
  if (auto t = target.lock()) {
    function(*t);
  }
}

}  // namespace automat
