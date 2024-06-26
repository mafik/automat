#include "time.hh"

#include <condition_variable>
#include <map>
#include <thread>

#include "base.hh"
#include "tasks.hh"
#include "thread_name.hh"

namespace automat {

using namespace time;

static std::jthread timer_thread;
static std::mutex mtx;
static std::condition_variable cv;

static std::multimap<SteadyPoint, std::unique_ptr<Task>> tasks;

static void TimerThread(std::stop_token automat_stop_token) {
  SetThreadName("Timer");
  bool stop = false;  // doesn't have to be atomic because it's protected by mtx
  std::stop_callback on_automat_stop(automat_stop_token, [&] {
    std::unique_lock<std::mutex> lck(mtx);
    stop = true;
    cv.notify_all();
  });

  while (true) {
    std::unique_lock<std::mutex> lck(mtx);
    if (tasks.empty()) {
      // LOG << "Timer thread waiting";
      cv.wait(lck);
    } else {
      // LOG << "Timer thread waiting until " << tasks.begin()->first.time_since_epoch().count()
      //     << " (" << tasks.size() << " tasks)";
      cv.wait_until(lck, tasks.begin()->first);
    }
    if (stop) {
      break;
    }
    auto now = SteadyClock::now();
    while (!tasks.empty() && tasks.begin()->first <= now) {
      // LOG << "Timer thread executing task " << tasks.begin()->second->Format();
      events.send(std::move(tasks.begin()->second));
      tasks.erase(tasks.begin());
    }
  }
}

void StartTimeThread(std::stop_token automat_stop_token) {
  timer_thread = std::jthread(TimerThread, automat_stop_token);
  timer_thread.detach();
}

static void TimerFinished(Location& here, SteadyPoint scheduled_time) {
  TimerNotificationReceiver* timer = here.As<TimerNotificationReceiver>();
  if (timer == nullptr) {
    ERROR << "Timer notification sent to an object which cannot receive it: " << here.Name();
    return;
  }
  timer->OnTimerNotification(here, scheduled_time);
}

struct TimerFinishedTask : Task {
  time::SteadyPoint scheduled_time;
  TimerFinishedTask(Location* target, time::SteadyPoint scheduled_time)
      : Task(target), scheduled_time(scheduled_time) {}
  std::string Format() override { return "TimerFinishedTask"; }
  void Execute() override {
    PreExecute();
    TimerFinished(*target, scheduled_time);
    PostExecute();
  }
};

void ScheduleAt(Location& here, SteadyPoint time) {
  std::unique_lock<std::mutex> lck(mtx);
  tasks.emplace(time, new TimerFinishedTask(&here, time));
  cv.notify_all();
}

void CancelScheduledAt(Location& here) {
  std::unique_lock<std::mutex> lck(mtx);
  for (auto it = tasks.begin(); it != tasks.end();) {
    if (it->second->target == &here) {
      it = tasks.erase(it);
    } else {
      ++it;
    }
  }
  cv.notify_all();
}

void CancelScheduledAt(Location& here, SteadyPoint time) {
  std::unique_lock<std::mutex> lck(mtx);
  auto [a, b] = tasks.equal_range(time);
  for (auto it = a; it != b; ++it) {
    if (it->second->target == &here) {
      tasks.erase(it);
      break;
    }
  }
  cv.notify_all();
}

void RescheduleAt(Location& here, SteadyPoint old_time, SteadyPoint new_time) {
  std::unique_lock<std::mutex> lck(mtx);
  auto [a, b] = tasks.equal_range(old_time);
  for (auto it = a; it != b; ++it) {
    if (it->second->target == &here) {
      tasks.erase(it);
      break;
    }
  }
  if (new_time <= SteadyClock::now()) {
    cv.notify_all();
    TimerFinished(here, new_time);
  } else {
    tasks.emplace(new_time, new TimerFinishedTask(&here, new_time));
    cv.notify_all();
  }
}

namespace time {

SystemPoint SystemFromSteady(SteadyPoint steady) { return SystemNow() + (steady - SteadyNow()); }
SteadyPoint SteadyFromSystem(SystemPoint system) { return SteadyNow() + (system - SystemNow()); }

}  // namespace time

}  // namespace automat