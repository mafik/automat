// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "linux_main.hh"

#include <include/core/SkBitmap.h>
#include <include/core/SkGraphics.h>
#include <include/core/SkSurface.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "audio.hh"
#include "automat.hh"
#include "format.hh"
#include "keyboard.hh"
#include "library.hh"  // IWYU pragma: keep
#include "log.hh"
#include "persistence.hh"
#include "root.hh"
#include "status.hh"
#include "vk.hh"
#include "window.hh"
#include "x11.hh"

#pragma comment(lib, "vk-bootstrap")
#pragma comment(lib, "xcb")
#pragma comment(lib, "xcb-xinput")

// See http://who-t.blogspot.com/search/label/xi2 for XInput2 documentation.

using namespace automat;

constexpr bool kDebugWindowManager = false;

xcb_connection_t* connection;
xcb_window_t xcb_window;
xcb_screen_t* screen;

#define WRAP(f, ...)                             \
  std::unique_ptr<f##_reply_t, void (*)(void*)>( \
      f##_reply(connection, f(connection, __VA_ARGS__), nullptr), free)

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

#define DECLARE_ATOM(name) xcb_atom_t name;

ATOMS(DECLARE_ATOM)

#undef DECLARE_ATOM

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

int client_width = 1280;
int client_height = 720;
uint8_t xi_opcode;

Vec2 window_position_on_screen;
Vec2 mouse_position_on_screen;

Vec<SystemEventHook*> system_event_hooks;

struct VerticalScroll {
  xcb_input_device_id_t device_id;
  uint16_t valuator_number;
  double increment;
  double last_value;
};

std::optional<VerticalScroll> vertical_scroll;

std::unique_ptr<gui::Pointer> mouse;

float DisplayPxPerMeter() {
  return 1000.0f * screen->width_in_pixels / screen->width_in_millimeters;
}

Vec2 WindowSize() { return Vec2(client_width, client_height) / DisplayPxPerMeter(); }

// "Screen" coordinates are in pixels and their origin is in the upper left
// corner. "Window" coordinates are in meters and their origin is in the bottom
// left window corner.

namespace automat::gui {
Vec2 ScreenToWindow(Vec2 screen) {
  Vec2 window = (screen - window_position_on_screen - Vec2(0, client_height)) / DisplayPxPerMeter();
  window.y = -window.y;
  return window;
}

Vec2 WindowToScreen(Vec2 window) {
  window.y = -window.y;
  return window * DisplayPxPerMeter() + window_position_on_screen + Vec2(0, client_height);
}

Vec2 GetMainPointerScreenPos() { return mouse_position_on_screen; }
}  // namespace automat::gui

using namespace automat::gui;

gui::Pointer& GetMouse() {
  if (!mouse) {
    mouse = std::make_unique<gui::Pointer>(*window, ScreenToWindow(mouse_position_on_screen));
  }
  return *mouse;
}

gui::PointerButton EventDetailToButton(uint32_t detail) {
  switch (detail) {
    case 1:
      return gui::PointerButton::Left;
    case 2:
      return gui::PointerButton::Middle;
    case 3:
      return gui::PointerButton::Right;
    default:
      return gui::PointerButton::Unknown;
  }
}

const char kWindowName[] = "Automat";

float fp1616_to_float(xcb_input_fp1616_t fp) { return fp / 65536.0f; }
double fp3232_to_double(xcb_input_fp3232_t fp) { return fp.integral + fp.frac / 4294967296.0; }

