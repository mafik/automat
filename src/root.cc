#include "root.h"

#include <condition_variable>
#include <thread>

namespace automaton {

Location root_location;
Machine *root_machine;
std::thread automaton_thread;

void InitRoot() {
  root_location = Location(nullptr);
  root_location.name = "Root location";
  root_machine = root_location.Create<Machine>();
  root_machine->name = "Root machine";
  automaton_thread = std::thread(RunThread);
}

void RunOnAutomatonThread(std::function<void()> f) {
  if (std::this_thread::get_id() == automaton_thread.get_id()) {
    f();
    return;
  }
  events.send(std::make_unique<FunctionTask>(&root_location,
                                             [f](Location &l) { f(); }));
}

void RunOnAutomatonThreadSynchronous(std::function<void()> f) {
  if (std::this_thread::get_id() == automaton_thread.get_id()) {
    f();
    return;
  }
  std::mutex mutex;
  std::condition_variable automaton_thread_done;
  std::unique_lock<std::mutex> lock(mutex);
  RunOnAutomatonThread([&]() {
    f();
    // wake the UI thread
    std::unique_lock<std::mutex> lock(mutex);
    automaton_thread_done.notify_all();
  });
  automaton_thread_done.wait(lock);
}

} // namespace automaton