#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// Automat's own Wayland compositor. Processes launched by the Command object
// connect to it as their display server; every mapped toplevel becomes a
// "Wayland Window" object on the board.
//
// Threading: a dedicated wayland thread owns the wl_display and all protocol
// state. Cross-thread work is posted onto it via Post(). Pixels flow out
// through WaylandWindow::pixels (object mutex + wake_counter); board
// mutations happen in UIFrame(), called once per frame on the UI thread.

#include <sys/types.h>

#include <cstdint>
#include <functional>

#include "status.hh"
#include "str.hh"

namespace automat::library {
struct WaylandWindow;
}

namespace automat::wayland {

// Starts the compositor thread. Somehow it's also responsible for process watching.
// Safe to call once at startup.
void Start();

// Disconnects clients and joins the compositor thread.
void Stop();

bool Running();

// The WAYLAND_DISPLAY value clients should use; empty when not running.
Str SocketName();

// UI-thread, once per frame: inserts newly mapped windows into the root
// board and removes windows whose client went away.
void UIFrame();

// Called from ~WaylandWindow: the user deleted the window object (black
// hole, bubble menu), so ask the client to close; SIGTERM follows if it
// ignores the request. Safe from any thread.
void NotifyWindowDestroyed(void* toplevel_handle);

// Watches a spawned child for exit. `on_exit` runs on the compositor
// thread with the raw waitpid() status. Safe to call from any thread.
void WatchProcess(pid_t pid, std::function<void(int wait_status)> on_exit, Status& status);

// UI-thread input injection. Coordinates are client-surface pixels. All of
// these are asynchronous and become no-ops when the window's client is gone.
void SendPointerEnter(library::WaylandWindow&, float sx, float sy);
void SendPointerMotion(library::WaylandWindow&, float sx, float sy);
void SendPointerButton(library::WaylandWindow&, uint32_t button, bool pressed);  // BTN_* codes
void SendPointerLeave(library::WaylandWindow&);
void SendKeyboardEnter(library::WaylandWindow&);
void SendKeyboardLeave(library::WaylandWindow&);
void SendKey(library::WaylandWindow&, uint32_t evdev_keycode, bool pressed, bool ctrl, bool alt,
             bool shift, bool super);

}  // namespace automat::wayland