void ScanDevices() {
  vertical_scroll.reset();
  if (auto reply = WRAP(xcb_input_xi_query_device, XCB_INPUT_DEVICE_ALL_MASTER)) {
    int n_devices = xcb_input_xi_query_device_infos_length(reply.get());
    auto it_device = xcb_input_xi_query_device_infos_iterator(reply.get());
    for (int i_device = 0; i_device < n_devices; ++i_device) {
      xcb_input_device_id_t deviceid = it_device.data->deviceid;
      int n_classes = xcb_input_xi_device_info_classes_length(it_device.data);
      auto it_classes = xcb_input_xi_device_info_classes_iterator(it_device.data);

      xcb_input_valuator_class_t* valuator_by_number[n_classes];
      for (int i_class = 0; i_class < n_classes; ++i_class) {
        uint16_t class_type = it_classes.data->type;
        switch (class_type) {
          case XCB_INPUT_DEVICE_CLASS_TYPE_VALUATOR: {
            xcb_input_valuator_class_t* valuator_class =
                (xcb_input_valuator_class_t*)it_classes.data;
            valuator_by_number[valuator_class->number] = valuator_class;
            break;
          }
          case XCB_INPUT_DEVICE_CLASS_TYPE_SCROLL: {
            xcb_input_scroll_class_t* scroll_class = (xcb_input_scroll_class_t*)it_classes.data;
            std::string scroll_type = scroll_class->scroll_type == XCB_INPUT_SCROLL_TYPE_VERTICAL
                                          ? "vertical"
                                          : "horizontal";
            std::string increment = std::to_string(fp3232_to_double(scroll_class->increment));
            if (scroll_class->scroll_type == XCB_INPUT_SCROLL_TYPE_VERTICAL) {
              vertical_scroll.emplace(deviceid, scroll_class->number,
                                      fp3232_to_double(scroll_class->increment), 0.0);
            }
            break;
          }
        }
        xcb_input_device_class_next(&it_classes);
      }

      if (vertical_scroll && vertical_scroll->device_id == deviceid) {
        xcb_input_valuator_class_t* valuator = valuator_by_number[vertical_scroll->valuator_number];
        vertical_scroll->last_value = fp3232_to_double(valuator->value);
      }

      xcb_input_xi_device_info_next(&it_device);
    }
  }
}

struct WM_STATE {
  bool MODAL = false;
  bool STICKY = false;
  bool MAXIMIZED_VERT = false;
  bool MAXIMIZED_HORZ = false;
  bool SHADED = false;
  bool SKIP_TASKBAR = false;
  bool SKIP_PAGER = false;
  bool HIDDEN = false;
  bool FULLSCREEN = false;
  bool ABOVE = false;
  bool BELOW = false;
  bool DEMANDS_ATTENTION = false;

  static WM_STATE Get() {
    using namespace atom;
    WM_STATE state;
    xcb_get_property_reply_t* reply = xcb_get_property_reply(
        connection, xcb_get_property(connection, 0, xcb_window, _NET_WM_STATE, XCB_ATOM_ANY, 0, 32),
        nullptr);
    if (reply) {
      xcb_atom_t* atoms = (xcb_atom_t*)xcb_get_property_value(reply);
      int n_atoms = xcb_get_property_value_length(reply) / sizeof(xcb_atom_t);
      std::map<xcb_atom_t, Fn<void()>> callbacks = {
          {_NET_WM_STATE_MODAL, [&] { state.MODAL = true; }},
          {_NET_WM_STATE_STICKY, [&] { state.STICKY = true; }},
          {_NET_WM_STATE_MAXIMIZED_VERT, [&] { state.MAXIMIZED_VERT = true; }},
          {_NET_WM_STATE_MAXIMIZED_HORZ, [&] { state.MAXIMIZED_HORZ = true; }},
          {_NET_WM_STATE_SHADED, [&] { state.SHADED = true; }},
          {_NET_WM_STATE_SKIP_TASKBAR, [&] { state.SKIP_TASKBAR = true; }},
          {_NET_WM_STATE_SKIP_PAGER, [&] { state.SKIP_PAGER = true; }},
          {_NET_WM_STATE_HIDDEN, [&] { state.HIDDEN = true; }},
          {_NET_WM_STATE_FULLSCREEN, [&] { state.FULLSCREEN = true; }},
          {_NET_WM_STATE_ABOVE, [&] { state.ABOVE = true; }},
          {_NET_WM_STATE_BELOW, [&] { state.BELOW = true; }},
          {_NET_WM_STATE_DEMANDS_ATTENTION, [&] { state.DEMANDS_ATTENTION = true; }},
      };
      for (int i = 0; i < n_atoms; ++i) {
        if (auto cb = callbacks.find(atoms[i]); cb != callbacks.end()) {
          cb->second();
        }
      }
      free(reply);
    }
    return state;
  }

