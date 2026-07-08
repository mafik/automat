// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
// Warning: coded with a stochastic parrot
#include "mux_win32.hpp"

#pragma push_macro("ERROR")
#include <winsock2.h>
#include <windows.h>
#pragma pop_macro("ERROR")

#pragma comment(lib, "ws2_32")

#include <climits>
#include <thread>

#include "format.hpp"
#include "log.hpp"
#include "thread_name.hpp"

namespace automat::mux {

static SOCKET ToSocket(int fd) { return (SOCKET)(intptr_t)fd; }

static std::chrono::steady_clock::duration ToDuration(double seconds) {
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(seconds));
}

void Epoll::Init(Status& status) {
  WSADATA wsa;
  if (int err = WSAStartup(MAKEWORD(2, 2), &wsa)) {
    status() += f("WSAStartup error {}", err);
    return;
  }
  SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET) {
    status() += f("socket() for wakeup, error {}", WSAGetLastError());
    return;
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int addr_len = sizeof(addr);
  if (bind(s, (sockaddr*)&addr, sizeof(addr)) || getsockname(s, (sockaddr*)&addr, &addr_len) ||
      connect(s, (sockaddr*)&addr, sizeof(addr))) {
    status() += f("wakeup socket setup, error {}", WSAGetLastError());
    closesocket(s);
    return;
  }
  u_long nonblocking = 1;
  ioctlsocket(s, FIONBIO, &nonblocking);
  wakeup = FD((int)s);
  initialized = true;
}

void Epoll::Add(Listener* listener, Status& status) {
  if (!initialized) {
    status() += "Epoll::Init() was not called";
    return;
  }
  {
    auto lock = std::lock_guard(mutex);
    listeners.push_back(listener);
  }
  Wake();
}

void Epoll::Mod(Listener*, Status&) {
  // Listening flags are re-read from the Listener when the poll set is rebuilt.
  Wake();
}

void Epoll::Del(Listener* l, Status& status) {
  {
    auto lock = std::lock_guard(mutex);
    if (std::erase(listeners, l) == 0) {
      status() += "Epoll::Del() of an unregistered listener";
      return;
    }
    for (auto& b : batch) {
      if (b == l) b = nullptr;
    }
  }
  Wake();
}

void Epoll::Wake() {
  char one = 1;
  (void)send(ToSocket(wakeup), &one, 1, 0);
}

void Epoll::Post(std::move_only_function<void()> fn) {
  {
    auto lock = std::lock_guard(post_mutex);
    posted.push_back(std::move(fn));
  }
  Wake();
}

void Epoll::Loop(Status& status, bool stop_when_empty, std::stop_token stop) {
  std::stop_callback on_stop(stop, [this] { Wake(); });
  std::vector<WSAPOLLFD> pollfds;
  for (;;) {
    {
      auto lock = std::lock_guard(mutex);
      auto timer_lock = std::lock_guard(timer_mutex);
      // A constructed Timer counts as a listener, like its timerfd does on Linux.
      if ((stop_when_empty && listeners.empty() && timers.empty()) || stop.stop_requested()) {
        break;
      }
      batch = listeners;
    }
    pollfds.clear();
    pollfds.push_back({.fd = ToSocket(wakeup), .events = POLLRDNORM});
    for (Listener* l : batch) {
      short events = 0;
      if (l->notify_read) events |= POLLRDNORM;
      if (l->notify_write) events |= POLLWRNORM;
      pollfds.push_back({.fd = ToSocket(l->fd), .events = events});
    }
    int timeout_ms = -1;
    {
      auto timer_lock = std::lock_guard(timer_mutex);
      auto now = std::chrono::steady_clock::now();
      for (Timer* t : timers) {
        if (!t->armed) continue;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t->deadline - now).count();
        int clamped = ms < 0 ? 0 : ms > INT_MAX ? INT_MAX : (int)ms;
        if (timeout_ms < 0 || clamped < timeout_ms) timeout_ms = clamped;
      }
    }
    int n = WSAPoll(pollfds.data(), (ULONG)pollfds.size(), timeout_ms);
    if (n == SOCKET_ERROR) {
      status() += f("WSAPoll error {}", WSAGetLastError());
      return;
    }
    {
      auto timer_lock = std::lock_guard(timer_mutex);
      auto now = std::chrono::steady_clock::now();
      for (Timer* t : timers) {
        if (!t->armed || t->deadline > now) continue;
        if (t->interval > 0) {
          t->deadline += ToDuration(t->interval);
        } else {
          t->armed = false;
        }
        due.push_back(t);
      }
    }
    for (size_t i = 0; i < due.size(); ++i) {
      Timer* t;
      {
        auto timer_lock = std::lock_guard(timer_mutex);
        t = due[i];
      }
      if (t == nullptr) continue;
      if (t->handler) t->handler();
    }
    {
      auto timer_lock = std::lock_guard(timer_mutex);
      due.clear();
    }
    if (n > 0) {
      if (pollfds[0].revents) {
        char buf[16];
        while (recv(ToSocket(wakeup), buf, sizeof(buf), 0) > 0) {
        }
        std::vector<std::move_only_function<void()>> drain;
        {
          auto lock = std::lock_guard(post_mutex);
          drain.swap(posted);
        }
        for (auto& fn : drain) fn();
      }
      for (size_t i = 1; i < pollfds.size(); ++i) {
        short revents = pollfds[i].revents;
        if (revents == 0) continue;
        Listener* l;
        {
          auto lock = std::lock_guard(mutex);
          l = batch[i - 1];
        }
        if (l == nullptr) continue;
        // Errors & hangups are folded into NotifyRead so the listener's read
        // path observes the failure and unregisters itself.
        if (revents & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL)) {
          l->NotifyRead(status);
          if (!status.Ok()) return;
        }
        {
          auto lock = std::lock_guard(mutex);
          l = batch[i - 1];
        }
        if (l == nullptr) continue;
        if (revents & POLLWRNORM) {
          l->NotifyWrite(status);
          if (!status.Ok()) return;
        }
      }
    }
    {
      auto lock = std::lock_guard(mutex);
      batch.clear();
    }
  }
}

