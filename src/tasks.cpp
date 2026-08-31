// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "tasks.hpp"

#include <atomic>
#include <shared_mutex>
#include <stop_token>
#include <tracy/Tracy.hpp>

#include "argument.hpp"
#include "automat.hpp"
#include "base.hpp"
#include "blockingconcurrentqueue.hpp"
#include "casting.hpp"
#include "error.hpp"
#include "error_recovery.hpp"
#include "memory.hpp"
#include "source_location.hpp"
#include "thread_name.hpp"
#include "time.hpp"
#include "ui_connection_widget.hpp"

namespace automat {

std::vector<Task*> global_successors;
moodycamel::BlockingConcurrentQueue<Task*> queue;

struct NoopTask : Task {
  NoopTask() : Task(nullptr) {}
  void OnExecute(std::unique_ptr<Task>& self) override {}
};

static void AutomatLoop(std::stop_token stop_token) {
  SetThreadName("Automat Loop");
  error_recovery::SignalStack signal_stack;
  while (!stop_token.stop_requested()) {
    Task* task;
    {
      ZoneScopedN("Dequeue");
      queue.wait_dequeue(task);
    }
    WeakPtr<Object> target = task->target;
    ERROR_RECOVERY_TRY { task->Execute(std::unique_ptr<Task>(task)); }
    ERROR_RECOVERY_CATCH(error) {
      Str source_human_readable;
      if (InMainExecutable(error.instruction_pointer)) {
        source_human_readable = f("{:#x}", error.instruction_pointer);
      } else {
        Str file;
        intptr_t offset;
        ResolveAddress(error.instruction_pointer, file, offset);
        if (file.empty()) {
          source_human_readable = f("{:#x}", offset);
        } else {
          source_human_readable = f("{}+{:#x}", file, offset);
        }
      }
      Str message;

      switch (error.type) {
        using enum LowLevelError::Type;
        case NONE:
          break;
        case EXECUTED_UNKNOWN_INSTRUCTION:
          message = f("Executed an unknown instruction at {}", source_human_readable);
          break;
        case READ_PROTECTED_MEMORY:
          message = f("Code at {} read protected memory", source_human_readable);
          break;
        case WROTE_PROTECTED_MEMORY:
          message = f("Code at {} wrote to protected memory", source_human_readable);
          break;
        case EXECUTED_PROTECTED_MEMORY:
          message = f("Executed protected memory at {}", source_human_readable);
          break;
        case READ_UNMAPPED_MEMORY:
          message = f("Code at {} read unmapped memory", source_human_readable);
          break;
        case WROTE_UNMAPPED_MEMORY:
          message = f("Code at {} wrote to unmapped memory", source_human_readable);
          break;
        case EXECUTED_UNMAPPED_MEMORY:
          message = f("Executed unmapped memory at {}", source_human_readable);
          break;
        case ACCESSED_UNALIGNED_MEMORY:
          message = f("Code at {} accessed unaligned memory", source_human_readable);
          break;
        case ARITHMETIC_ERROR:
          message = f("Arithmetic error caused by code at {}", source_human_readable);
          break;
        case STACK_OVERFLOW:
          message = f("Code at {} overflowed the stack", source_human_readable);
          break;
      }
      auto location = error.FindSourceLocation().value_or(SourceLocation());
      if (auto obj = target.Lock()) {
        obj->ReportError(message, location);
      } else {
        ERROR << "Recovered from an error with no target object: " << message;
      }
    }
  }
}

std::vector<std::jthread> worker_threads;

void StartWorkerThreads(std::stop_token stop_token) {
  for (int i = 0; i < 4; ++i) {
    worker_threads.emplace_back(AutomatLoop, stop_token);
  }
}

void JoinWorkerThreads() {
  // Explicit join included for readability only - jthread::~jthread will join the threads
  // automatically anyway.
  // NoopTasks are used to unblock the worker threads that are waiting in wait_dequeue.
  //
  // Note that it can't be called from stop_token's callback because stop_requested is still false
  // when it runs!
  for (int i = 0; i < worker_threads.size(); ++i) {
    (new NoopTask())->Schedule();
  }
  for (auto& thread : worker_threads) {
    thread.join();
  }
  worker_threads.clear();
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

Task::Task(WeakPtr<Object> target)
    : target(target),
      source(nullptr),
      source_interface(nullptr),
      predecessors(),
      successors(global_successors) {
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

void ScheduleNext(Object& source) {
  source.Each<NextArg>([&](NextArg next) {
    ScheduleArgumentTargets(next);
    return LoopControl::Continue;
  });
}

void ScheduleArgumentTargets(Argument arg) {
  // audio::Play(source.object->NextSound());

  arg.state->last_activity.fetch_add(1, std::memory_order_relaxed);

  if (auto next = arg.Find()) {
    // The target may be a Signal sub-interface or the Object itself (its first Signal).
    Signal signal;
    if (auto* s = dyn_cast_if_present<Signal::Table>(next.Get())) {
      signal = Signal(next.Owner<Object>(), s);
    } else if (auto* obj = next.Owner<Object>()) {
      signal = obj->As<Signal>();
    }
    if (signal) {
      signal.ScheduleRun();
    }
  }
  arg.WakeToys();
}

void RunTask::OnExecute(std::unique_ptr<Task>& self) {
  ZoneScopedN("RunTask");
  if (auto s = target.lock()) {
    auto* sig = static_cast<Signal::Table*>(signal);
    if (auto lr = s->As<LongRunning>();
        lr && lr.IsRunning() && sig->while_long_running == Signal::kInhibit) {
      return;
    }
    s->ClearOwnError();
    // Cast the `self` to RunTask for the OnRun invocation
    std::unique_ptr<RunTask> self_as_run_task((RunTask*)self.release());
    if (sig->on_run) {
      sig->on_run(Signal(*s, *sig), self_as_run_task);
    }
    // If OnRun didn't "steal" the ownership then we have to return it back.
    self.reset(self_as_run_task.release());

    if (self) {
      DoneRunning(*s);
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
    if (auto lr = s->As<LongRunning>()) {
      lr.Cancel();
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
