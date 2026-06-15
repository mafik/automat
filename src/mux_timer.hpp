#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <functional>

#include "mux_epoll.hpp"
#include "status.hpp"

namespace automat::mux {

struct Timer : Epoll::Listener {
  Epoll& epoll;
  Status status;
  std::function<void()> handler;

  Timer(Epoll& epoll);
  ~Timer();

  // Setting `initial_s` to zero disarms the timer.
  void Arm(double initial_s, double interval_s = 0);
  void Disarm();

  // Calls `handler` whenever the timer triggers. Part of the Epoll::Listener
  // interface.
  void NotifyRead(Status&) override;

  StrView Name() const override;
};

}  // namespace automat::mux
