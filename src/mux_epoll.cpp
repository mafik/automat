// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "mux_epoll.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>

#include "colony.hpp"
#include "format.hpp"
#include "libc.hpp"
#include "linux_syscall.hpp"
#include "log.hpp"
#include "thread_name.hpp"

//  #define DEBUG_EPOLL

namespace automat::mux {

void Epoll::Init(Status& status) {
  fd = LinuxSyscall(__NR_epoll_create1, kCloexec);
  if (fd < 0) {
    status() += f("epoll_create1: {}", strerror(-fd));
    return;
  }
  wakeup.epoll = this;
  wakeup.fd = LinuxSyscall(__NR_eventfd2, 0, kCloexec | kNonblock);
  EpollEvent ev = {.events = kEpollIn, .data = (uint64_t)&wakeup};
  if (int r = LinuxSyscall(__NR_epoll_ctl, fd, kEpollCtlAdd, (int)wakeup.fd, &ev); r < 0) {
    status() += f("epoll_ctl(EPOLL_CTL_ADD) wakeup: {}", strerror(-r));
  }
}

static EpollEvent MakeEpollEvent(Epoll::Listener* listener) {
  EpollEvent ev = {.events = 0, .data = (uint64_t)listener};
  if (listener->notify_read) {
    ev.events |= kEpollIn;
  }
  if (listener->notify_write) {
    ev.events |= kEpollOut;
  }
  return ev;
}

void Epoll::Add(Listener* listener, Status& status) {
  if (fd == 0) {
    status() += "Epoll::Init() was not called";
    return;
  }
  EpollEvent ev = MakeEpollEvent(listener);
  if (int r = LinuxSyscall(__NR_epoll_ctl, fd, kEpollCtlAdd, (int)listener->fd, &ev); r < 0) {
    status() +=
        f("epoll_ctl(EPOLL_CTL_ADD) epfd={} fd={}: {}", fd, (int)listener->fd, strerror(-r));
    return;
  }
  ++listener_count;
#ifdef DEBUG_EPOLL
  LOG << "Added listener for " << listener->Name() << listener->fd << ". Currently "
      << listener_count << " active listeners.";
#endif
}

void Epoll::Mod(Listener* listener, Status& status) {
  EpollEvent ev = MakeEpollEvent(listener);
#ifdef DEBUG_EPOLL
  LOG << "epoll_ctl " << listener->Name() << listener->fd << " "
      << (ev.events & kEpollOut ? "RDWR" : "RD");
#endif
  if (int r = LinuxSyscall(__NR_epoll_ctl, fd, kEpollCtlMod, (int)listener->fd, &ev); r < 0) {
    status() += f("epoll_ctl(EPOLL_CTL_MOD): {}", strerror(-r));
  }
}

void Epoll::Del(Listener* l, Status& status) {
  if (int r = LinuxSyscall(__NR_epoll_ctl, fd, kEpollCtlDel, (int)l->fd, nullptr); r < 0) {
    status() += f("epoll_ctl(EPOLL_CTL_DEL): {}", strerror(-r));
    return;
  }
  --listener_count;
  for (int i = 0; i < events_count; ++i) {
    if (events[i].data == (uint64_t)l) {
      events[i].data = 0;
    }
  }
#ifdef DEBUG_EPOLL
  LOG << "Removed listener for " << l->Name() << l->fd << ". Currently " << listener_count
      << " active listeners.";
#endif
}

void Epoll::Wake() {
  uint64_t one = 1;
  (void)!write(wakeup.fd, &one, sizeof(one));
}

void Epoll::Post(std::move_only_function<void()> fn) {
  {
    auto lock = std::lock_guard(post_mutex);
    posted.push_back(std::move(fn));
  }
  Wake();
}

void Epoll::Wakeup::NotifyRead(Status&) {
  uint64_t value;
  (void)!read(fd, &value, sizeof(value));
  std::vector<std::move_only_function<void()>> drain;
  {
    auto lock = std::lock_guard(epoll->post_mutex);
    drain.swap(epoll->posted);
  }
  for (auto& fn : drain) fn();
}

void Epoll::Loop(Status& status, bool stop_when_empty, std::stop_token stop) {
  std::stop_callback on_stop(stop, [this] { Wake(); });
  for (;;) {
    if ((stop_when_empty && listener_count == 0) || stop.stop_requested()) {
      break;
    }
    events_count = LinuxSyscall(__NR_epoll_pwait, fd, events, kMaxEpollEvents, -1, nullptr, 0);
    if (events_count < 0) {
      if (events_count == -EINTR) {
        continue;
      }
      status() += f("epoll_pwait: {}", strerror(-events_count));
      return;
    }

    for (int i = 0; i < events_count; ++i) {
      if (events[i].data == 0) continue;
      Listener* l = (Listener*)events[i].data;
#ifdef DEBUG_EPOLL
      if (strcmp(l->Name(), "Timer")) {
        bool in = events[i].events & kEpollIn;
        bool out = events[i].events & kEpollOut;
        LOG << "epoll_wait[" << i << "/" << events_count << "] " << l->Name() << l->fd << " "
            << (in ? "RD" : "") << (out ? "WR" : "");
      }
#endif
      if (events[i].events & kEpollIn) {
        l->NotifyRead(status);
        if (!status.Ok()) {
#ifdef DEBUG_EPOLL
          ERROR << l->Name() << ": " << ErrorMessage(status);
#endif
          return;
        }
#ifdef DEBUG_EPOLL
        if (errno) {
          ERROR << l->Name() << " didn't clean errno after NotifyRead: " << strerror(errno);
          errno = 0;
        }
#endif
      }
      if (events[i].data == 0) continue;
      if (events[i].events & kEpollOut) {
        l->NotifyWrite(status);
        if (!status.Ok()) {
#ifdef DEBUG_EPOLL
          ERROR << l->Name() << ": " << ErrorMessage(status);
#endif
          return;
        }
#ifdef DEBUG_EPOLL
        if (errno) {
          ERROR << l->Name() << " didn't clean errno after NotifyWrite: " << strerror(errno);
          errno = 0;
        }
#endif
      }
    }
    events_count = 0;
  }
}

Epoll epoll;

namespace {

libc::ChimeraThread loop_thread;
std::stop_source stop_source;

// One watched child; created and destroyed only on the shared epoll thread.
struct ProcessWatch : Epoll::Listener {
  pid_t pid = 0;
  std::function<void(int)> on_exit;
  StrView Name() const override { return "ProcessWatch"sv; }
  void NotifyRead(Status&) override;
};

Colony<ProcessWatch> process_watches;

void ProcessWatch::NotifyRead(Status&) {
  // pidfd readability means the child is reapable; waitpid yields the raw status
  // the watcher's callback decodes.
  int wstatus = 0;
  while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
  }
  auto cb = std::move(on_exit);
  Status local;
  epoll.Del(this, local);
  process_watches.erase(process_watches.get_iterator(this));  // frees this; closes the pidfd
  if (cb) cb(wstatus);
}

}  // namespace

