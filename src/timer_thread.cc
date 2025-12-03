// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "timer_thread.hh"

#include <condition_variable>
#include <map>
#include <thread>
#include <tracy/Tracy.hpp>

#include "base.hh"
#include "status.hh"
#include "tasks.hh"
#include "thread_name.hh"
#include "time.hh"

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
      // LOG << "Timer thread waiting until " <<
      // time::ToSeconds(tasks.begin()->first.time_since_epoch())
      //     << " (" << tasks.size() << " tasks)";
      auto wake_time = tasks.begin()->first;
      cv.wait_until(lck, wake_time);
    }
    if (stop) {
      break;
    }
    vector<std::unique_ptr<Task>> ready_tasks;
    auto now = SteadyClock::now();
    while (!tasks.empty() && tasks.begin()->first <= now) {
      ready_tasks.emplace_back(std::move(tasks.begin()->second));
      tasks.erase(tasks.begin());
    }

    lck.unlock();

    for (auto& task : ready_tasks) {
      task.release()->Schedule();
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
  TimerFinishedTask(WeakPtr<Location> target, time::SteadyPoint scheduled_time)
      : Task(target), scheduled_time(scheduled_time) {}
  std::string Format() override { return "TimerFinishedTask"; }
  void OnExecute(std::unique_ptr<Task>& self) override {
    ZoneScopedN("TimerFinishedTask");
    auto loc = target.lock();
    if (loc == nullptr) return;
    TimerFinished(*loc, scheduled_time);
  }
};

void ScheduleAt(Location& here, SteadyPoint time) {
  std::unique_lock<std::mutex> lck(mtx);
  tasks.emplace(time, new TimerFinishedTask(here.AcquirePtr<Location>(), time));
  cv.notify_all();
}

void CancelScheduledAt(Location& here) {
  std::unique_lock<std::mutex> lck(mtx);
  for (auto it = tasks.begin(); it != tasks.end();) {
    if (it->second->target.lock().get() == &here) {
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
    if (it->second->target.lock().get() == &here) {
      tasks.erase(it);
      break;
    }
  }
  cv.notify_all();
}

StatusCode RescheduleAt(Location& here, SteadyPoint old_time, SteadyPoint new_time) {
  std::unique_lock<std::mutex> lck(mtx);
  auto [a, b] = tasks.equal_range(old_time);
  for (auto it = a; it != b; ++it) {
    if (it->second->target.lock().get() == &here) {
      static_cast<TimerFinishedTask*>(it->second.get())->scheduled_time = new_time;
      auto tmp = std::move(it->second);
      tasks.erase(it);
      tasks.emplace(new_time, std::move(tmp));
      cv.notify_all();
      return STATUS_OK;
    }
  }
  return STATUS_FAILED;
}

}  // namespace automat
