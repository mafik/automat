// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "xcb_window.hh"

#include <stop_token>

#include "automat.hh"
#include "fn.hh"
#include "vec.hh"
#include "vk.hh"
#include "x11.hh"
#include "xcb.hh"

using namespace maf;

namespace xcb {

static float DisplayPxPerMeter() {
  return 1000.0f * screen->width_in_pixels / screen->width_in_millimeters;
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

  static WM_STATE Get(xcb_window_t xcb_window) {
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

  void Set(xcb_window_t xcb_window) {
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

float fp1616_to_float(xcb_input_fp1616_t fp) { return fp / 65536.0f; }
double fp3232_to_double(xcb_input_fp3232_t fp) { return fp.integral + fp.frac / 4294967296.0; }

// TODO: rename
static void ScanDevices(XCBWindow& os_window) {
  os_window.vertical_scroll.reset();
  if (auto reply = xcb::input_xi_query_device(XCB_INPUT_DEVICE_ALL_MASTER)) {
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
              os_window.vertical_scroll.emplace(deviceid, scroll_class->number,
                                                fp3232_to_double(scroll_class->increment), 0.0);
            }
            break;
          }
        }
        xcb_input_device_class_next(&it_classes);
      }

      if (os_window.vertical_scroll && os_window.vertical_scroll->device_id == deviceid) {
        xcb_input_valuator_class_t* valuator =
            valuator_by_number[os_window.vertical_scroll->valuator_number];
        os_window.vertical_scroll->last_value = fp3232_to_double(valuator->value);
      }

      xcb_input_xi_device_info_next(&it_device);
    }
  }
}

std::unique_ptr<automat::gui::OSWindow> XCBWindow::Make(automat::gui::Window& root,
                                                        maf::Status& status) {
  xcb::Connect(status);
  if (!OK(status)) {
    return nullptr;
  }
  auto os_window = std::unique_ptr<XCBWindow>(new XCBWindow(root));
  float pixels_per_meter = DisplayPxPerMeter();
  os_window->client_width = root.size.x * pixels_per_meter;
  os_window->client_height = root.size.y * pixels_per_meter;

  os_window->xcb_window = xcb_generate_id(connection);
  uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t value_list[] = {screen->white_pixel, XCB_EVENT_MASK_EXPOSURE |
                                                    XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                                                    XCB_EVENT_MASK_PROPERTY_CHANGE};

  xcb_create_window(connection, XCB_COPY_FROM_PARENT, os_window->xcb_window, screen->root, 0, 0,
                    os_window->client_width, os_window->client_height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, value_mask, value_list);

  WM_STATE wm_state = WM_STATE::Get(os_window->xcb_window);
  wm_state.MAXIMIZED_HORZ = root.maximized_horizontally;
  wm_state.MAXIMIZED_VERT = root.maximized_vertically;
  wm_state.ABOVE = root.always_on_top;
  wm_state.Set(os_window->xcb_window);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, os_window->xcb_window, XCB_ATOM_WM_NAME,
                      XCB_ATOM_STRING, 8, sizeof(automat::gui::kWindowName),
                      automat::gui::kWindowName);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, os_window->xcb_window, atom::WM_PROTOCOLS,
                      4, 32, 1, &atom::WM_DELETE_WINDOW);

  xcb_map_window(connection, os_window->xcb_window);

  if (!isnan(root.output_device_x)) {
    uint32_t x = 0;
    if (root.output_device_x >= 0) {
      x = roundf(root.output_device_x * DisplayPxPerMeter());
    } else {
      x = roundf(screen->width_in_pixels + root.output_device_x * DisplayPxPerMeter() -
                 os_window->client_width);
    }
    xcb_configure_window(connection, os_window->xcb_window, XCB_CONFIG_WINDOW_X, &x);
  }
  if (!isnan(root.output_device_y)) {
    uint32_t y = 0;
    if (root.output_device_y >= 0) {
      y = roundf(root.output_device_y * DisplayPxPerMeter());
    } else {
      y = roundf(screen->height_in_pixels + root.output_device_y * DisplayPxPerMeter() -
                 os_window->client_height);
    }
    xcb_configure_window(connection, os_window->xcb_window, XCB_CONFIG_WINDOW_Y, &y);
  }

  xcb_flush(connection);

  if (auto reply = xcb::input_xi_query_version(XCB_INPUT_MAJOR_VERSION, XCB_INPUT_MINOR_VERSION)) {
    std::pair<int, int> server_version(reply->major_version, reply->minor_version);
    std::pair<int, int> required_version(2, 2);
    if (server_version < required_version) {
      AppendErrorMessage(status) += "XInput version 2.2 or higher required for multitouch.";
      return nullptr;
    }
  } else {
    AppendErrorMessage(status) += "Failed to query XInput version.";
    return nullptr;
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
      xcb_input_xi_select_events_checked(connection, os_window->xcb_window, 1, &event_mask.header);
  if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(connection, cookie)}) {
    AppendErrorMessage(status) += f("Failed to select events: %d", error->error_code);
    return nullptr;
  }

  ScanDevices(*os_window);

  root.DisplayPixelDensity(DisplayPxPerMeter());
  root.RequestResize = [&](Vec2 new_size) {
    const static uint32_t values[] = {
        static_cast<uint32_t>(new_size.x * root.display_pixels_per_meter),
        static_cast<uint32_t>(new_size.y * root.display_pixels_per_meter)};
    xcb_configure_window(connection, os_window->xcb_window,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    xcb_flush(connection);
  };
  root.RequestMaximize = nullptr;

  return os_window;
}