void Init(Status& status) {
  epoll.Init(status);
  if (!status.Ok()) return;
  loop_thread = libc::StartChimeraThread([] {
    SetThreadName("Epoll");
    Status status;
    epoll.Loop(status, false, stop_source.get_token());
    if (!status.Ok()) ERROR << "mux::epoll loop stopped: " << status.ToStr();
  });
}

void Stop() {
  stop_source.request_stop();
  if (loop_thread.joinable()) loop_thread.join();
}

void WatchProcess(pid_t pid, std::function<void(int)> on_exit, Status& status) {
  // pidfd_open lacks a guaranteed glibc wrapper across versions, so go through
  // the syscall. The pid is still our unreaped child, so it is valid here.
  FD pidfd = LinuxSyscall(__NR_pidfd_open, pid, 0);
  if (pidfd < 0) {
    status() += f("pidfd_open(pid={}): {}", (int)pid, strerror(-pidfd));
    return;
  }
  // Registration touches the listener set, so it must run on the epoll thread.
  epoll.Post([pidfd = std::move(pidfd), pid, cb = std::move(on_exit)]() mutable {
    ProcessWatch* w = &*process_watches.emplace();
    w->fd = std::move(pidfd);
    w->pid = pid;
    w->on_exit = std::move(cb);
    Status st;
    epoll.Add(w, st);
    if (!st.Ok()) {
      ERROR << "mux: couldn't watch pid " << (int)pid << ": " << st.ToStr();
      process_watches.erase(process_watches.get_iterator(w));  // closes the pidfd
    }
  });
}

}  // namespace automat::mux
