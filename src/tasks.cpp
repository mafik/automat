// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "tasks.hpp"

#include <atomic>
#include <shared_mutex>
#include <stop_token>
#include <tracy/Tracy.hpp>
#include <vector>

#include "argument.hpp"
#include "automat.hpp"
#include "base.hpp"
#include "blockingconcurrentqueue.hpp"
#include "casting.hpp"
#include "command.hpp"
#include "error.hpp"
#include "error_recovery.hpp"
#include "memory.hpp"
#include "source_location.hpp"
#include "thread_name.hpp"
#include "time.hpp"
#include "ui_connection_widget.hpp"

namespace automat {

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

Task::Task(WeakPtr<Object> target) : target(target) {}

Task::~Task() {
  if (parent) parent->CountDown();
}

void Task::SetParent(Task& new_parent) {
  parent = &new_parent;
  ++new_parent.countdown;
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
  CountDown();
}

void Task::CountDown() {
  if (--countdown == 0) {
    // TODO: moodycamel::concurrentqueue should run faster with explicit producer tokens
    queue.enqueue(this);
  }
}

void Task::Execute(std::unique_ptr<Task> self) {
  ZoneScopedN("Execute");
  scheduled = false;
  OnExecute(self);  // may steal self (lol)
}

std::string Task::Format() { return "Task()"; }

std::string RunTask::Format() { return f("RunTask({})", Name(target)); }

Locked<Command> FollowArgument(Argument arg) {
  // audio::Play(source.object->NextSound());

  if (auto* last_activity = arg.LastActivity()) {
    last_activity->fetch_add(1, std::memory_order_relaxed);
  }

  Locked<Command> target;
  if (auto next = arg.Find()) {
    // The target may be a Command sub-interface or the Object itself (its first Command).
    Command command;
    if (auto* s = dyn_cast_if_present<Command::Table>(next.table_ptr)) {
      command = Command(next.object_ptr, s);
    } else if (auto* obj = next.object_ptr) {
      command = obj->Find<Command>();
    }
    if (command) {
      command.object_ptr->IncrementOwningRefs();
      target = AdoptLocked(command);
    }
  }
  arg.WakeToys();
  return target;
}

void ScheduleArgumentTargets(Argument arg) {
  if (auto command = FollowArgument(arg)) command.ScheduleRun();
}

void RunTask::OnExecute(std::unique_ptr<Task>& self) {
  ZoneScopedN("RunTask");
  auto object = target.lock();
  if (!object) return;
  auto cmd = AdoptLocked(Command(object.Release(), cast<Command::Table>(command)));
  while (cmd) cmd = cmd.Run();
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
