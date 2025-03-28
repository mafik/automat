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

#define ATOMS(MACRO)                     \
  MACRO(WM_STATE)                        \
  MACRO(WM_PROTOCOLS)                    \
  MACRO(WM_DELETE_WINDOW)                \
  MACRO(_NET_ACTIVE_WINDOW)              \
  MACRO(_NET_WM_USER_TIME)               \
  MACRO(_NET_WM_STATE)                   \
  MACRO(_NET_WM_STATE_MODAL)             \
  MACRO(_NET_WM_STATE_STICKY)            \
  MACRO(_NET_WM_STATE_MAXIMIZED_VERT)    \
  MACRO(_NET_WM_STATE_MAXIMIZED_HORZ)    \
  MACRO(_NET_WM_STATE_SHADED)            \
  MACRO(_NET_WM_STATE_SKIP_TASKBAR)      \
  MACRO(_NET_WM_STATE_SKIP_PAGER)        \
  MACRO(_NET_WM_STATE_HIDDEN)            \
  MACRO(_NET_WM_STATE_FULLSCREEN)        \
  MACRO(_NET_WM_STATE_ABOVE)             \
  MACRO(_NET_WM_STATE_BELOW)             \
  MACRO(_NET_WM_STATE_DEMANDS_ATTENTION) \
  MACRO(_GTK_FRAME_EXTENTS)

#define DECLARE_ATOM(name) extern xcb_atom_t name;
ATOMS(DECLARE_ATOM)
#undef DECLARE_ATOM

maf::Str ToStr(xcb_atom_t);

}  // namespace atom

void Connect(maf::Status& status);

struct FreeDeleter {
  void operator()(void* p) const { free(p); }
};

inline void flush() { xcb_flush(connection); }

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

inline std::unique_ptr<xcb_query_pointer_reply_t, FreeDeleter> query_pointer() {
  return std::unique_ptr<xcb_query_pointer_reply_t, FreeDeleter>(
      xcb_query_pointer_reply(connection, xcb_query_pointer(connection, screen->root), nullptr));
}

inline std::unique_ptr<xcb_query_tree_reply_t, FreeDeleter> query_tree(xcb_window_t window) {
  return std::unique_ptr<xcb_query_tree_reply_t, FreeDeleter>(
      xcb_query_tree_reply(connection, xcb_query_tree(connection, window), nullptr));
}

inline std::span<xcb_window_t> query_tree_children(const xcb_query_tree_reply_t& reply) {
  return std::span<xcb_window_t>(xcb_query_tree_children(&reply), reply.children_len);
}

// Note: the `delete` flag is always set to false by this wrapper.
inline std::unique_ptr<xcb_get_property_reply_t, FreeDeleter> get_property(xcb_window_t window,
                                                                           xcb_atom_t property,
                                                                           xcb_atom_t type,
                                                                           uint32_t long_offset,
                                                                           uint32_t long_length) {
  return std::unique_ptr<xcb_get_property_reply_t, FreeDeleter>(xcb_get_property_reply(
      connection,
      xcb_get_property(connection, false, window, property, type, long_offset, long_length),
      nullptr));
}

// A helper for reading properties that can be encoded as arbitrary types and could have arbitrary
// lengths.
std::string GetPropertyString(xcb_window_t window, xcb_atom_t property);

inline std::unique_ptr<xcb_get_geometry_reply_t, xcb::FreeDeleter> get_geometry(
    xcb_window_t window) {
  return std::unique_ptr<xcb_get_geometry_reply_t, xcb::FreeDeleter>(
      xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr));
}

inline std::unique_ptr<xcb_translate_coordinates_reply_t, xcb::FreeDeleter> translate_coordinates(
    xcb_window_t src_window, xcb_window_t dst_window, int16_t src_x, int16_t src_y) {
  return std::unique_ptr<xcb_translate_coordinates_reply_t, xcb::FreeDeleter>(
      xcb_translate_coordinates_reply(
          connection, xcb_translate_coordinates(connection, src_window, dst_window, src_x, src_y),
          nullptr));
}

inline std::unique_ptr<xcb_get_window_attributes_reply_t, xcb::FreeDeleter> get_window_attributes(
    xcb_window_t window) {
  return std::unique_ptr<xcb_get_window_attributes_reply_t, xcb::FreeDeleter>(
      xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, window),
                                      nullptr));
}

inline std::unique_ptr<xcb_get_input_focus_reply_t, xcb::FreeDeleter> get_input_focus() {
  return std::unique_ptr<xcb_get_input_focus_reply_t, xcb::FreeDeleter>(
      xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr));
}

void ReplaceProperty32(xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint32_t value);

namespace freedesktop {

// Activate the target `window` by sending a _NET_ACTIVE_WINDOW event to the root window.
//
// If `active_window` is provided, it will be used as the active window in the event. This may
// make the window manager more compliant.
void ActivateWindow(xcb_window_t window, xcb_window_t active_window = XCB_WINDOW_NONE);
}  // namespace freedesktop

}  // namespace xcb
