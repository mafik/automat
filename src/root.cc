// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "root.hh"

#include <atomic>
#include <thread>

#include "prototypes.hh"
#include "window.hh"

namespace automat {

std::shared_ptr<Location> root_location;
std::shared_ptr<Machine> root_machine;
std::jthread automat_thread;
std::atomic_bool automat_thread_finished = false;

// TODO: merge this with InitAutomat
void InitRoot() {
  root_location = std::make_shared<Location>();
  root_location->name = "Root location";
  root_machine = root_location->Create<Machine>();
  root_machine->parent = gui::window;
  root_machine->name = "Root machine";
  automat_thread = std::jthread(RunThread);
  auto& prototypes = Prototypes();
  sort(prototypes.begin(), prototypes.end(),
       [](const auto& a, const auto& b) { return a->Name() < b->Name(); });
}

void StopRoot() {
  if (automat_thread.joinable()) {
    automat_thread.request_stop();
    automat_thread.join();
  }
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
  events.send(std::make_unique<FunctionTask>(root_location, [f](Location& l) { f(); }));
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