Timer::Timer(Epoll& epoll) : epoll(epoll) {
  auto timer_lock = std::lock_guard(epoll.timer_mutex);
  epoll.timers.push_back(this);
}

Timer::~Timer() {
  auto timer_lock = std::lock_guard(epoll.timer_mutex);
  std::erase(epoll.timers, this);
  for (auto& t : epoll.due) {
    if (t == this) t = nullptr;
  }
}

void Timer::Arm(double initial_s, double interval_s) {
  {
    auto timer_lock = std::lock_guard(epoll.timer_mutex);
    if (initial_s <= 0) {
      armed = false;
      interval = 0;
    } else {
      armed = true;
      interval = interval_s;
      deadline = std::chrono::steady_clock::now() + ToDuration(initial_s);
    }
  }
  epoll.Wake();  // recompute the poll timeout
}

void Timer::Disarm() { Arm(0, 0); }

Epoll epoll;

namespace {

std::thread loop_thread;
std::stop_source stop_source;

struct ProcessWatch {
  HANDLE process = nullptr;
  HANDLE wait = nullptr;
  std::function<void(int)> on_exit;
};

void CALLBACK OnProcessSignaled(void* context, BOOLEAN) {
  auto* w = (ProcessWatch*)context;
  DWORD exit_code = 0;
  GetExitCodeProcess(w->process, &exit_code);
  // A blocking UnregisterWaitEx would deadlock on this thread pool thread, so
  // cleanup runs on the mux thread instead.
  epoll.Post([w, exit_code] {
    UnregisterWaitEx(w->wait, INVALID_HANDLE_VALUE);
    CloseHandle(w->process);
    auto cb = std::move(w->on_exit);
    delete w;
    if (cb) cb((int)exit_code);
  });
}

}  // namespace

void Init(Status& status) {
  epoll.Init(status);
  if (!status.Ok()) return;
  loop_thread = std::thread([] {
    SetThreadName("Mux");
    Status status;
    epoll.Loop(status, false, stop_source.get_token());
    if (!status.Ok()) ERROR << "mux loop stopped: " << status.ToStr();
  });
}

void Stop() {
  stop_source.request_stop();
  if (loop_thread.joinable()) loop_thread.join();
}

void WatchProcess(int pid, std::function<void(int)> on_exit, Status& status) {
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (process == nullptr) {
    status() += f("OpenProcess(pid={}) error {}", pid, GetLastError());
    return;
  }
  auto* w = new ProcessWatch{.process = process, .on_exit = std::move(on_exit)};
  if (!RegisterWaitForSingleObject(&w->wait, process, OnProcessSignaled, w, INFINITE,
                                   WT_EXECUTEONLYONCE)) {
    status() += f("RegisterWaitForSingleObject(pid={}) error {}", pid, GetLastError());
    CloseHandle(process);
    delete w;
  }
}

}  // namespace automat::mux
