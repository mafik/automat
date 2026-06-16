#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// Automat's own Wayland compositor. Processes launched by the Command object
// connect to it as their display server; every mapped toplevel becomes a
// "Wayland Window" object on the board.

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>

#include "optional.hpp"
#include "status.hpp"
#include "str.hpp"

namespace automat::library {
struct WaylandWindow;
}

namespace automat::wayland {

struct Server {
  struct Impl;

  Server(std::stop_token stop);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool Running();
  Str SocketName();
  void UIFrame();
  void NotifyWindowDestroyed(void* toplevel_handle);
  void SendPointerEnter(library::WaylandWindow&, float sx, float sy);
  void SendPointerMotion(library::WaylandWindow&, float sx, float sy);
  void SendPointerButton(library::WaylandWindow&, uint32_t button, bool pressed);
  void SendPointerAxis(library::WaylandWindow&, float notches_up);
  void SendPointerLeave(library::WaylandWindow&);
  void SendKeyboardEnter(library::WaylandWindow&);
  void SendKeyboardLeave(library::WaylandWindow&);
  void SendKey(library::WaylandWindow&, uint32_t evdev_keycode, bool pressed, bool ctrl, bool alt,
               bool shift, bool super);

 private:
  std::unique_ptr<Impl> impl;
};

extern Optional<Server> server;

}  // namespace automat::wayland
