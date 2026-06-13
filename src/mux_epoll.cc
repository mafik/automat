// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "mux_epoll.hh"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

//  #define DEBUG_EPOLL

#ifdef DEBUG_EPOLL
#include "log.hh"
#endif

namespace automat::mux {

void Epoll::Init(Status& status) {
  fd = epoll_create1(EPOLL_CLOEXEC);
  if (fd < 0) {
    status() += "epoll_create1";
    return;
  }
  wakeup.epoll = this;
  wakeup.fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  epoll_event ev = {.events = EPOLLIN, .data = {.ptr = &wakeup}};
  if (epoll_ctl(fd, EPOLL_CTL_ADD, wakeup.fd, &ev) == -1) {
    status() += "epoll_ctl(EPOLL_CTL_ADD) wakeup";
  }
}

static epoll_event MakeEpollEvent(Epoll::Listener* listener) {
  epoll_event ev = {.events = 0, .data = {.ptr = listener}};
  if (listener->notify_read) {
    ev.events |= EPOLLIN;
  }
  if (listener->notify_write) {
    ev.events |= EPOLLOUT;
  }
  return ev;
}

void Epoll::Add(Listener* listener, Status& status) {
  if (fd == 0) {
    status() += "Epoll::Init() was not called";
    return;
  }
  epoll_event ev = MakeEpollEvent(listener);
  if (int r = epoll_ctl(fd, EPOLL_CTL_ADD, listener->fd, &ev); r == -1) {
    status() += "epoll_ctl(EPOLL_CTL_ADD) epfd=" + ToStr(fd) + " fd=" + ToStr(listener->fd);
    return;
  }
  ++listener_count;
#ifdef DEBUG_EPOLL
  LOG << "Added listener for " << listener->Name() << listener->fd << ". Currently "
      << listener_count << " active listeners.";
#endif
}

void Epoll::Mod(Listener* listener, Status& status) {
  epoll_event ev = MakeEpollEvent(listener);
#ifdef DEBUG_EPOLL
  LOG << "epoll_ctl " << listener->Name() << listener->fd << " "
      << (ev.events & EPOLLOUT ? "RDWR" : "RD");
#endif
  if (int r = epoll_ctl(fd, EPOLL_CTL_MOD, listener->fd, &ev); r == -1) {
    status() += "epoll_ctl(EPOLL_CTL_MOD)";
  }
}

void Epoll::Del(Listener* l, Status& status) {
  if (int r = epoll_ctl(fd, EPOLL_CTL_DEL, l->fd, nullptr); r == -1) {
    status() += "epoll_ctl(EPOLL_CTL_DEL)";
    return;
  }
  --listener_count;
  for (int i = 0; i < events_count; ++i) {
    if (events[i].data.ptr == l) {
      events[i].data.ptr = nullptr;
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

void Epoll::Loop(Status& status, std::stop_token stop) {
  std::stop_callback on_stop(stop, [this] { Wake(); });
  for (;;) {
    if (listener_count == 0 || stop.stop_requested()) {
      break;
    }
    events_count = epoll_wait(fd, events, kMaxEpollEvents, -1);
    if (events_count == -1) {
      if (errno == EINTR) {
        continue;
      }
      status() += "epoll_wait";
      return;
    }

    for (int i = 0; i < events_count; ++i) {
      if (events[i].data.ptr == nullptr) continue;
      Listener* l = (Listener*)events[i].data.ptr;
#ifdef DEBUG_EPOLL
      if (strcmp(l->Name(), "Timer")) {
        bool in = events[i].events & EPOLLIN;
        bool out = events[i].events & EPOLLOUT;
        LOG << "epoll_wait[" << i << "/" << events_count << "] " << l->Name() << l->fd << " "
            << (in ? "RD" : "") << (out ? "WR" : "");
      }
#endif
      if (events[i].events & EPOLLIN) {
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
      if (events[i].data.ptr == nullptr) continue;
      if (events[i].events & EPOLLOUT) {
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

}  // namespace automat::mux