  void Set() {
    using namespace atom;
    Vec<xcb_atom_t> atoms;
    if (MODAL) atoms.push_back(_NET_WM_STATE_MODAL);
    if (STICKY) atoms.push_back(_NET_WM_STATE_STICKY);
    if (MAXIMIZED_VERT) atoms.push_back(_NET_WM_STATE_MAXIMIZED_VERT);
    if (MAXIMIZED_HORZ) atoms.push_back(_NET_WM_STATE_MAXIMIZED_HORZ);
    if (SHADED) atoms.push_back(_NET_WM_STATE_SHADED);
    if (SKIP_TASKBAR) atoms.push_back(_NET_WM_STATE_SKIP_TASKBAR);
    if (SKIP_PAGER) atoms.push_back(_NET_WM_STATE_SKIP_PAGER);
    if (HIDDEN) atoms.push_back(_NET_WM_STATE_HIDDEN);
    if (FULLSCREEN) atoms.push_back(_NET_WM_STATE_FULLSCREEN);
    if (ABOVE) atoms.push_back(_NET_WM_STATE_ABOVE);
    if (BELOW) atoms.push_back(_NET_WM_STATE_BELOW);
    if (DEMANDS_ATTENTION) atoms.push_back(_NET_WM_STATE_DEMANDS_ATTENTION);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcb_window, _NET_WM_STATE, XCB_ATOM_ATOM,
                        32, atoms.size(), atoms.data());
  }
};

Vec<xcb_atom_t> GetPropertyAtoms(xcb_atom_t property) {
  xcb_get_property_reply_t* reply = xcb_get_property_reply(
      connection, xcb_get_property(connection, 0, xcb_window, property, XCB_ATOM_ANY, 0, 32),
      nullptr);
  Vec<xcb_atom_t> vec;
  if (reply) {
    xcb_atom_t* atoms = (xcb_atom_t*)xcb_get_property_value(reply);
    int n_atoms = xcb_get_property_value_length(reply) / sizeof(xcb_atom_t);
    vec.reserve(n_atoms);
    for (int i = 0; i < n_atoms; ++i) {
      vec.push_back(atoms[i]);
    }
    free(reply);
  }
  return vec;
}

void ConnectXCB(Status& status) {
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
}

