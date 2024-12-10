// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>

#include "status.hh"

namespace xcb {

extern xcb_connection_t* connection;
extern xcb_screen_t* screen;
extern uint8_t xi_opcode;

namespace atom {

#define ATOMS(MACRO)                  \
  MACRO(WM_PROTOCOLS)                 \
  MACRO(WM_DELETE_WINDOW)             \
  MACRO(_NET_WM_STATE)                \
  MACRO(_NET_WM_STATE_MODAL)          \
  MACRO(_NET_WM_STATE_STICKY)         \
  MACRO(_NET_WM_STATE_MAXIMIZED_VERT) \
  MACRO(_NET_WM_STATE_MAXIMIZED_HORZ) \
  MACRO(_NET_WM_STATE_SHADED)         \
  MACRO(_NET_WM_STATE_SKIP_TASKBAR)   \
  MACRO(_NET_WM_STATE_SKIP_PAGER)     \
  MACRO(_NET_WM_STATE_HIDDEN)         \
  MACRO(_NET_WM_STATE_FULLSCREEN)     \
  MACRO(_NET_WM_STATE_ABOVE)          \
  MACRO(_NET_WM_STATE_BELOW)          \
  MACRO(_NET_WM_STATE_DEMANDS_ATTENTION)

#define DECLARE_ATOM(name) extern xcb_atom_t name;
ATOMS(DECLARE_ATOM)
#undef DECLARE_ATOM

maf::Str ToStr(xcb_atom_t);

}  // namespace atom

void Connect(maf::Status& status);

struct FreeDeleter {
  void operator()(void* p) const { free(p); }
};

inline std::unique_ptr<xcb_input_xi_query_device_reply_t, FreeDeleter> input_xi_query_device(
    xcb_input_device_id_t deviceid) {
  return std::unique_ptr<xcb_input_xi_query_device_reply_t, FreeDeleter>(
      xcb_input_xi_query_device_reply(connection, xcb_input_xi_query_device(connection, deviceid),
                                      nullptr));
}

inline std::unique_ptr<xcb_input_xi_query_version_reply_t, FreeDeleter> input_xi_query_version(
    uint16_t major_version, uint16_t minor_version) {
  return std::unique_ptr<xcb_input_xi_query_version_reply_t, FreeDeleter>(
      xcb_input_xi_query_version_reply(
          connection, xcb_input_xi_query_version(connection, major_version, minor_version),
          nullptr));
}

}  // namespace xcb