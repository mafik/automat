#include "linux_main.hh"

#include <include/core/SkGraphics.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "format.hh"
#include "keyboard.hh"
#include "library.hh"  // IWYU pragma: keep
#include "log.hh"
#include "root.hh"
#include "vk.hh"
#include "window.hh"

#pragma comment(lib, "vk-bootstrap")
#pragma comment(lib, "xcb")
#pragma comment(lib, "xcb-xinput")

// See http://who-t.blogspot.com/search/label/xi2 for XInput2 documentation.

using namespace automat;

xcb_connection_t* connection;
xcb_window_t xcb_window;
xcb_screen_t* screen;
xcb_atom_t wm_protocols;
xcb_atom_t wm_delete_window;
int window_width = 1280;
int window_height = 720;
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

std::unique_ptr<gui::Window> window;
std::unique_ptr<gui::Pointer> mouse;

float DisplayPxPerMeter() {
  return 1000.0f * screen->width_in_pixels / screen->width_in_millimeters;
}

Vec2 WindowSize() { return Vec2(window_width, window_height) / DisplayPxPerMeter(); }

// "Screen" coordinates are in pixels and their origin is in the upper left
// corner. "Window" coordinates are in meters and their origin is in the bottom
// left window corner.

namespace automat::gui {
Vec2 ScreenToWindow(Vec2 screen) {
  Vec2 window = (screen - window_position_on_screen - Vec2(0, window_height)) / DisplayPxPerMeter();
  window.y = -window.y;
  return window;
}

Vec2 WindowToScreen(Vec2 window) {
  window.y = -window.y;
  return window * DisplayPxPerMeter() + window_position_on_screen + Vec2(0, window_height);
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
double fp3232_to_double(xcb_input_fp3232_t fp) { return fp.integral + fp.frac / 4294967296.0; }

#define WRAP(f, ...)                             \
  std::unique_ptr<f##_reply_t, void (*)(void*)>( \
      f##_reply(connection, f(connection, __VA_ARGS__), nullptr), free)

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

std::string CreateWindow() {
  int screenp = 0;
  connection = xcb_connect(nullptr, &screenp);
  if (xcb_connection_has_error(connection)) {
    return "Failed to connect to X server.";
  }

  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(connection));
  for (int i = 0; i < screenp; ++i) {
    xcb_screen_next(&iter);
  }
  screen = iter.data;

  xcb_window = xcb_generate_id(connection);
  uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t value_list[] = {screen->black_pixel,
                           XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};

  xcb_create_window(connection, XCB_COPY_FROM_PARENT, xcb_window, screen->root, 0, 0, window_width,
                    window_height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                    value_mask, value_list);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcb_window, XCB_ATOM_WM_NAME,
                      XCB_ATOM_STRING, 8, sizeof(kWindowName), kWindowName);

  if (auto reply = WRAP(xcb_intern_atom, 0, strlen("WM_DELETE_WINDOW"), "WM_DELETE_WINDOW")) {
    wm_delete_window = reply->atom;
  }
  if (auto reply = WRAP(xcb_intern_atom, 0, strlen("WM_PROTOCOLS"), "WM_PROTOCOLS")) {
    wm_protocols = reply->atom;
  }

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcb_window, wm_protocols, 4, 32, 1,
                      &wm_delete_window);

  xcb_map_window(connection, xcb_window);
  xcb_flush(connection);

  const xcb_query_extension_reply_t* xinput_data =
      xcb_get_extension_data(connection, &xcb_input_id);
  if (!xinput_data->present) {
    return "XInput extension not present.";
  }
  xi_opcode = xinput_data->major_opcode;

  if (auto reply =
          WRAP(xcb_input_xi_query_version, XCB_INPUT_MAJOR_VERSION, XCB_INPUT_MINOR_VERSION)) {
    std::pair<int, int> server_version(reply->major_version, reply->minor_version);
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
    return f("Failed to select events: %d", error->error_code);
  }

  ScanDevices();

  return "";
}

#undef WRAP

void Paint() {
  SkCanvas& canvas = *vk::GetBackbufferCanvas();
  canvas.save();
  canvas.translate(0, 0);
  canvas.scale(DisplayPxPerMeter(), DisplayPxPerMeter());
  if (window) {
    window->Draw(canvas);
  }
  canvas.restore();
  vk::Present();
}

void RenderLoop() {
  std::atomic_bool running = true;
  xcb_generic_event_t* event;

  std::stop_callback on_automat_stop(automat_thread.get_stop_token(), [&] { running = false; });

  while (running) {
    event = xcb_poll_for_event(connection);

    if (event) {
      for (auto hook : system_event_hooks) {
        if (hook->Intercept(event)) {
          goto intercepted;
        }
      }

      switch (event->response_type & ~0x80) {
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
          if (ev->width != window_width || ev->height != window_height) {
            window_width = ev->width;
            window_height = ev->height;

            if (auto err = vk::Resize(window_width, window_height); !err.empty()) {
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
          break;
        }
        case XCB_CLIENT_MESSAGE: {
          xcb_client_message_event_t* cm = (xcb_client_message_event_t*)event;
          if (cm->data.data32[0] == wm_delete_window) running = false;
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

                RunOnAutomatThread([=] { GetMouse().ButtonDown(EventDetailToButton(ev->detail)); });
                break;
              }
              case XCB_INPUT_BUTTON_RELEASE: {
                xcb_input_button_release_event_t* ev = (xcb_input_button_release_event_t*)event;
                // Ignore emulated mouse wheel "buttons"
                if (ev->flags & XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED) {
                  break;
                }
                RunOnAutomatThread([=] { GetMouse().ButtonUp(EventDetailToButton(ev->detail)); });
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

int LinuxMain(int argc, char* argv[]) {
  SkGraphics::Init();
  InitRoot();

  if (auto err = CreateWindow(); !err.empty()) {
    FATAL << "Failed to create window: " << err;
  }

  if (auto err = vk::Init(); !err.empty()) {
    FATAL << "Failed to initialize Vulkan: " << err;
  }

  window.reset(new gui::Window(WindowSize(), DisplayPxPerMeter()));
  gui::keyboard = std::make_unique<gui::Keyboard>(*window);

  RenderLoop();

  mouse.reset();

  vk::Destroy();
  xcb_destroy_window(connection, xcb_window);

  window.reset();

  LOG << "Exiting.";

  return 0;
}