void CreateWindow(Status& status) {
  xcb_window = xcb_generate_id(connection);
  uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t value_list[] = {screen->white_pixel, XCB_EVENT_MASK_EXPOSURE |
                                                    XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                                                    XCB_EVENT_MASK_PROPERTY_CHANGE};

  xcb_create_window(connection, XCB_COPY_FROM_PARENT, xcb_window, screen->root, 0, 0, client_width,
                    client_height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                    value_mask, value_list);

  WM_STATE wm_state = WM_STATE::Get();
  wm_state.MAXIMIZED_HORZ = window->maximized_horizontally;
  wm_state.MAXIMIZED_VERT = window->maximized_vertically;
  wm_state.ABOVE = window->always_on_top;
  wm_state.Set();

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcb_window, XCB_ATOM_WM_NAME,
                      XCB_ATOM_STRING, 8, sizeof(kWindowName), kWindowName);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcb_window, atom::WM_PROTOCOLS, 4, 32, 1,
                      &atom::WM_DELETE_WINDOW);

  xcb_map_window(connection, xcb_window);

  if (!isnan(window->output_device_x)) {
    uint32_t x = 0;
    if (window->output_device_x >= 0) {
      x = roundf(window->output_device_x * DisplayPxPerMeter());
    } else {
      x = roundf(screen->width_in_pixels + window->output_device_x * DisplayPxPerMeter() -
                 client_width);
    }
    xcb_configure_window(connection, xcb_window, XCB_CONFIG_WINDOW_X, &x);
  }
  if (!isnan(window->output_device_y)) {
    uint32_t y = 0;
    if (window->output_device_y >= 0) {
      y = roundf(window->output_device_y * DisplayPxPerMeter());
    } else {
      y = roundf(screen->height_in_pixels + window->output_device_y * DisplayPxPerMeter() -
                 client_height);
    }
    xcb_configure_window(connection, xcb_window, XCB_CONFIG_WINDOW_Y, &y);
  }

  xcb_flush(connection);

  const xcb_query_extension_reply_t* xinput_data =
      xcb_get_extension_data(connection, &xcb_input_id);
  if (!xinput_data->present) {
    AppendErrorMessage(status) += "XInput extension not present.";
    return;
  }
  xi_opcode = xinput_data->major_opcode;

  if (auto reply =
          WRAP(xcb_input_xi_query_version, XCB_INPUT_MAJOR_VERSION, XCB_INPUT_MINOR_VERSION)) {
    std::pair<int, int> server_version(reply->major_version, reply->minor_version);
    std::pair<int, int> required_version(2, 2);
    if (server_version < required_version) {
      AppendErrorMessage(status) += "XInput version 2.2 or higher required for multitouch.";
      return;
    }
  } else {
    AppendErrorMessage(status) += "Failed to query XInput version.";
    return;
  }

  struct input_event_mask {
    xcb_input_event_mask_t header = {
        .deviceid = XCB_INPUT_DEVICE_ALL_MASTER,
        .mask_len = 1,
    };
    uint32_t mask = XCB_INPUT_XI_EVENT_MASK_DEVICE_CHANGED | XCB_INPUT_XI_EVENT_MASK_KEY_PRESS |
                    XCB_INPUT_XI_EVENT_MASK_KEY_RELEASE | XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS |
                    XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE | XCB_INPUT_XI_EVENT_MASK_MOTION |
                    XCB_INPUT_XI_EVENT_MASK_ENTER | XCB_INPUT_XI_EVENT_MASK_LEAVE |
                    XCB_INPUT_XI_EVENT_MASK_FOCUS_IN | XCB_INPUT_XI_EVENT_MASK_FOCUS_OUT |
                    XCB_INPUT_XI_EVENT_MASK_TOUCH_BEGIN | XCB_INPUT_XI_EVENT_MASK_TOUCH_UPDATE |
                    XCB_INPUT_XI_EVENT_MASK_TOUCH_END;
  } event_mask;

  xcb_void_cookie_t cookie =
      xcb_input_xi_select_events_checked(connection, xcb_window, 1, &event_mask.header);
  if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(connection, cookie)}) {
    AppendErrorMessage(status) += f("Failed to select events: %d", error->error_code);
    return;
  }

  ScanDevices();
}

#undef WRAP

void Paint() {
#ifdef CPU_RENDERING
  auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(client_width, client_height));

  SkCanvas& canvas = *surface->getCanvas();
  canvas.translate(0, client_height);
  canvas.scale(1, -1);

  xcb_gcontext_t graphics_context = xcb_generate_id(connection);
  xcb_create_gc(connection, graphics_context, xcb_window, 0, nullptr);

#else
  SkCanvas& canvas = *vk::GetBackbufferCanvas();
#endif
  canvas.save();
  canvas.translate(0, 0);
  canvas.scale(DisplayPxPerMeter(), DisplayPxPerMeter());
  if (window) {
    window->Draw(canvas);
  }
  canvas.restore();
