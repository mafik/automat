#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>

#include <atomic>
#include <span>

#include "status.hpp"
#include "unique_ptr.hpp"

namespace xcb {

extern xcb_connection_t* connection;
extern xcb_screen_t* screen;
extern uint8_t xi_opcode;

// Last X server timestamp observed on any input event. Updated from the XCB
// event loop, read by ActivateWindow so the WM honours focus-stealing rules.
extern std::atomic<xcb_timestamp_t> last_event_time;

// The window currently holding the WM's _NET_ACTIVE_WINDOW. Tracked by the
// XCB event loop (initial query + PropertyNotify on root). Used by
// ActivateWindow as the "requestor's currently active window" field, which
// some WMs check as part of focus-stealing prevention.
extern std::atomic<xcb_window_t> active_window;

namespace atom {

#define ATOM_FROM_IDENTIFIER(name) ATOM(name, #name)
#define ATOMS                                            \
  ATOM_FROM_IDENTIFIER(WM_STATE)                         \
  ATOM_FROM_IDENTIFIER(WM_PROTOCOLS)                     \
  ATOM_FROM_IDENTIFIER(WM_DELETE_WINDOW)                 \
  ATOM_FROM_IDENTIFIER(WM_NAME)                          \
  ATOM_FROM_IDENTIFIER(_NET_ACTIVE_WINDOW)               \
  ATOM_FROM_IDENTIFIER(_NET_WM_USER_TIME)                \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE)                    \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_MODAL)              \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_STICKY)             \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_MAXIMIZED_VERT)     \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_MAXIMIZED_HORZ)     \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_SHADED)             \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_SKIP_TASKBAR)       \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_SKIP_PAGER)         \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_HIDDEN)             \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_FULLSCREEN)         \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_ABOVE)              \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_BELOW)              \
  ATOM_FROM_IDENTIFIER(_NET_WM_STATE_DEMANDS_ATTENTION)  \
  ATOM_FROM_IDENTIFIER(_GTK_FRAME_EXTENTS)               \
  ATOM_FROM_IDENTIFIER(INCR)                             \
  ATOM_FROM_IDENTIFIER(XdndAware)                        \
  ATOM_FROM_IDENTIFIER(XdndEnter)                        \
  ATOM_FROM_IDENTIFIER(XdndPosition)                     \
  ATOM_FROM_IDENTIFIER(XdndStatus)                       \
  ATOM_FROM_IDENTIFIER(XdndLeave)                        \
  ATOM_FROM_IDENTIFIER(XdndDrop)                         \
  ATOM_FROM_IDENTIFIER(XdndFinished)                     \
  ATOM_FROM_IDENTIFIER(XdndSelection)                    \
  ATOM_FROM_IDENTIFIER(XdndTypeList)                     \
  ATOM_FROM_IDENTIFIER(XdndActionCopy)                   \
  ATOM_FROM_IDENTIFIER(XdndActionList)                   \
  ATOM_FROM_IDENTIFIER(XdndActionDescription)            \
  ATOM_FROM_IDENTIFIER(XdndDirectSave0)                  \
  ATOM(image_png, "image/png")                           \
  ATOM(image_webp, "image/webp")                         \
  ATOM(image_jpeg, "image/jpeg")                         \
  ATOM(image_jpg, "image/jpg")                           \
  ATOM(image_gif, "image/gif")                           \
  ATOM(image_bmp, "image/bmp")                           \
  ATOM(application_octet_stream, "application/octet-stream") \
  ATOM(text_uri_list, "text/uri-list")

#define ATOM(name, str) extern xcb_atom_t name;
ATOMS
#undef ATOM

automat::Str ToStr(xcb_atom_t);

}  // namespace atom

void Connect(automat::Status& status);

inline void flush() { xcb_flush(connection); }

inline std::unique_ptr<xcb_input_xi_query_device_reply_t, automat::DeleteWithFree> input_xi_query_device(
    xcb_input_device_id_t deviceid) {
  return std::unique_ptr<xcb_input_xi_query_device_reply_t, automat::DeleteWithFree>(
      xcb_input_xi_query_device_reply(connection, xcb_input_xi_query_device(connection, deviceid),
                                      nullptr));
}

