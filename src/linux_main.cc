#include "linux_main.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <include/core/SkGraphics.h>

#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>

#include "format.h"
#include "gui.h"
#include "library.h"
#include "log.h"
#include "root.h"
#include "vk.h"

#pragma comment(lib, "xcb")
#pragma comment(lib, "xcb-xinput")

// See http://who-t.blogspot.com/search/label/xi2 for XInput2 documentation.

using namespace automaton;

xcb_connection_t *connection;
xcb_window_t xcb_window;
xcb_screen_t *screen;
xcb_atom_t wm_protocols;
xcb_atom_t wm_delete_window;
int window_width = 1280;
int window_height = 720;
uint8_t xi_opcode;

vec2 window_position_on_screen;
vec2 mouse_position_on_screen;

struct VerticalScroll {
  xcb_input_device_id_t device_id;
  uint16_t valuator_number;
  double increment;
  double last_value;
};

std::optional<VerticalScroll> vertical_scroll;

std::unique_ptr<gui::Window> window;
std::unique_ptr<gui::Pointer> mouse;

float DisplayPxPerMeter() {
  return 1000.0f * screen->width_in_pixels / screen->width_in_millimeters;
}

vec2 WindowSize() {
  return Vec2(window_width, window_height) / DisplayPxPerMeter();
}

// "Screen" coordinates are in pixels and their origin is in the upper left
// corner. "Window" coordinates are in meters and their origin is in the bottom
// left window corner.

vec2 ScreenToWindow(vec2 screen) {
  vec2 window = (screen - window_position_on_screen - Vec2(0, window_height)) /
                DisplayPxPerMeter();
  window.Y = -window.Y;
  return window;
}

gui::Pointer &GetMouse() {
  if (!mouse) {
    mouse = std::make_unique<gui::Pointer>(
        *window, ScreenToWindow(mouse_position_on_screen));
  }
  return *mouse;
}

enum class KeyCode {
  Esc = 9,
  Digit1 = 10,
  Digit2 = 11,
  Digit3 = 12,
  Digit4 = 13,
  Digit5 = 14,
  Digit6 = 15,
  Digit7 = 16,
  Digit8 = 17,
  Digit9 = 18,
  Digit0 = 19,
  Minus = 20,
  Equals = 21,
  Backspace = 22,
  Tab = 23,
  Q = 24,
  W = 25,
  E = 26,
  R = 27,
  T = 28,
  Y = 29,
  U = 30,
  I = 31,
  O = 32,
  P = 33,
  LeftBracket = 34,
  RightBracket = 35,
  Return = 36,
  CtrlLeft = 37,
  A = 38,
  S = 39,
  D = 40,
  F = 41,
  G = 42,
  H = 43,
  J = 44,
  K = 45,
  L = 46,
  Semicolon = 47,
  Apostrophe = 48,
  Backtick = 49,
  ShiftLeft = 50,
  BackSlash = 51,
  Z = 52,
  X = 53,
  C = 54,
  V = 55,
  B = 56,
  N = 57,
  M = 58,
  Comma = 59,
  Dot = 60,
  Slash = 61,
  ShiftRight = 62,
  KpMultiply = 63,
  AltLeft = 64,
  Space = 65,
  CapsLock = 66,
  F1 = 67,
  F2 = 68,
  F3 = 69,
  F4 = 70,
  F5 = 71,
  F6 = 72,
  F7 = 73,
  F8 = 74,
  F9 = 75,
  F10 = 76,
  NumLock = 77,
  ScrollLock = 78,
  Kp7 = 79,
  Kp8 = 80,
  Kp9 = 81,
  KpMinus = 82,
  Kp4 = 83,
  Kp5 = 84,
  Kp6 = 85,
  KpPlus = 86,
  Kp1 = 87,
  Kp2 = 88,
  Kp3 = 89,
  Kp0 = 90,
  KpDot = 91,
  International = 94,
  F11 = 95,
  F12 = 96,
  Home = 97,
  Up = 98,
  PageUp = 99,
  Left = 100,
  Right = 102,
  Down = 104,
  End = 103,
  PageDown = 105,
  Insert = 106,
  Delete = 107,
  KpEnter = 108,
  CtrlRight = 109,
  Pause = 110,
  PrintScrn = 111,
  KpDivide = 112,
  AltRight = 113,
  SuperLeft = 115,
  SuperRight = 116,
  Menu = 117,
};