#ifdef CPU_RENDERING
  SkPixmap pixmap;
  if (!surface->peekPixels(&pixmap)) {
    FATAL << "Failed to peek pixels.";
  }
  xcb_void_cookie_t cookie =
      xcb_put_image_checked(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, xcb_window, graphics_context,
                            client_width, client_height, 0, 0, 0, screen->root_depth,
                            pixmap.computeByteSize(), (const U8*)pixmap.addr());
  if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(connection, cookie)}) {
    ERROR << "Failed to put image: " << error->error_code;
  }
  xcb_free_gc(connection, graphics_context);
#else
  vk::Present();
#endif
}

void RenderLoop() {
  std::atomic_bool running = true;
  // TODO: maybe use unique_ptr here
  xcb_generic_event_t *event, *peeked_event = nullptr;
  bool keys_down[256] = {0};

  std::stop_callback on_automat_stop(automat_thread.get_stop_token(), [&] { running = false; });

  while (running) {
    if (peeked_event) {
      event = peeked_event;
      peeked_event = nullptr;
    } else {
      event = xcb_poll_for_event(connection);
    }

    if (event) {
      for (auto hook : system_event_hooks) {
        if (hook->Intercept(event)) {
          goto intercepted;
        }
      }

      auto opcode = event->response_type & ~0x80;
      switch (opcode) {
        case XCB_EXPOSE: {
          xcb_expose_event_t* ev = (xcb_expose_event_t*)event;
          // ev-count is the number of expose events that are still in the queue.
          // We only want to do a full redraw on the last expose event.
          if (ev->count == 0) {
            Paint();
          }
          break;
        }
        case XCB_MAP_NOTIFY: {  // ignore
          xcb_map_notify_event_t* ev = (xcb_map_notify_event_t*)event;
          break;
        }
        case XCB_REPARENT_NOTIFY: {  // ignore
          xcb_reparent_notify_event_t* ev = (xcb_reparent_notify_event_t*)event;
          break;
        }
        case XCB_CONFIGURE_NOTIFY: {
          xcb_configure_notify_event_t* ev = (xcb_configure_notify_event_t*)event;
          if (ev->width != client_width || ev->height != client_height) {
            client_width = ev->width;
            client_height = ev->height;

            if (auto err = vk::Resize(client_width, client_height); !err.empty()) {
              ERROR << err;
            }
            window->Resize(WindowSize());
          }

          // This event may be sent when the window is moved. However sometimes it holds the wrong
          // coordinates. This happens for example on Ubuntu 22.04 - only events sent from the
          // window manager are correct. Querying the position from geometry also returns the wrong
          // position. The only way to get the correct on-screen position that was found to be
          // reliable was to translate the point 0, 0 to root window coordinates.
          xcb_translate_coordinates_reply_t* reply = xcb_translate_coordinates_reply(
              connection, xcb_translate_coordinates(connection, xcb_window, screen->root, 0, 0),
              nullptr);
          window_position_on_screen.x = reply->dst_x;
          window_position_on_screen.y = reply->dst_y;

          if (window_position_on_screen.x <= screen->width_in_pixels / 2.f) {
            window->output_device_x = window_position_on_screen.x / DisplayPxPerMeter();
          } else {
            // store the distance from the right screen edge, as a negative number
            window->output_device_x =
                (window_position_on_screen.x + client_width - (int)screen->width_in_pixels) /
                DisplayPxPerMeter();
          }
          if (window_position_on_screen.y <= screen->height_in_pixels / 2.f) {
            window->output_device_y = window_position_on_screen.y / DisplayPxPerMeter();
          } else {
            // store the distance from the bottom screen edge, as a negative number
            window->output_device_y =
                (window_position_on_screen.y + client_height - (int)screen->height_in_pixels) /
                DisplayPxPerMeter();
          }
          break;
        }
        case XCB_PROPERTY_NOTIFY: {
          xcb_property_notify_event_t* ev = (xcb_property_notify_event_t*)event;
          if (ev->atom == atom::_NET_WM_STATE) {
            WM_STATE wm_state = WM_STATE::Get();
            window->maximized_horizontally = wm_state.MAXIMIZED_HORZ;
            window->maximized_vertically = wm_state.MAXIMIZED_VERT;
            window->always_on_top = wm_state.ABOVE;
          } else if (kDebugWindowManager) {
            LOG << "Unhandled property notify event " << atom::ToStr(ev->atom) << ": "
                << dump_struct(*ev);
          }
          break;
        }
        case XCB_CLIENT_MESSAGE: {
          xcb_client_message_event_t* cm = (xcb_client_message_event_t*)event;
          if (cm->data.data32[0] == atom::WM_DELETE_WINDOW) running = false;
          break;
        }
        case XCB_MAPPING_NOTIFY: {
          xcb_mapping_notify_event_t* ev = (xcb_mapping_notify_event_t*)event;
          // TODO: check this out
          // https://tronche.com/gui/x/xlib/events/window-state-change/mapping.html
          break;
        }
        case XCB_GE_GENERIC: {
          xcb_ge_generic_event_t* ev = (xcb_ge_generic_event_t*)event;
          if (ev->extension == xi_opcode) {
            switch (ev->event_type) {
              case XCB_INPUT_DEVICE_CHANGED: {
                // This event usually indicates that the slave device has changed.
                // We should update the scroll valua based on the valuator from the
                // current slave.
                xcb_input_device_changed_event_t* ev = (xcb_input_device_changed_event_t*)event;
                if (vertical_scroll && ev->deviceid == vertical_scroll->device_id) {
                  if (ev->reason == XCB_INPUT_CHANGE_REASON_SLAVE_SWITCH) {
                    xcb_input_device_class_iterator_t it =
                        xcb_input_device_changed_classes_iterator(ev);
                    while (it.rem) {
                      xcb_input_device_class_t* it_class = it.data;
                      if (it_class->type == XCB_INPUT_DEVICE_CLASS_TYPE_VALUATOR) {
                        xcb_input_valuator_class_t* valuator_class =
                            (xcb_input_valuator_class_t*)it_class;
                        if (valuator_class->number == vertical_scroll->valuator_number) {
                          vertical_scroll->last_value = fp3232_to_double(valuator_class->value);
                        }
                      }
                      xcb_input_device_class_next(&it);
                    }
                  } else {
                    // TODO: handle other reasons more gracefully
                    ScanDevices();
                  }
                }
                break;
              }
              case XCB_INPUT_RAW_KEY_PRESS: {
                gui::keyboard->KeyDown(*(xcb_input_raw_key_press_event_t*)event);
                break;
              }
              case XCB_INPUT_KEY_PRESS: {
                gui::keyboard->KeyDown(*(xcb_input_key_press_event_t*)event);
                break;
              }
              case XCB_INPUT_RAW_KEY_RELEASE: {
                gui::keyboard->KeyUp(*(xcb_input_raw_key_release_event_t*)event);
                break;
              }
              case XCB_INPUT_KEY_RELEASE: {
                gui::keyboard->KeyUp(*(xcb_input_key_release_event_t*)event);
                break;
              }
              case XCB_INPUT_BUTTON_PRESS: {
                xcb_input_button_press_event_t* ev = (xcb_input_button_press_event_t*)event;
                // Ignore emulated mouse wheel "buttons"
                if (ev->flags & XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED) {
                  break;
                }
                // auto cookie =
                //     xcb_grab_pointer(connection, 0, xcb_window,
                //                      XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_MOTION
                //                      |
                //                          XCB_EVENT_MASK_POINTER_MOTION_HINT,
                //                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE,
                //                      XCB_CURSOR_NONE, XCB_CURRENT_TIME);
                // xcb_flush(connection);
                // xcb_request_check(connection, cookie)

                RunOnAutomatThread(
                    [detail = ev->detail] { GetMouse().ButtonDown(EventDetailToButton(detail)); });
                break;
              }
              case XCB_INPUT_BUTTON_RELEASE: {
                xcb_input_button_release_event_t* ev = (xcb_input_button_release_event_t*)event;
                // Ignore emulated mouse wheel "buttons"
                if (ev->flags & XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED) {
                  break;
                }
                RunOnAutomatThread(
                    [detail = ev->detail] { GetMouse().ButtonUp(EventDetailToButton(detail)); });
                break;
              }
              case XCB_INPUT_MOTION: {
                xcb_input_motion_event_t* ev = (xcb_input_motion_event_t*)event;

                if (vertical_scroll && ev->deviceid == vertical_scroll->device_id) {
                  xcb_input_fp3232_t* axisvalues = xcb_input_button_press_axisvalues(ev);
                  int n_axisvalues = xcb_input_button_press_axisvalues_length(ev);
                  uint32_t* valuator_mask = xcb_input_button_press_valuator_mask(ev);
                  int mask_len = xcb_input_button_press_valuator_mask_length(ev);
                  int i_axis = 0;
                  int i_valuator = 0;
                  for (int i_mask = 0; i_mask < mask_len; ++i_mask) {
                    uint32_t mask = valuator_mask[i_mask];
                    while (mask) {
                      if (mask & 1) {
                        if (i_valuator == vertical_scroll->valuator_number) {
                          double new_value = fp3232_to_double(axisvalues[i_axis]);
                          double delta = new_value - vertical_scroll->last_value;
                          vertical_scroll->last_value = new_value;
                          if (abs(delta) > 1000000) {
                            // http://who-t.blogspot.com/2012/06/xi-21-protocol-design-issues.html
                            delta = (delta > 0 ? 1 : -1) * vertical_scroll->increment;
                          }
                          RunOnAutomatThread(
                              [=] { GetMouse().Wheel(-delta / vertical_scroll->increment); });
                        }
                        ++i_axis;
                      }
                      mask >>= 1;
                      ++i_valuator;
                    }
                  }
                }
                mouse_position_on_screen.x = fp1616_to_float(ev->root_x);
                mouse_position_on_screen.y = fp1616_to_float(ev->root_y);
                RunOnAutomatThread(
                    [=] { GetMouse().Move(ScreenToWindow(mouse_position_on_screen)); });
                break;
              }
              case XCB_INPUT_ENTER: {
                if (vertical_scroll) {
                  // See
                  // http://who-t.blogspot.com/2012/06/xi-21-protocol-design-issues.html
                  // Instead of ignoring the first update, we're refreshing the
                  // last_scroll. It's a bit more expensive than the GTK approach,
                  // but gives better UX.
                  ScanDevices();
                }
                xcb_input_enter_event_t* ev = (xcb_input_enter_event_t*)event;
                mouse_position_on_screen.x = fp1616_to_float(ev->root_x);
                mouse_position_on_screen.y = fp1616_to_float(ev->root_y);
                RunOnAutomatThread(
                    [=] { GetMouse().Move(ScreenToWindow(mouse_position_on_screen)); });
                break;
              }
              case XCB_INPUT_LEAVE: {
                break;
              }
              case XCB_INPUT_FOCUS_IN: {
                break;
              }
              case XCB_INPUT_FOCUS_OUT: {
                break;
              }
              case XCB_INPUT_TOUCH_BEGIN: {
                break;
              }
              case XCB_INPUT_TOUCH_UPDATE: {
                break;
              }
              case XCB_INPUT_TOUCH_END: {
                break;
              }
              default: {
                LOG << "Unknown XI event (event_type=" << ev->event_type << ")";
                break;
              }
            }
          } else {
            LOG << "Unknown XCB_GE_GENERIC event (extension=" << ev->extension
                << ", event_type=" << ev->event_type << ")";
          }
          break;
        }
        case XCB_KEY_PRESS:  // fallthrough
        case XCB_KEY_RELEASE: {
          // This is only called by registered hotkeys
          xcb_key_press_event_t* ev = (xcb_key_press_event_t*)event;
          auto key = x11::X11KeyCodeToKey((x11::KeyCode)ev->detail);
          // LOG << "Key event: " << dump_struct(*ev) << " " << automat::gui::ToStr(key);
          if (opcode == XCB_KEY_RELEASE) {
            peeked_event = xcb_poll_for_event(connection);
            if (peeked_event && peeked_event->response_type == XCB_KEY_PRESS) {
              xcb_key_press_event_t* peeked_ev = (xcb_key_press_event_t*)peeked_event;
              if (peeked_ev->time == ev->time && peeked_ev->detail == ev->detail) {
                // Ignore key repeats
                break;
              }
            }
          }
          if (opcode == XCB_KEY_PRESS && keys_down[ev->detail]) {
            // Ignore key repeats
            break;
          }
          if (opcode == XCB_KEY_PRESS) {
            keys_down[ev->detail] = true;
          } else {
            keys_down[ev->detail] = false;
          }
          gui::Key key_struct = {
              .physical = key,
              .logical = key,
          };
          bool handled = false;
          for (auto& key_grab : keyboard->key_grabs) {
            if (key_grab->key == key) {
              if (opcode == XCB_KEY_PRESS) {
                key_grab->grabber.KeyGrabberKeyDown(*key_grab);
              } else {
                key_grab->grabber.KeyGrabberKeyUp(*key_grab);
              }
              handled = true;
              break;
            }
          }
          if (opcode == XCB_KEY_PRESS) {
            gui::keyboard->LogKeyDown(key_struct);
          } else {
            gui::keyboard->LogKeyUp(key_struct);
          }
          if (!handled) {
            if (opcode == XCB_KEY_PRESS) {
              gui::keyboard->KeyDown(*(xcb_input_key_press_event_t*)event);
            } else {
              gui::keyboard->KeyUp(*(xcb_input_key_release_event_t*)event);
            }
          }
          break;
        }
        default:
          LOG << "Unhandled event: " << dump_struct(*event);
          break;
      }
    } else {  // event == nullptr
      Paint();
    }
  intercepted:
    free(event);
  }
}

