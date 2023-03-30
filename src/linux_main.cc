#include "linux_main.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>

#include "format.h"
#include "log.h"
#include "vk.h"

#pragma comment(lib, "xcb")
#pragma comment(lib, "xcb-xinput")

using namespace automaton;

xcb_connection_t *connection;
xcb_window_t window;
xcb_screen_t *screen;
xcb_atom_t wm_protocols;
xcb_atom_t wm_delete_window;
int window_width = 1280;
int window_height = 720;
uint8_t xi_opcode;

float DisplayPxPerMeter() {
  return 1000.0f * screen->width_in_pixels / screen->width_in_millimeters;
}

vec2 WindowSize() {
  return Vec2(window_width, window_height) / DisplayPxPerMeter();
}

const char kWindowName[] = "Automat";

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

  window = xcb_generate_id(connection);
  uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t value_list[] = {
      screen->black_pixel,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};

  xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0,
                    0, window_width, window_height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                    value_mask, value_list);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, sizeof(kWindowName),
                      kWindowName);

#define WRAP(f, ...)                                                           \
  std::unique_ptr<f##_reply_t>(                                                \
      f##_reply(connection, f(connection, __VA_ARGS__), nullptr))

  if (auto reply = WRAP(xcb_intern_atom, 0, strlen("WM_DELETE_WINDOW"),
                        "WM_DELETE_WINDOW")) {
    wm_delete_window = reply->atom;
  }
  if (auto reply =
          WRAP(xcb_intern_atom, 0, strlen("WM_PROTOCOLS"), "WM_PROTOCOLS")) {
    wm_protocols = reply->atom;
  }

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, wm_protocols, 4,
                      32, 1, &wm_delete_window);

  xcb_map_window(connection, window);
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
      connection, window, 1, &event_mask.header);
  if (std::unique_ptr<xcb_generic_error_t> error{
          xcb_request_check(connection, cookie)}) {
    return f("Failed to select events: %d", error->error_code);
  }

#undef WRAP

  return "";
}

void RenderLoop() {
  bool running = true;
  xcb_generic_event_t *event;
  xcb_client_message_event_t *cm;

  while (running) {
    event = xcb_wait_for_event(connection);

    switch (event->response_type & ~0x80) {
    case XCB_EXPOSE: {
      xcb_expose_event_t *ev = (xcb_expose_event_t *)event;
      // ev-count is the number of expose events that are still in the queue.
      // We only want to do a full redraw on the last expose event.
      if (ev->count == 0) {
        SkCanvas *canvas = vk::GetBackbufferCanvas();
        canvas->clear(SK_ColorWHITE);
        vk::Present();
      }
      break;
    }
    case XCB_MAP_NOTIFY: {
      xcb_map_notify_event_t *ev = (xcb_map_notify_event_t *)event;
      LOG() << "Map notify: " << ev->window;
      break;
    }
    case XCB_REPARENT_NOTIFY: {
      xcb_reparent_notify_event_t *ev = (xcb_reparent_notify_event_t *)event;
      LOG() << "Reparent notify: " << ev->window;
      break;
    }
    case XCB_CONFIGURE_NOTIFY: {
      xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t *)event;
      if (ev->width != window_width || ev->height != window_height) {
        window_width = ev->width;
        window_height = ev->height;
        
        if (auto err = vk::Resize(window_width, window_height); !err.empty()) {
          ERROR() << err;
        }
      }
      break;
    }
    case XCB_CLIENT_MESSAGE: {
      cm = (xcb_client_message_event_t *)event;

      if (cm->data.data32[0] == wm_delete_window)
        running = false;

      break;
    }
    case XCB_GE_GENERIC: {
      xcb_ge_generic_event_t *ev = (xcb_ge_generic_event_t *)event;
      if (ev->extension == xi_opcode) {
        switch (ev->event_type) {
        case XCB_INPUT_KEY_PRESS: {
          xcb_input_key_press_event_t *ev = (xcb_input_key_press_event_t *)event;
          LOG() << "XCB_INPUT_KEY_PRESS " << ev->detail;
          break;
        }
        case XCB_INPUT_KEY_RELEASE: {
          LOG() << "XCB_INPUT_KEY_RELEASE";
          break;
        }
        case XCB_INPUT_BUTTON_PRESS: {
          LOG() << "XCB_INPUT_BUTTON_PRESS";
          break;
        }
        case XCB_INPUT_BUTTON_RELEASE: {
          LOG() << "XCB_INPUT_BUTTON_RELEASE";
          break;
        }
        case XCB_INPUT_MOTION: {
          LOG_EVERY_N_SEC(1) << "XCB_INPUT_MOTION";
          break;
        }
        case XCB_INPUT_ENTER: {
          LOG() << "XCB_INPUT_ENTER";
          break;
        }
        case XCB_INPUT_LEAVE: {
          LOG() << "XCB_INPUT_LEAVE";
          break;
        }
        case XCB_INPUT_FOCUS_IN: {
          LOG() << "XCB_INPUT_FOCUS_IN";
          break;
        }
        case XCB_INPUT_FOCUS_OUT: {
          LOG() << "XCB_INPUT_FOCUS_OUT";
          break;
        }
        case XCB_INPUT_TOUCH_BEGIN: {
          LOG() << "XCB_INPUT_TOUCH_BEGIN";
          break;
        }
        case XCB_INPUT_TOUCH_UPDATE: {
          LOG() << "XCB_INPUT_TOUCH_UPDATE";
          break;
        }
        case XCB_INPUT_TOUCH_END: {
          LOG() << "XCB_INPUT_TOUCH_END";
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

    free(event);
  }

  xcb_destroy_window(connection, window);
}

int LinuxMain(int argc, char *argv[]) {
  if (auto err = CreateWindow(); !err.empty()) {
    FATAL() << "Failed to create window: " << err;
  }

  if (auto err = vk::Init(); !err.empty()) {
    FATAL() << "Failed to initialize Vulkan: " << err;
  }

  RenderLoop();

  vk::Destroy();

  return 0;
}