gui::Key X11KeyCodeToKey(KeyCode key_code) {
  switch (key_code) {
  case KeyCode::W:
    return gui::kKeyW;
  case KeyCode::A:
    return gui::kKeyA;
  case KeyCode::S:
    return gui::kKeyS;
  case KeyCode::D:
    return gui::kKeyD;
  default:
    return gui::kKeyUnknown;
  }
}

gui::Button EventDetailToButton(uint32_t detail) {
  switch (detail) {
  case 1:
    return gui::kMouseLeft;
  case 2:
    return gui::kMouseMiddle;
  case 3:
    return gui::kMouseRight;
  default:
    return gui::kButtonUnknown;
  }
}

const char kWindowName[] = "Automat";

float fp1616_to_float(xcb_input_fp1616_t fp) { return fp / 65536.0f; }
double fp3232_to_double(xcb_input_fp3232_t fp) {
  return fp.integral + fp.frac / 4294967296.0;
}

#define WRAP(f, ...)                                                           \
  std::unique_ptr<f##_reply_t>(                                                \
      f##_reply(connection, f(connection, __VA_ARGS__), nullptr))

void ScanDevices() {
  vertical_scroll.reset();
  if (auto reply =
          WRAP(xcb_input_xi_query_device, XCB_INPUT_DEVICE_ALL_MASTER)) {
    int n_devices = xcb_input_xi_query_device_infos_length(reply.get());
    auto it_device = xcb_input_xi_query_device_infos_iterator(reply.get());
    for (int i_device = 0; i_device < n_devices; ++i_device) {
      xcb_input_device_id_t deviceid = it_device.data->deviceid;
      int n_classes = xcb_input_xi_device_info_classes_length(it_device.data);
      auto it_classes =
          xcb_input_xi_device_info_classes_iterator(it_device.data);

      xcb_input_valuator_class_t *valuator_by_number[n_classes];
      for (int i_class = 0; i_class < n_classes; ++i_class) {
        uint16_t class_type = it_classes.data->type;
        switch (class_type) {
        case XCB_INPUT_DEVICE_CLASS_TYPE_VALUATOR: {
          xcb_input_valuator_class_t *valuator_class =
              (xcb_input_valuator_class_t *)it_classes.data;
          valuator_by_number[valuator_class->number] = valuator_class;
          break;
        }
        case XCB_INPUT_DEVICE_CLASS_TYPE_SCROLL: {
          xcb_input_scroll_class_t *scroll_class =
              (xcb_input_scroll_class_t *)it_classes.data;
          std::string scroll_type =
              scroll_class->scroll_type == XCB_INPUT_SCROLL_TYPE_VERTICAL
                  ? "vertical"
                  : "horizontal";
          std::string increment =
              std::to_string(fp3232_to_double(scroll_class->increment));
          if (scroll_class->scroll_type == XCB_INPUT_SCROLL_TYPE_VERTICAL) {
            vertical_scroll.emplace(deviceid, scroll_class->number,
                                    fp3232_to_double(scroll_class->increment),
                                    0.0);
          }
          break;
        }
        }
        xcb_input_device_class_next(&it_classes);
      }

      if (vertical_scroll && vertical_scroll->device_id == deviceid) {
        xcb_input_valuator_class_t *valuator =
            valuator_by_number[vertical_scroll->valuator_number];
        vertical_scroll->last_value = fp3232_to_double(valuator->value);
      }

      xcb_input_xi_device_info_next(&it_device);
    }
  }
}