void automat::StopAutomat(maf::Status&) { automat_thread.request_stop(); }

int LinuxMain(int argc, char* argv[]) {
  audio::Init(&argc, &argv);
  SkGraphics::Init();
  Status status;
  ConnectXCB(status);
  if (!OK(status)) {
    FATAL << status;
  }

  InitAutomat(status);
  if (!OK(status)) {
    ERROR << "Failed to initialize Automat: " << status;
    status.Reset();  // try to continue
  }

  float pixels_per_meter = DisplayPxPerMeter();
  client_width = window->size.x * pixels_per_meter;
  client_height = window->size.y * pixels_per_meter;

  CreateWindow(status);
  if (!OK(status)) {
    FATAL << "Failed to create window: " << status;
  }

  window->DisplayPixelDensity(DisplayPxPerMeter());
  window->RequestResize = [&](Vec2 new_size) {
    const static uint32_t values[] = {
        static_cast<uint32_t>(new_size.x * window->display_pixels_per_meter),
        static_cast<uint32_t>(new_size.y * window->display_pixels_per_meter)};
    xcb_configure_window(connection, xcb_window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
    xcb_flush(connection);
  };
  window->RequestMaximize = nullptr;

#ifdef CPU_RENDERING
#else
  if (auto err = vk::Init(); !err.empty()) {
    FATAL << "Failed to initialize Vulkan: " << err;
  }
#endif

  RenderLoop();

  StopRoot();

  SaveState(*window, status);
  if (!OK(status)) {
    ERROR << "Failed to save state: " << status;
  }

  root_machine->locations.clear();

  mouse.reset();
  keyboard.reset();
  window.reset();

  vk::Destroy();
  xcb_destroy_window(connection, xcb_window);

  audio::Stop();

  LOG << "Exiting.";

  return 0;
}
