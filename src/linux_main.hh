// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <xcb/xcb.h>

#include "vec.hh"

using namespace maf;

extern xcb_connection_t* connection;
extern xcb_window_t xcb_window;
extern xcb_screen_t* screen;
extern uint8_t xi_opcode;

struct SystemEventHook {
  virtual ~SystemEventHook() = default;
  virtual bool Intercept(xcb_generic_event_t*) = 0;  // only on Linux!
};

// TODO: remove this global variable & instead push the platform-specific code to the Keyboard class
extern Vec<SystemEventHook*> system_event_hooks;

int LinuxMain(int argc, char* argv[]);