std::string CreateWindow() {
  int screenp = 0;
  connection = xcb_connect(nullptr, &screenp);
  if (xcb_connection_has_error(connection)) {
    return "Failed to connect to X server.";
  }

  xcb_screen_iterator_t iter =
      xcb_setup_roots_iterator(xcb_get_setup(connection));
  for (int i = 0; i < screenp; ++i) {
    xcb_screen_next(&iter);
  }
  screen = iter.data;

  xcb_window = xcb_generate_id(connection);
  uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t value_list[] = {screen->black_pixel,
                           XCB_EVENT_MASK_EXPOSURE |
                               XCB_EVENT_MASK_STRUCTURE_NOTIFY};

  xcb_create_window(connection, XCB_COPY_FROM_PARENT, xcb_window, screen->root,
                    0, 0, window_width, window_height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                    value_mask, value_list);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcb_window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, sizeof(kWindowName),
                      kWindowName);

  if (auto reply = WRAP(xcb_intern_atom, 0, strlen("WM_DELETE_WINDOW"),
                        "WM_DELETE_WINDOW")) {
    wm_delete_window = reply->atom;
  }
  if (auto reply =
          WRAP(xcb_intern_atom, 0, strlen("WM_PROTOCOLS"), "WM_PROTOCOLS")) {
    wm_protocols = reply->atom;
  }

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcb_window,
                      wm_protocols, 4, 32, 1, &wm_delete_window);

  xcb_map_window(connection, xcb_window);
  xcb_flush(connection);

  const xcb_query_extension_reply_t *xinput_data =
      xcb_get_extension_data(connection, &xcb_input_id);
  if (!xinput_data->present) {
    return "XInput extension not present.";
  }
  xi_opcode = xinput_data->major_opcode;

  if (auto reply = WRAP(xcb_input_xi_query_version, XCB_INPUT_MAJOR_VERSION,
                        XCB_INPUT_MINOR_VERSION)) {
    std::pair<int, int> server_version(reply->major_version,
                                       reply->minor_version);
    std::pair<int, int> required_version(2, 2);
    if (server_version < required_version) {
      return "XInput version 2.2 or higher required for multitouch.";
    }
  } else {
    return "Failed to query XInput version.";
  }

  struct input_event_mask {
    xcb_input_event_mask_t header = {
        .deviceid = XCB_INPUT_DEVICE_ALL_MASTER,
        .mask_len = 1,
    };
    uint32_t mask =
        XCB_INPUT_XI_EVENT_MASK_DEVICE_CHANGED |
        XCB_INPUT_XI_EVENT_MASK_KEY_PRESS |
        XCB_INPUT_XI_EVENT_MASK_KEY_RELEASE |
        XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS |
        XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE |
        XCB_INPUT_XI_EVENT_MASK_MOTION | XCB_INPUT_XI_EVENT_MASK_ENTER |
        XCB_INPUT_XI_EVENT_MASK_LEAVE | XCB_INPUT_XI_EVENT_MASK_FOCUS_IN |
        XCB_INPUT_XI_EVENT_MASK_FOCUS_OUT |
        XCB_INPUT_XI_EVENT_MASK_TOUCH_BEGIN |
        XCB_INPUT_XI_EVENT_MASK_TOUCH_UPDATE |
        XCB_INPUT_XI_EVENT_MASK_TOUCH_END;
  } event_mask;

  xcb_void_cookie_t cookie = xcb_input_xi_select_events_checked(
      connection, xcb_window, 1, &event_mask.header);
  if (std::unique_ptr<xcb_generic_error_t> error{
          xcb_request_check(connection, cookie)}) {
    return f("Failed to select events: %d", error->error_code);
  }

  ScanDevices();

  return "";
}

#undef WRAP

void Paint() {
  SkCanvas &canvas = *vk::GetBackbufferCanvas();
  canvas.save();
  canvas.translate(0, window_height);
  canvas.scale(DisplayPxPerMeter(), -DisplayPxPerMeter());
  if (window) {
    window->Draw(canvas);
  }
  canvas.restore();
  vk::Present();
}

