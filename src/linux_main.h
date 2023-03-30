#pragma once

#include <xcb/xcb.h>

extern xcb_connection_t *connection;
extern xcb_window_t window;

int LinuxMain(int argc, char* argv[]);
