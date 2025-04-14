// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "xcb.hh"

#include <xkbcommon/xkbcommon-x11.h>

#include <cstring>
#include <map>

using namespace maf;

namespace xcb {

xcb_connection_t* connection = nullptr;
xcb_screen_t* screen = nullptr;
uint8_t xi_opcode = 0;

#define WRAP(f, ...)                             \
  std::unique_ptr<f##_reply_t, void (*)(void*)>( \
      f##_reply(connection, f(connection, __VA_ARGS__), nullptr), free)

namespace atom {

#define DEFINE_ATOM(name) xcb_atom_t name;
ATOMS(DEFINE_ATOM)
#undef DEFINE_ATOM

std::map<xcb_atom_t, Str> atom_names;

void Initialize() {
#define REQUEST_ATOM(name) \
  auto name##_request = xcb_intern_atom(connection, 0, strlen(#name), #name);
  ATOMS(REQUEST_ATOM)
#undef REQUEST_ATOM

#define ATOM_REPLY(name)                                                         \
  auto name##_reply = std::unique_ptr<xcb_intern_atom_reply_t, void (*)(void*)>( \
      xcb_intern_atom_reply(connection, name##_request, nullptr), free);         \
  name = name##_reply->atom;                                                     \
  atom_names[name] = #name;
  ATOMS(ATOM_REPLY)
#undef ATOM_REPLY
}

Str ToStr(xcb_atom_t atom) {
  if (auto it = atom_names.find(atom); it != atom_names.end()) {
    return it->second;
  }
  xcb_get_atom_name_reply_t* reply =
      xcb_get_atom_name_reply(connection, xcb_get_atom_name(connection, atom), NULL);
  char* name = xcb_get_atom_name_name(reply);
  Str name_str(name, xcb_get_atom_name_name_length(reply));
  free(reply);
  return atom_names[atom] = name_str;
}

}  // namespace atom

void Connect(Status& status) {
  if (connection) {
    return;
  }
  int screenp = 0;
  connection = xcb_connect(nullptr, &screenp);
  if (xcb_connection_has_error(connection)) {
    AppendErrorMessage(status) += "Failed to connect to X server.";
    return;
  }

  atom::Initialize();

  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(connection));
  for (int i = 0; i < screenp; ++i) {
    xcb_screen_next(&iter);
  }
  screen = iter.data;

  const xcb_query_extension_reply_t* xinput_data =
      xcb_get_extension_data(connection, &xcb_input_id);
  if (!xinput_data->present) {
    AppendErrorMessage(status) += "XInput extension not present.";
    return;
  }
  xi_opcode = xinput_data->major_opcode;

  xkb_x11_setup_xkb_extension(xcb::connection, XKB_X11_MIN_MAJOR_XKB_VERSION,
                              XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                              nullptr, nullptr, nullptr, nullptr);
}

std::string GetPropertyString(xcb_window_t window, xcb_atom_t property) {
  auto reply = get_property(window, property, XCB_ATOM_STRING, 0, 100 / 4);
  if (!reply) {
    return "";
  }
  if (reply->bytes_after == 0) {
    return std::string((char*)xcb_get_property_value(reply.get()),
                       (size_t)xcb_get_property_value_length(reply.get()));
  }
  auto proper_type = reply->type;
  auto proper_size = reply->bytes_after + reply->value_len;
  reply = get_property(window, property, proper_type, 0, (proper_size + 3) / 4);
  if (!reply) {
    return "";
  }
  return std::string((char*)xcb_get_property_value(reply.get()),
                     (size_t)xcb_get_property_value_length(reply.get()));
}

void ReplaceProperty32(xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint32_t value) {
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, property, type, 32, 1, &value);
}

namespace freedesktop {

void ActivateWindow(xcb_window_t window, xcb_window_t active_window) {
  xcb_client_message_event_t event = {
      .response_type = XCB_CLIENT_MESSAGE,
      .format = 32,
      .sequence = 0,
      .window = window,
      .type = xcb::atom::_NET_ACTIVE_WINDOW,
      .data = {.data32 =
                   {
                       1,  // source indication - 1 means application
                       0,  // TODO: time
                       active_window,
                   }},
  };

  xcb_send_event(xcb::connection, 1, xcb::screen->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 (const char*)&event);
}

}  // namespace freedesktop
}  // namespace xcb