inline std::unique_ptr<xcb_input_xi_query_version_reply_t, automat::DeleteWithFree> input_xi_query_version(
    uint16_t major_version, uint16_t minor_version) {
  return std::unique_ptr<xcb_input_xi_query_version_reply_t, automat::DeleteWithFree>(
      xcb_input_xi_query_version_reply(
          connection, xcb_input_xi_query_version(connection, major_version, minor_version),
          nullptr));
}

inline std::unique_ptr<xcb_query_pointer_reply_t, automat::DeleteWithFree> query_pointer() {
  return std::unique_ptr<xcb_query_pointer_reply_t, automat::DeleteWithFree>(
      xcb_query_pointer_reply(connection, xcb_query_pointer(connection, screen->root), nullptr));
}

inline std::unique_ptr<xcb_query_tree_reply_t, automat::DeleteWithFree> query_tree(xcb_window_t window) {
  return std::unique_ptr<xcb_query_tree_reply_t, automat::DeleteWithFree>(
      xcb_query_tree_reply(connection, xcb_query_tree(connection, window), nullptr));
}

inline std::span<xcb_window_t> query_tree_children(const xcb_query_tree_reply_t& reply) {
  return std::span<xcb_window_t>(xcb_query_tree_children(&reply), reply.children_len);
}

// Note: the `delete` flag is always set to false by this wrapper.
inline std::unique_ptr<xcb_get_property_reply_t, automat::DeleteWithFree> get_property(xcb_window_t window,
                                                                           xcb_atom_t property,
                                                                           xcb_atom_t type,
                                                                           uint32_t long_offset,
                                                                           uint32_t long_length) {
  return std::unique_ptr<xcb_get_property_reply_t, automat::DeleteWithFree>(xcb_get_property_reply(
      connection,
      xcb_get_property(connection, false, window, property, type, long_offset, long_length),
      nullptr));
}

// A helper for reading properties that can be encoded as arbitrary types and could have arbitrary
// lengths.
std::string GetPropertyString(xcb_window_t window, xcb_atom_t property);

inline std::unique_ptr<xcb_get_geometry_reply_t, automat::DeleteWithFree> get_geometry(
    xcb_window_t window) {
  return std::unique_ptr<xcb_get_geometry_reply_t, automat::DeleteWithFree>(
      xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr));
}

inline std::unique_ptr<xcb_translate_coordinates_reply_t, automat::DeleteWithFree> translate_coordinates(
    xcb_window_t src_window, xcb_window_t dst_window, int16_t src_x, int16_t src_y) {
  return std::unique_ptr<xcb_translate_coordinates_reply_t, automat::DeleteWithFree>(
      xcb_translate_coordinates_reply(
          connection, xcb_translate_coordinates(connection, src_window, dst_window, src_x, src_y),
          nullptr));
}

inline std::unique_ptr<xcb_get_window_attributes_reply_t, automat::DeleteWithFree> get_window_attributes(
    xcb_window_t window) {
  return std::unique_ptr<xcb_get_window_attributes_reply_t, automat::DeleteWithFree>(
      xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, window),
                                      nullptr));
}

inline std::unique_ptr<xcb_get_input_focus_reply_t, automat::DeleteWithFree> get_input_focus() {
  return std::unique_ptr<xcb_get_input_focus_reply_t, automat::DeleteWithFree>(
      xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr));
}

void ReplaceProperty32(xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint32_t value);

namespace freedesktop {

// Activate the target `window` by sending a _NET_ACTIVE_WINDOW event to the root window.
//
// Pulls the X server timestamp and Automat's currently-active window from the
// cached globals (`xcb::last_event_time`, `xcb::our_window`) so the WM can
// validate the request against its focus-stealing-prevention rules.
//
// Non-blocking and safe to call from any thread.
void ActivateWindow(xcb_window_t window);
}  // namespace freedesktop

}  // namespace xcb
