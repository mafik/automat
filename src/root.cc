#include "root.hh"

#include <condition_variable>
#include <thread>

namespace automat {

Location root_location;
Machine* root_machine;
std::jthread automat_thread;

void InitRoot() {
  root_location.name = "Root location";
  root_machine = root_location.Create<Machine>();
  root_machine->name = "Root machine";
  automat_thread = std::jthread(RunThread);
}

void StopRoot() {
  if (automat_thread.joinable()) {
    automat_thread.request_stop();
    automat_thread.join();
  }
}

void RunOnAutomatThread(std::function<void()> f) {
  if (std::this_thread::get_id() == automat_thread.get_id()) {
    f();
    return;
  }
  events.send(std::make_unique<FunctionTask>(&root_location, [f](Location& l) { f(); }));
}

void RunOnAutomatThreadSynchronous(std::function<void()> f) {
  if (std::this_thread::get_id() == automat_thread.get_id()) {
    f();
    return;
  }
  std::mutex mutex;
  std::condition_variable automat_thread_done;
  std::unique_lock<std::mutex> lock(mutex);
  RunOnAutomatThread([&]() {
    f();
    // wake the UI thread
    std::unique_lock<std::mutex> lock(mutex);
    automat_thread_done.notify_all();
  });
  automat_thread_done.wait(lock);
}

}  // namespace automat