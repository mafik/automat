// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "root.hh"

#include <atomic>
#include <condition_variable>
#include <thread>

#include "concurrentqueue.hh"
#include "global_resources.hh"
#include "prototypes.hh"
#include "thread_name.hh"
#include "timer_thread.hh"
#include "window.hh"

using namespace maf;

namespace automat {

std::stop_source stop_source;
std::shared_ptr<Location> root_location;
std::shared_ptr<Machine> root_machine;
std::jthread automat_thread;
std::atomic_bool automat_thread_finished = false;

moodycamel::ConcurrentQueue<Task*> queue;
std::mutex automat_threads_mutex;
std::condition_variable automat_threads_cv;

// TODO: Merge this RunThread
void RunLoop(const int max_iterations) {
  int iterations = 0;
  while (max_iterations < 0 || iterations < max_iterations) {
    Task* task;
    if (!queue.try_dequeue(task)) {
      break;
    }
    task->Execute();
    ++iterations;
  }
}

void EnqueueTask(Task* task) {
  queue.enqueue(task);
  std::unique_lock lk(automat_threads_mutex);
  automat_threads_cv.notify_one();
}

static void RunThread(std::stop_token stop_token) {
  SetThreadName("Automat Loop");
  while (!stop_token.stop_requested()) {
    Task* task;
    if (!queue.try_dequeue(task)) {
      std::unique_lock lk(automat_threads_mutex);
      if (!queue.try_dequeue(task)) {
        automat_threads_cv.wait(lk);
        continue;
      }
    }
    task->Execute();
  }
  automat_thread_finished = true;
  automat_thread_finished.notify_all();
}

// TODO: merge this with InitAutomat
void InitRoot() {
  root_location = std::make_shared<Location>();
  root_location->name = "Root location";
  root_location->parent = gui::window;
  root_machine = root_location->Create<Machine>();
  root_machine->parent = gui::window;
  root_machine->name = "Root machine";
  StartTimeThread(stop_source.get_token());
  automat_thread = std::jthread(RunThread, stop_source.get_token());
  auto& prototypes = Prototypes();
  sort(prototypes.begin(), prototypes.end(),
       [](const auto& a, const auto& b) { return a->Name() < b->Name(); });
}

void StopRoot() {
  if (automat_thread.joinable()) {
    {
      std::unique_lock lk(automat_threads_mutex);
      stop_source.request_stop();
    }
    automat_threads_cv.notify_all();
    automat_thread.join();
  }
  resources::Release();
}

void AssertAutomatThread() {
  if (automat_thread.get_stop_source().stop_requested()) {
    assert(automat_thread_finished);
  } else {
    assert(std::this_thread::get_id() == automat_thread.get_id());
  }
}

void RunOnAutomatThread(std::function<void()> f) {
  if (std::this_thread::get_id() == automat_thread.get_id()) {
    f();
    return;
  }
  if (automat_thread.get_stop_source().stop_requested()) {
    if (!automat_thread_finished) {
      automat_thread_finished.wait(false);
    }
    f();
    return;
  }
  auto task = new FunctionTask(root_location, [f](Location& l) { f(); });
  task->Schedule();
}

void RunOnAutomatThreadSynchronous(std::function<void()> f) {
  if (std::this_thread::get_id() == automat_thread.get_id()) {
    f();
    return;
  }
  std::atomic_bool done = false;
  RunOnAutomatThread([&]() {
    f();
    // wake the UI thread
    done = true;
    done.notify_all();
  });
  done.wait(false);
}

}  // namespace automat