XCBWindow::~XCBWindow() {
  if (xcb_window) {
    xcb_destroy_window(xcb::connection, xcb_window);
    xcb_window = 0;
  }
}

automat::gui::Pointer& XCBWindow::GetMouse() {
  if (!mouse) {
    mouse =
        std::make_unique<automat::gui::Pointer>(root, ScreenToWindowPx(mouse_position_on_screen));
  }
  return *mouse;
}

Vec2 XCBWindow::ScreenToWindowPx(Vec2 screen) {
  Vec2 window = (screen - window_position_on_screen - Vec2(0, client_height));
  window.y = -window.y;
  return window;
}

Vec2 XCBWindow::WindowPxToScreen(Vec2 window) {
  window.y = -window.y;
  return window + window_position_on_screen + Vec2(0, client_height);
}

static automat::gui::PointerButton EventDetailToButton(uint32_t detail) {
  switch (detail) {
    case 1:
      return automat::gui::PointerButton::Left;
    case 2:
      return automat::gui::PointerButton::Middle;
    case 3:
      return automat::gui::PointerButton::Right;
    default:
      return automat::gui::PointerButton::Unknown;
  }
}

void XCBWindow::MainLoop() {
  std::atomic_bool running = true;
  // TODO: maybe use unique_ptr here
  xcb_generic_event_t *event, *peeked_event = nullptr;
  bool keys_down[256] = {0};

  std::stop_callback on_automat_stop(automat::stop_source.get_token(), [&] { running = false; });

  while (running) {
    if (peeked_event) {
      event = peeked_event;
      peeked_event = nullptr;
    } else {
      event = xcb_wait_for_event(connection);
    }

    if (event) {
      auto opcode = event->response_type & ~0x80;
      switch (opcode) {
        case XCB_EXPOSE: {
          xcb_expose_event_t* ev = (xcb_expose_event_t*)event;
          // ev-count is the number of expose events that are still in the queue.
          // We only want to do a full redraw on the last expose event.
          if (ev->count == 0) {
            automat::gui::window->WakeAnimation();
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

            if (auto err = automat::vk::Resize(client_width, client_height); !err.empty()) {
              ERROR << err;
            }
            root.Resize(Vec2(client_width, client_height) / DisplayPxPerMeter());
          }

          // This event may be sent when the window is moved. However sometimes it holds the wrong
          // coordinates. This happens for example on Ubuntu 22.04 - only events sent from the
          // window manager are correct. Querying the position from geometry also returns the
          // wrong position. The only way to get the correct on-screen position that was found to
          // be reliable was to translate the point 0, 0 to root window coordinates.
          xcb_translate_coordinates_reply_t* reply = xcb_translate_coordinates_reply(
              connection, xcb_translate_coordinates(connection, xcb_window, screen->root, 0, 0),
              nullptr);
          window_position_on_screen.x = reply->dst_x;
          window_position_on_screen.y = reply->dst_y;

          if (window_position_on_screen.x <= screen->width_in_pixels / 2.f) {
            root.output_device_x = window_position_on_screen.x / DisplayPxPerMeter();
          } else {
            // store the distance from the right screen edge, as a negative number
            root.output_device_x =
                (window_position_on_screen.x + client_width - (int)screen->width_in_pixels) /
                DisplayPxPerMeter();
          }
          if (window_position_on_screen.y <= screen->height_in_pixels / 2.f) {
            root.output_device_y = window_position_on_screen.y / DisplayPxPerMeter();
          } else {
            // store the distance from the bottom screen edge, as a negative number
            root.output_device_y =
                (window_position_on_screen.y + client_height - (int)screen->height_in_pixels) /
                DisplayPxPerMeter();
          }
          break;
        }
        case XCB_PROPERTY_NOTIFY: {
          xcb_property_notify_event_t* ev = (xcb_property_notify_event_t*)event;
          if (ev->atom == atom::_NET_WM_STATE) {
            WM_STATE wm_state = WM_STATE::Get(xcb_window);
            root.maximized_horizontally = wm_state.MAXIMIZED_HORZ;
            root.maximized_vertically = wm_state.MAXIMIZED_VERT;
            root.always_on_top = wm_state.ABOVE;
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
                    ScanDevices(*this);
                  }
                }
                break;
              }
              case XCB_INPUT_RAW_KEY_PRESS: {
                automat::gui::keyboard->KeyDown(*(xcb_input_raw_key_press_event_t*)event);
                break;
              }
              case XCB_INPUT_KEY_PRESS: {
                automat::gui::keyboard->KeyDown(*(xcb_input_key_press_event_t*)event);
                break;
              }
              case XCB_INPUT_RAW_KEY_RELEASE: {
                automat::gui::keyboard->KeyUp(*(xcb_input_raw_key_release_event_t*)event);
                break;
              }
              case XCB_INPUT_KEY_RELEASE: {
                automat::gui::keyboard->KeyUp(*(xcb_input_key_release_event_t*)event);
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
                //                      XCB_EVENT_MASK_BUTTON_RELEASE |
                //                      XCB_EVENT_MASK_BUTTON_MOTION
                //                      |
                //                          XCB_EVENT_MASK_POINTER_MOTION_HINT,
                //                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE,
                //                      XCB_CURSOR_NONE, XCB_CURRENT_TIME);
                // xcb_flush(connection);
                // xcb_request_check(connection, cookie)
                auto lock = Lock();
                GetMouse().ButtonDown(EventDetailToButton(ev->detail));
                break;
              }
              case XCB_INPUT_BUTTON_RELEASE: {
                xcb_input_button_release_event_t* ev = (xcb_input_button_release_event_t*)event;
                // Ignore emulated mouse wheel "buttons"
                if (ev->flags & XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED) {
                  break;
                }
                auto lock = Lock();
                GetMouse().ButtonUp(EventDetailToButton(ev->detail));
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
                          auto lock = Lock();
                          GetMouse().Wheel(-delta / vertical_scroll->increment);
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
                auto lock = Lock();
                GetMouse().Move(ScreenToWindowPx(mouse_position_on_screen));
                break;
              }
              case XCB_INPUT_ENTER: {
                if (vertical_scroll) {
                  // See
                  // http://who-t.blogspot.com/2012/06/xi-21-protocol-design-issues.html
                  // Instead of ignoring the first update, we're refreshing the
                  // last_scroll. It's a bit more expensive than the GTK approach,
                  // but gives better UX.
                  ScanDevices(*this);
                }
                xcb_input_enter_event_t* ev = (xcb_input_enter_event_t*)event;
                mouse_position_on_screen.x = fp1616_to_float(ev->root_x);
                mouse_position_on_screen.y = fp1616_to_float(ev->root_y);
                auto lock = Lock();
                GetMouse().Move(ScreenToWindowPx(mouse_position_on_screen));
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
          automat::gui::Key key_struct = {
              .physical = key,
              .logical = key,
          };
          bool handled = false;
          for (auto& key_grab : automat::gui::keyboard->key_grabs) {
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
            automat::gui::keyboard->LogKeyDown(key_struct);
          } else {
            automat::gui::keyboard->LogKeyUp(key_struct);
          }
          if (!handled) {
            if (opcode == XCB_KEY_PRESS) {
              automat::gui::keyboard->KeyDown(*(xcb_input_key_press_event_t*)event);
            } else {
              automat::gui::keyboard->KeyUp(*(xcb_input_key_release_event_t*)event);
            }
          }
          break;
        }
        default:
          LOG << "Unhandled event: " << dump_struct(*event);
          break;
      }
    } else {  // event == nullptr
      root.WakeAnimation();
    }
    free(event);
  }
}

maf::Optional<Vec2> XCBWindow::MousePositionScreenPx() {
  return maf::Optional<Vec2>(mouse_position_on_screen);
}

}  // namespace xcb