void RenderLoop() {
  bool running = true;
  xcb_generic_event_t *event;

  while (running) {
    event = xcb_poll_for_event(connection);

    if (event) {
      switch (event->response_type & ~0x80) {
      case XCB_EXPOSE: {
        xcb_expose_event_t *ev = (xcb_expose_event_t *)event;
        // ev-count is the number of expose events that are still in the queue.
        // We only want to do a full redraw on the last expose event.
        if (ev->count == 0) {
          Paint();
        }
        break;
      }
      case XCB_MAP_NOTIFY: { // ignore
        xcb_map_notify_event_t *ev = (xcb_map_notify_event_t *)event;
        break;
      }
      case XCB_REPARENT_NOTIFY: { // ignore
        xcb_reparent_notify_event_t *ev = (xcb_reparent_notify_event_t *)event;
        break;
      }
      case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *ev =
            (xcb_configure_notify_event_t *)event;
        if (ev->width != window_width || ev->height != window_height) {
          window_width = ev->width;
          window_height = ev->height;

          if (auto err = vk::Resize(window_width, window_height);
              !err.empty()) {
            ERROR() << err;
          }
          window->Resize(WindowSize());
        }
        window_position_on_screen = Vec2(ev->x, ev->y);
        break;
      }
      case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *cm = (xcb_client_message_event_t *)event;
        if (cm->data.data32[0] == wm_delete_window)
          running = false;
        break;
      }
      case XCB_GE_GENERIC: {
        xcb_ge_generic_event_t *ev = (xcb_ge_generic_event_t *)event;
        if (ev->extension == xi_opcode) {
          switch (ev->event_type) {
          case XCB_INPUT_DEVICE_CHANGED: {
            // This event usually indicates that the slave device has changed.
            // We should update the scroll valua based on the valuator from the
            // current slave.
            xcb_input_device_changed_event_t *ev =
                (xcb_input_device_changed_event_t *)event;
            if (vertical_scroll && ev->deviceid == vertical_scroll->device_id) {
              if (ev->reason == XCB_INPUT_CHANGE_REASON_SLAVE_SWITCH) {
                xcb_input_device_class_iterator_t it =
                    xcb_input_device_changed_classes_iterator(ev);
                while (it.rem) {
                  xcb_input_device_class_t *it_class = it.data;
                  if (it_class->type == XCB_INPUT_DEVICE_CLASS_TYPE_VALUATOR) {
                    xcb_input_valuator_class_t *valuator_class =
                        (xcb_input_valuator_class_t *)it_class;
                    if (valuator_class->number ==
                        vertical_scroll->valuator_number) {
                      vertical_scroll->last_value =
                          fp3232_to_double(valuator_class->value);
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
          case XCB_INPUT_KEY_PRESS: {
            xcb_input_key_press_event_t *ev =
                (xcb_input_key_press_event_t *)event;
            window->KeyDown(X11KeyCodeToKey((KeyCode)ev->detail));
            break;
          }
          case XCB_INPUT_KEY_RELEASE: {
            xcb_input_key_release_event_t *ev =
                (xcb_input_key_release_event_t *)event;
            window->KeyUp(X11KeyCodeToKey((KeyCode)ev->detail));
            break;
          }
          case XCB_INPUT_BUTTON_PRESS: {
            xcb_input_button_press_event_t *ev =
                (xcb_input_button_press_event_t *)event;
            // Ignore emulated mouse wheel "buttons"
            if (ev->flags & XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED) {
              break;
            }
            GetMouse().ButtonDown(EventDetailToButton(ev->detail));
            break;
          }
          case XCB_INPUT_BUTTON_RELEASE: {
            xcb_input_button_release_event_t *ev =
                (xcb_input_button_release_event_t *)event;
            // Ignore emulated mouse wheel "buttons"
            if (ev->flags & XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED) {
              break;
            }
            GetMouse().ButtonUp(EventDetailToButton(ev->detail));
            break;
          }
          case XCB_INPUT_MOTION: {
            xcb_input_motion_event_t *ev = (xcb_input_motion_event_t *)event;

            if (vertical_scroll && ev->deviceid == vertical_scroll->device_id) {
              xcb_input_fp3232_t *axisvalues =
                  xcb_input_button_press_axisvalues(ev);
              int n_axisvalues = xcb_input_button_press_axisvalues_length(ev);
              uint32_t *valuator_mask =
                  xcb_input_button_press_valuator_mask(ev);
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
                      GetMouse().Wheel(-delta / vertical_scroll->increment);
                    }
                    ++i_axis;
                  }
                  mask >>= 1;
                  ++i_valuator;
                }
              }
            }
            mouse_position_on_screen.X = fp1616_to_float(ev->root_x);
            mouse_position_on_screen.Y = fp1616_to_float(ev->root_y);
            GetMouse().Move(ScreenToWindow(mouse_position_on_screen));
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
            xcb_input_enter_event_t *ev = (xcb_input_enter_event_t *)event;
            mouse_position_on_screen.X = fp1616_to_float(ev->root_x);
            mouse_position_on_screen.Y = fp1616_to_float(ev->root_y);
            GetMouse().Move(ScreenToWindow(mouse_position_on_screen));
            break;
          }
          case XCB_INPUT_LEAVE: {
            mouse.reset();
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
            LOG() << "Unknown XI event (event_type=" << ev->event_type << ")";
            break;
          }
          }
        } else {
          LOG() << "Unknown XCB_GE_GENERIC event (extension=" << ev->extension
                << ", event_type=" << ev->event_type << ")";
        }
        break;
      }
      default:
        LOG() << "Unhandled event: " << event->response_type;
        break;
      }
    } else { // event == nullptr
      Paint();
    }

    free(event);
  }

  xcb_destroy_window(connection, xcb_window);
}

int LinuxMain(int argc, char *argv[]) {
  SkGraphics::Init();
  InitRoot();

  if (auto err = CreateWindow(); !err.empty()) {
    FATAL() << "Failed to create window: " << err;
  }

  if (auto err = vk::Init(); !err.empty()) {
    FATAL() << "Failed to initialize Vulkan: " << err;
  }

  window.reset(new gui::Window(WindowSize(), DisplayPxPerMeter()));

  RenderLoop();

  vk::Destroy();

  return 0;
}
