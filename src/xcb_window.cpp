// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "xcb_window.hpp"

#include <xcb/xinput.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <atomic>
#include <bit>
#include <stop_token>

#include "board.hpp"
#include "drag_action.hpp"
#include "engine.hpp"
#include "file_import.hpp"
#include "fn.hpp"
#include "format.hpp"
#include "library_data_offer.hpp"
#include "library_file.hpp"
#include "location.hpp"
#include "log.hpp"
#include "root_widget.hpp"
#include "vec.hpp"
#include "x11_keys.hpp"
#include "xcb.hpp"

using namespace automat;

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
    auto reply = xcb::get_property(xcb_window, _NET_WM_STATE, XCB_ATOM_ANY, 0, 32);
    if (reply) {
      xcb_atom_t* atoms = (xcb_atom_t*)xcb_get_property_value(reply.get());
      int n_atoms = xcb_get_property_value_length(reply.get()) / sizeof(xcb_atom_t);
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

  Str ToStr() const {
    return f(
        "WM_STATE: MODAL=%b STICKY=%b MAXIMIZED_VERT=%b MAXIMIZED_HORZ=%b SHADED=%b "
        "SKIP_TASKBAR=%b SKIP_PAGER=%b HIDDEN=%b FULLSCREEN=%b ABOVE=%b BELOW=%b "
        "DEMANDS_ATTENTION=%b",
        MODAL, STICKY, MAXIMIZED_VERT, MAXIMIZED_HORZ, SHADED, SKIP_TASKBAR, SKIP_PAGER, HIDDEN,
        FULLSCREEN, ABOVE, BELOW, DEMANDS_ATTENTION);
  }
};

static void SendToDragSource(xcb_window_t source, xcb_atom_t message, const uint32_t data[5]) {
  xcb_client_message_event_t event = {
      .response_type = XCB_CLIENT_MESSAGE,
      .format = 32,
      .window = source,
      .type = message,
  };
  memcpy(event.data.data32, data, sizeof(uint32_t) * 5);
  xcb_send_event(connection, false, source, XCB_EVENT_MASK_NO_EVENT, (const char*)&event);
  flush();
}

constexpr bool kDebugDragAndDrop = true;

static Str JoinAtomNames(const xcb_atom_t* atoms, size_t count) {
  Str result;
  for (size_t i = 0; i < count; ++i) {
    if (i) result += ", ";
    result += atom::ToStr(atoms[i]);
  }
  return result;
}

static Str TakeDragData(xcb_window_t window, xcb_atom_t property, xcb_atom_t* out_type) {
  auto cookie = xcb_get_property(connection, true, window, property, XCB_GET_PROPERTY_TYPE_ANY, 0,
                                 UINT32_MAX / 4);
  std::unique_ptr<xcb_get_property_reply_t, DeleteWithFree> reply(
      xcb_get_property_reply(connection, cookie, nullptr));
  if (!reply) {
    *out_type = XCB_NONE;
    return "";
  }
  *out_type = reply->type;
  return Str((char*)xcb_get_property_value(reply.get()),
             (size_t)xcb_get_property_value_length(reply.get()));
}

float fp1616_to_float(xcb_input_fp1616_t fp) { return fp / 65536.0f; }
double fp3232_to_double(xcb_input_fp3232_t fp) { return fp.integral + fp.frac / 4294967296.0; }

struct XcbDevice {
  struct VerticalScroll {
    uint16_t valuator_number;
    double increment;
    double last_value;
  };

  std::optional<VerticalScroll> vertical_scroll;

  struct Valuator {
    uint16_t number;
  };

  std::optional<Valuator> relative_x;
  std::optional<Valuator> relative_y;
};

std::map<xcb_input_device_id_t, XcbDevice> devices;

static void ScanDevices(XCBWindow& window) {
  devices.clear();
  if (auto reply = xcb::input_xi_query_device(XCB_INPUT_DEVICE_ALL_MASTER)) {
    int n_devices = xcb_input_xi_query_device_infos_length(reply.get());
    auto it_device = xcb_input_xi_query_device_infos_iterator(reply.get());
    for (int i_device = 0; i_device < n_devices; ++i_device) {
      xcb_input_device_id_t deviceid = it_device.data->deviceid;
      XcbDevice& device = devices[deviceid];
      if (it_device.data->type == XCB_INPUT_DEVICE_TYPE_MASTER_POINTER) {
        window.master_pointer_device_id = deviceid;
      } else if (it_device.data->type == XCB_INPUT_DEVICE_TYPE_MASTER_KEYBOARD) {
        window.master_keyboard_device_id = deviceid;
      }
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
            auto label = atom::ToStr(valuator_class->label);
            if (label == "Rel X") {
              device.relative_x.emplace(XcbDevice::Valuator{.number = valuator_class->number});
            } else if (label == "Rel Y") {
              device.relative_y.emplace(XcbDevice::Valuator{.number = valuator_class->number});
            }
            // LOG << "Valuator " << dump_struct(*valuator_class);
            // LOG << "Label: " << label;
            break;
          }
          case XCB_INPUT_DEVICE_CLASS_TYPE_SCROLL: {
            xcb_input_scroll_class_t* scroll_class = (xcb_input_scroll_class_t*)it_classes.data;
            std::string scroll_type = scroll_class->scroll_type == XCB_INPUT_SCROLL_TYPE_VERTICAL
                                          ? "vertical"
                                          : "horizontal";
            std::string increment = std::to_string(fp3232_to_double(scroll_class->increment));
            if (scroll_class->scroll_type == XCB_INPUT_SCROLL_TYPE_VERTICAL) {
              device.vertical_scroll.emplace(scroll_class->number,
                                             fp3232_to_double(scroll_class->increment), 0.0);
            }
            break;
          }
        }
        xcb_input_device_class_next(&it_classes);
      }

      if (device.vertical_scroll) {
        xcb_input_valuator_class_t* valuator =
            valuator_by_number[device.vertical_scroll->valuator_number];
        device.vertical_scroll->last_value = fp3232_to_double(valuator->value);
      }

      xcb_input_xi_device_info_next(&it_device);
    }
  }
}

std::unique_ptr<automat::ui::Window> XCBWindow::Make(automat::ui::RootWidget& root,
                                                     Status& status) {
  xcb::Connect(status);
  if (!OK(status)) {
    return nullptr;
  }
  auto window = std::unique_ptr<XCBWindow>(new XCBWindow(root));

  if (xcb_cursor_context_new(connection, screen, std::out_ptr(window->cursor_context)) < 0) {
    ERROR << "Error: Failed to create cursor context";
  }

  float pixels_per_meter = DisplayPxPerMeter();
  window->client_width = root.size.x * pixels_per_meter;
  window->client_height = root.size.y * pixels_per_meter;

  window->xcb_window = xcb_generate_id(connection);
  uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t value_list[] = {screen->white_pixel, XCB_EVENT_MASK_EXPOSURE |
                                                    XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                                                    XCB_EVENT_MASK_PROPERTY_CHANGE};

  {
    xcb_void_cookie_t cookie = xcb_create_window(
        connection, XCB_COPY_FROM_PARENT, window->xcb_window, screen->root, 0, 0,
        window->client_width, window->client_height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual, value_mask, value_list);
    if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(connection, cookie)}) {
      AppendErrorMessage(status) += f("Failed to create window: {}", error->error_code);
      return nullptr;
    }
  }

  WM_STATE wm_state = WM_STATE::Get(window->xcb_window);
  wm_state.MAXIMIZED_HORZ = root.maximized_horizontally;
  wm_state.MAXIMIZED_VERT = root.maximized_vertically;
  wm_state.ABOVE = root.always_on_top;
  wm_state.Set(window->xcb_window);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window->xcb_window, XCB_ATOM_WM_NAME,
                      XCB_ATOM_STRING, 8, sizeof(automat::ui::kWindowName),
                      automat::ui::kWindowName);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window->xcb_window, atom::WM_PROTOCOLS,
                      XCB_ATOM_ATOM, 32, 1, &atom::WM_DELETE_WINDOW);

  ReplaceProperty32(window->xcb_window, atom::XdndAware, XCB_ATOM_ATOM, 5);

  // Setting user time to 0 indicates that the window wasn't created as a result of a user action
  // and prevents window activation.
  ReplaceProperty32(window->xcb_window, atom::_NET_WM_USER_TIME, XCB_ATOM_CARDINAL, 0);

  xcb_map_window(connection, window->xcb_window);

  if (!isnan(root.output_device_x) && !root.maximized_horizontally) {
    uint32_t x = 0;
    if (root.output_device_x >= 0) {
      x = roundf(root.output_device_x * DisplayPxPerMeter());
    } else {
      x = roundf(screen->width_in_pixels + root.output_device_x * DisplayPxPerMeter() -
                 window->client_width);
    }
    xcb_configure_window(connection, window->xcb_window, XCB_CONFIG_WINDOW_X, &x);
  }
  if (!isnan(root.output_device_y) && !root.maximized_vertically) {
    uint32_t y = 0;
    if (root.output_device_y >= 0) {
      y = roundf(root.output_device_y * DisplayPxPerMeter());
    } else {
      y = roundf(screen->height_in_pixels + root.output_device_y * DisplayPxPerMeter() -
                 window->client_height);
    }
    xcb_configure_window(connection, window->xcb_window, XCB_CONFIG_WINDOW_Y, &y);
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
      xcb_input_xi_select_events_checked(connection, window->xcb_window, 1, &event_mask.header);
  if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(connection, cookie)}) {
    AppendErrorMessage(status) += f("Failed to select events: {}", error->error_code);
    return nullptr;
  }

  ScanDevices(*window);

  root.DisplayPixelDensity(DisplayPxPerMeter());

  return window;
}

void XCBWindow::RequestResize(Vec2 new_size) {
  const static uint32_t values[] = {
      static_cast<uint32_t>(new_size.x * root.display_pixels_per_meter),
      static_cast<uint32_t>(new_size.y * root.display_pixels_per_meter)};
  xcb_configure_window(connection, xcb_window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                       values);
  xcb_flush(connection);
  root.Resized(new_size);
}

void XCBWindow::RequestMaximize(bool horizontally, bool vertically) {
  WM_STATE wm_state = WM_STATE::Get(xcb_window);
  wm_state.MAXIMIZED_HORZ = horizontally;
  wm_state.MAXIMIZED_VERT = vertically;
  wm_state.Set(xcb_window);
  root.Maximized(horizontally, vertically);
}

void XCBWindow::RequestMinimizeToTray() {
  xcb_unmap_window(connection, xcb_window);
  xcb_flush(connection);
}

void XCBWindow::RequestRestoreFromTray() {
  xcb_map_window(connection, xcb_window);
  xcb_flush(connection);
}

void XCBWindow::OnRegisterInput(bool keylogging, bool pointerlogging) {
  struct input_event_mask {
    xcb_input_event_mask_t header = {
        .deviceid = XCB_INPUT_DEVICE_ALL_MASTER,
        .mask_len = 1,
    };
    uint32_t mask = 0;
  } event_mask;

  if (keylogging) {
    event_mask.mask |=
        XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE;
  }

  if (pointerlogging) {
    event_mask.mask |= XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS |
                       XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE |
                       XCB_INPUT_XI_EVENT_MASK_RAW_MOTION;
  }

  xcb_void_cookie_t cookie =
      xcb_input_xi_select_events_checked(xcb::connection, xcb::screen->root, 1, &event_mask.header);

  if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(xcb::connection, cookie)}) {
    ERROR << f("Couldn't select X11 events for keylogging: {}", error->error_code);
  }
}

XCBWindow::~XCBWindow() {
  if (xcb_window) {
    xcb_destroy_window(xcb::connection, xcb_window);
    xcb_window = 0;
  }
}

void XCBWindow::OnWindowWatchingChanged() {
  bool need_watch = !window_watchings.empty();
  if (need_watch == watching_root_property) return;
  watching_root_property = need_watch;

  // Get current event mask on root window, then add/remove PROPERTY_CHANGE
  auto attrs = xcb::get_window_attributes(screen->root);
  uint32_t current_mask = attrs ? attrs->your_event_mask : 0;
  uint32_t new_mask;
  if (need_watch) {
    new_mask = current_mask | XCB_EVENT_MASK_PROPERTY_CHANGE;
  } else {
    new_mask = current_mask & ~XCB_EVENT_MASK_PROPERTY_CHANGE;
  }
  uint32_t values[] = {new_mask};
  xcb_change_window_attributes(connection, screen->root, XCB_CW_EVENT_MASK, values);
  xcb_flush(connection);
}

struct XCBPointerGrab : automat::ui::PointerGrab {
  XCBWindow& xcb_window;
  XCBPointerGrab(automat::ui::Pointer& pointer, automat::ui::PointerGrabber& grabber,
                 XCBWindow& xcb_window)
      : automat::ui::PointerGrab(pointer, grabber), xcb_window(xcb_window) {
    xcb_cursor_t cursor = xcb_cursor_load_cursor(xcb_window.cursor_context.get(), "crosshair");

    uint32_t mask = XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE |
                    XCB_INPUT_XI_EVENT_MASK_MOTION;
    auto cookie = xcb_input_xi_grab_device(
        connection, screen->root, XCB_CURRENT_TIME, cursor, xcb_window.master_pointer_device_id,
        XCB_INPUT_GRAB_MODE_22_ASYNC, XCB_INPUT_GRAB_MODE_22_ASYNC, false, 1, &mask);

    std::unique_ptr<xcb_generic_error_t, automat::DeleteWithFree> error;

    std::unique_ptr<xcb_input_xi_grab_device_reply_t, automat::DeleteWithFree> reply(
        xcb_input_xi_grab_device_reply(connection, cookie, std::out_ptr(error)));
    if (reply) {
      if (reply->status != XCB_GRAB_STATUS_SUCCESS) {
        ERROR << "Failed to grab the pointer: " << reply->status;
      }
    }

    if (error) {
      ERROR << "Error while attempting to grab pointer: " << dump_struct(*error);
    }

    if (cursor != XCB_NONE) {
      xcb_free_cursor(connection, cursor);
    }
  }
  ~XCBPointerGrab() {
    xcb_void_cookie_t cookie = xcb_input_xi_ungrab_device(connection, XCB_CURRENT_TIME,
                                                          xcb_window.master_pointer_device_id);
    if (std::unique_ptr<xcb_generic_error_t, automat::DeleteWithFree> error{
            xcb_request_check(connection, cookie)}) {
      ERROR << "Failed to ungrab the pointer";
    }
  }
};

struct XCBPointer : automat::ui::Pointer {
  XCBWindow& xcb_window;

  static const char* GetCursorName(automat::ui::Cursor icon) {
    switch (icon) {
      case automat::ui::Cursor::Arrow:
        return "left_ptr";
      case automat::ui::Cursor::Hand:
        return "hand1";
      case automat::ui::Cursor::IBeam:
        return "xterm";
      case automat::ui::Cursor::AllScroll:
        return "all-scroll";
      case automat::ui::Cursor::ResizeHorizontal:
        return "size_hor";
      case automat::ui::Cursor::ResizeVertical:
        return "size_ver";
      case automat::ui::Cursor::Crosshair:
        return "crosshair";
      default:
        return "left_ptr";
    }
  }

  XCBPointer(automat::ui::RootWidget& root, Vec2 position, XCBWindow& xcb_window)
      : automat::ui::Pointer(root, position), xcb_window(xcb_window) {}

  void OnCursorChanged(automat::ui::Cursor old_cursor, automat::ui::Cursor new_cursor) override {
    UpdateCursor(new_cursor);
  }

  void UpdateCursor(automat::ui::Cursor icon) {
    xcb_cursor_t cursor =
        xcb_cursor_load_cursor(xcb_window.cursor_context.get(), GetCursorName(icon));
    if (cursor != XCB_NONE) {
      uint32_t cursor_value = cursor;
      xcb_change_window_attributes(connection, xcb_window.xcb_window, XCB_CW_CURSOR, &cursor_value);
      xcb_free_cursor(connection, cursor);
      xcb_flush(connection);
    }
  }

  automat::ui::PointerGrab& RequestGlobalGrab(automat::ui::PointerGrabber& grabber) override {
    grab.reset(new XCBPointerGrab(*this, grabber, xcb_window));
    return *grab;
  }
};

std::unique_ptr<automat::ui::Pointer> XCBWindow::MakeMouse() {
  return std::make_unique<XCBPointer>(root, ScreenToWindowPx(mouse_position_on_screen), *this);
}

Vec2 XCBWindow::ScreenToWindowPx(Vec2 screen) { return screen - window_position_on_screen; }

Vec2 XCBWindow::WindowPxToScreen(Vec2 window) { return window + window_position_on_screen; }

static automat::ui::PointerButton EventDetailToButton(uint32_t detail) {
  switch (detail) {
    case 1:
      return automat::ui::PointerButton::Left;
    case 2:
      return automat::ui::PointerButton::Middle;
    case 3:
      return automat::ui::PointerButton::Right;
    case 8:
      return automat::ui::PointerButton::Back;
    case 9:
      return automat::ui::PointerButton::Forward;
    default:
      ERROR_ONCE << "Unknown pointer button detail: " << detail;
      return automat::ui::PointerButton::Unknown;
  }
}

template <typename EventT>
struct Valuators {
  using MaskFn = uint32_t* (*)(const EventT*);
  using MaskLengthFn = int (*)(const EventT*);
  using AxisValuesFn = xcb_input_fp3232_t* (*)(const EventT*);

  const EventT& ev;
  uint32_t* mask;
  int mask_len;
  xcb_input_fp3232_t* axisvalues;

  Valuators(const EventT& ev, MaskFn mask_fn, MaskLengthFn mask_length_fn,
            AxisValuesFn axisvalues_fn)
      : ev(ev) {
    mask = mask_fn(&ev);
    mask_len = mask_length_fn(&ev);
    axisvalues = axisvalues_fn(&ev);
  }

  Valuators(const EventT& ev);

  bool HasValuator(int number) {
    int i = number / 32;
    int rem = number % 32;
    if (i >= mask_len) {
      return false;
    }
    return mask[i] & (1 << rem);
  }

  double GetValuator(int number) {
    int i = number / 32;
    int rem = number % 32;
    if (i >= mask_len) {
      return 0;
    }
    // Each event may report many valuators. Here we count how many valuators have to be skipped.
    int valuator_idx = 0;
    for (int k = 0; k < i; ++k) {
      valuator_idx += std::popcount(mask[k]);
    }
    valuator_idx += std::popcount(mask[i] & ((1 << rem) - 1));
    return fp3232_to_double(axisvalues[valuator_idx]);
  }

  XcbDevice* GetDevice() {
    auto device_it = devices.find(ev.deviceid);
    if (device_it == devices.end()) {
      ERROR_ONCE << "XInput device " << ev.deviceid
                 << " not found. Plug & play support is not implemented yet.";
      return nullptr;
    }
    return &device_it->second;
  }

  // Some events contain relative valuse, other contain absolute. Set the `absolute` parameter
  // appropriately.
  Optional<double> GetVerticalScrollDelta(bool absolute) {
    auto device = GetDevice();
    if (!device) {
      return std::nullopt;
    }
    if (!device->vertical_scroll.has_value()) {
      return std::nullopt;
    }
    XcbDevice::VerticalScroll& vertical_scroll = device->vertical_scroll.value();
    if (!HasValuator(vertical_scroll.valuator_number)) {
      return std::nullopt;
    }
    double delta;
    if (absolute) {
      double new_value = GetValuator(vertical_scroll.valuator_number);
      delta = new_value - vertical_scroll.last_value;
      vertical_scroll.last_value = new_value;
      if (abs(delta) > 1000000) {
        // http://who-t.blogspot.com/2012/06/xi-21-protocol-design-issues.html
        delta = (delta > 0 ? 1 : -1) * vertical_scroll.increment;
      }
    } else {
      delta = GetValuator(vertical_scroll.valuator_number);
    }
    return -delta / vertical_scroll.increment;
  }

  Optional<Vec2> GetRelativeXY() {
    auto device = GetDevice();
    if (!device) {
      return std::nullopt;
    }
    if (!device->relative_x.has_value() && !device->relative_y.has_value()) {
      return std::nullopt;
    }
    auto x_valuator_number = device->relative_x->number;
    auto y_valuator_number = device->relative_y->number;
    auto has_x = HasValuator(x_valuator_number);
    auto has_y = HasValuator(y_valuator_number);
    if (!has_x && !has_y) {
      return std::nullopt;
    }
    if (has_x && has_y) {
      return Vec2(GetValuator(x_valuator_number), GetValuator(y_valuator_number));
    } else if (has_x) {
      return Vec2(GetValuator(x_valuator_number), 0);
    } else {
      return Vec2(0, GetValuator(y_valuator_number));
    }
  }
};

static Valuators<xcb_input_raw_button_press_event_t> RawButtonValuators(
    const xcb_input_raw_button_press_event_t& ev) {
  return Valuators<xcb_input_raw_button_press_event_t>(
      ev, xcb_input_raw_button_press_valuator_mask, xcb_input_raw_button_press_valuator_mask_length,
      xcb_input_raw_button_press_axisvalues);
}

static Valuators<xcb_input_button_press_event_t> ButtonValuators(
    const xcb_input_button_press_event_t& ev) {
  return Valuators<xcb_input_button_press_event_t>(ev, xcb_input_button_press_valuator_mask,
                                                   xcb_input_button_press_valuator_mask_length,
                                                   xcb_input_button_press_axisvalues);
}

void XCBWindow::MainLoop(std::stop_token stop_token) {
  std::atomic_bool running = true;
  // TODO: maybe use unique_ptr here
  xcb_generic_event_t *event, *peeked_event = nullptr;
  bool keys_down[256] = {0};

  std::stop_callback on_automat_stop(stop_token, [&] {
    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = xcb_window;
    ev.data.data32[0] = atom::WM_DELETE_WINDOW;
    // ev.type = atom::WM_DELETE_WINDOW;
    ev.format = 32;
    // optionally stash a small tag in ev.data.data32[0..4]
    xcb_send_event(connection, false, xcb_window, XCB_EVENT_MASK_NO_EVENT, (const char*)&ev);
    xcb_flush(connection);
  });

  using library::DataOffer;
  using library::File;

  Ptr<DataOffer> current_offer;
  Ptr<Location> offer_loc;
  Vec<WeakPtr<DataOffer>> live_offers;  // every offer still transferring
  xcb_atom_t uri_probe_property = XCB_NONE;
  WeakPtr<DataOffer> uri_probe_offer;

  auto Drag = [&]() -> DragLocationAction* {
    ui::Pointer* mouse = MouseOrNull();
    if (!mouse) return nullptr;
    return dynamic_cast<DragLocationAction*>(
        mouse->actions[static_cast<int>(ui::PointerButton::Left)].get());
  };

  SmallVec<xcb_atom_t, 8> free_transfer_properties;
  int transfer_property_counter = 0;

  auto AcquireTransferProperty = [&] {
    if (!free_transfer_properties.empty()) {
      return free_transfer_properties.pop_back_val();
    }
    Str name = f("AUTOMAT_TRANSFER_{}", transfer_property_counter++);
    auto cookie = xcb_intern_atom(connection, false, name.size(), name.data());
    std::unique_ptr<xcb_intern_atom_reply_t, DeleteWithFree> reply(
        xcb_intern_atom_reply(connection, cookie, nullptr));
    return reply ? reply->atom : (xcb_atom_t)XCB_NONE;
  };

  auto Basename = [](StrView s) {
    auto slash = s.find_last_of('/');
    return Str(slash == StrView::npos ? s : s.substr(slash + 1));
  };

  struct Found {
    Ptr<DataOffer> offer;
    DataOffer::Entry* entry;
  };
  auto FindEntry = [&](xcb_atom_t property, xcb_timestamp_t time, xcb_atom_t target) -> Found {
    for (auto& w : live_offers) {
      auto offer = w.Lock();
      if (!offer) continue;
      int n = offer->Count();
      for (int i = 0; i < n; ++i) {
        auto& e = offer->entries[i];
        if (e.done.load(std::memory_order_acquire)) continue;
        bool match =
            property != XCB_NONE ? (e.property == property) : (e.time == time && e.type == target);
        if (match) return {offer, &e};
      }
    }
    return {nullptr, nullptr};
  };

  auto CheckDone = [&](DataOffer& offer) {
    if (!offer.Done()) return;
    {
      auto lock = Lock();
      if (offer_loc && &offer == current_offer.get()) {
        if (auto* drag = Drag()) drag->RemoveFromGroup(*offer_loc);
        offer_loc.reset();
      } else if (auto L = offer.HomeLocation()) {
        if (auto board = L->LockBoard()) board->Extract(*L);
      }
    }
    std::erase_if(live_offers, [&](WeakPtr<DataOffer>& w) {
      auto p = w.Lock();
      return !p || p.get() == &offer;
    });
    if (!offer.dropped) return;
    int n = offer.Count();
    bool accepted = false;
    for (int i = 0; i < n; ++i) {
      accepted |= !offer.entries[i].failed.load(std::memory_order_relaxed);
    }
    if (accepted && offer.action == atom::XdndActionMove) {
      for (int i = 0; i < n; ++i) {
        auto& e = offer.entries[i];
        if (e.failed.load(std::memory_order_relaxed) || e.source_path.str.empty()) continue;
        Status st;
        e.source_path.Unlink(st, true);
      }
    }
    uint32_t reply[5] = {xcb_window, accepted ? 1u : 0u, accepted ? (xcb_atom_t)offer.action : 0, 0,
                         0};
    SendToDragSource((xcb_window_t)offer.source, atom::XdndFinished, reply);
  };

  auto CompleteEntry = [&](Ptr<DataOffer> offer, DataOffer::Entry& e, bool success) {
    if (success && e.type == atom::application_octet_stream) {
      Status st;
      WriteInto(e.dst, e.data, st);
      success = OK(st);
    }
    if (e.property != XCB_NONE) {
      free_transfer_properties.push_back((xcb_atom_t)e.property);
      e.property = XCB_NONE;
    }
    offer->Complete(e, success);
    if (success && !e.spawned) {
      e.spawned = true;
      auto lock = Lock();
      auto file = MAKE_PTR(File);
      file->SetPath(e.dst.str);
      auto loc = MAKE_PTR(Location);
      loc->InsertHere(std::move(file));
      DragLocationAction* drag = offer.get() == current_offer.get() ? Drag() : nullptr;
      if (drag) {
        drag->AddToGroup(std::move(loc));
      } else if (auto offer_location = offer->HomeLocation()) {
        if (auto board = offer_location->LockBoard()) {
          Vec2 pos = offer_location->PeekPosition() + Vec2(6_mm, -6_mm);
          loc->board = board->AcquireWeakPtr();
          {
            auto vm_lock = std::lock_guard(engine.mutex);
            board->locations.emplace(board->locations.begin(), *board, loc);
          }
          if (auto* direct = std::get_if<Location::Direct>(&loc->placement)) direct->position = pos;
          loc->WakeToys();
        }
      }
    }
    CheckDone(*offer);
  };

  auto StartOctet = [&](Ptr<DataOffer> offer, StrView name) {
    auto* e = offer->Add(name.empty() ? StrView("file") : name);
    if (!e) return;
    e->type = atom::application_octet_stream;
    e->source = offer->source;
    e->dst = UniqueNameIn(AutomatDir(), e->name);
    e->property = AcquireTransferProperty();
    e->time = xcb::last_event_time.load(std::memory_order_relaxed);
    xcb_convert_selection(connection, xcb_window, atom::XdndSelection,
                          atom::application_octet_stream, e->property, e->time);
    flush();
  };

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
            root.WakeAnimation();
          }
          break;
        }
        case XCB_UNMAP_NOTIFY: {  // ignore
          xcb_unmap_notify_event_t* ev = (xcb_unmap_notify_event_t*)event;
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
          auto lock = Lock();
          xcb_configure_notify_event_t* ev = (xcb_configure_notify_event_t*)event;
          if (ev->width != client_width || ev->height != client_height) {
            client_width = ev->width;
            client_height = ev->height;
            root.Resized(Vec2(client_width, client_height) / DisplayPxPerMeter());
          }

          // This event may be sent when the window is moved. However sometimes it holds the wrong
          // coordinates. This happens for example on Ubuntu 22.04 - only events sent from the
          // window manager are correct. Querying the position from geometry also returns the
          // wrong position. The only way to get the correct on-screen position that was found to
          // be reliable was to translate the point 0, 0 to root window coordinates.
          auto reply = xcb::translate_coordinates(xcb_window, screen->root, 0, 0);
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
        case XCB_SELECTION_NOTIFY: {
          auto* ev = (xcb_selection_notify_event_t*)event;
          if (ev->selection != atom::XdndSelection || ev->requestor != xcb_window) break;
          if (uri_probe_property != XCB_NONE && ev->property == uri_probe_property) {
            Ptr<DataOffer> offer = uri_probe_offer.Lock();
            xcb_atom_t type = XCB_NONE;
            Str list = TakeDragData(xcb_window, uri_probe_property, &type);
            free_transfer_properties.push_back(uri_probe_property);
            uri_probe_property = XCB_NONE;
            if (!offer) break;
            Vec<Str> paths;
            Str url_name;
            StrView remaining = list;
            size_t i = 0;
            while (i < remaining.size()) {
              size_t eol = remaining.find('\n', i);
              StrView line = remaining.substr(i, eol == StrView::npos ? StrView::npos : eol - i);
              i = eol == StrView::npos ? remaining.size() : eol + 1;
              while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.remove_suffix(1);
              }
              if (line.empty() || line.front() == '#') continue;
              if (line.starts_with("file://")) {
                Status st;
                Path p = PathFromFileURI(line, st);
                if (OK(st) && !p.str.empty()) paths.push_back(p.str);
              } else if (url_name.empty()) {
                url_name = Basename(line);
              }
            }
            if (!paths.empty()) {
              for (auto& p : paths) {
                auto* e = offer->Add(Basename(p));
                if (!e) break;
                e->type = atom::text_uri_list;
                e->source = offer->source;
                e->source_path = Path(p);
                e->dst = UniqueNameIn(AutomatDir(), e->name);
                e->bytes_total.store(FileSize(e->source_path), std::memory_order_relaxed);
                offer->WakeToys();
                Status st;
                CopyInto(e->source_path, e->dst, st, &e->bytes_done);
                CompleteEntry(offer, *e, OK(st));
              }
            } else if (offer->types.Contains(atom::application_octet_stream)) {
              StartOctet(offer, offer->xds_name.empty() ? url_name : offer->xds_name);
            }
            offer->sealed = true;
            CheckDone(*offer);
            break;
          }
          Found found = FindEntry(ev->property, ev->time, ev->target);
          if (!found.entry) break;
          if (ev->property == XCB_NONE) {
            CompleteEntry(found.offer, *found.entry, false);
            break;
          }
          xcb_atom_t type = XCB_NONE;
          Str data = TakeDragData(xcb_window, (xcb_atom_t)found.entry->property, &type);
          if (type == atom::INCR) {
            found.entry->incremental = true;
            break;
          }
          found.entry->data = std::move(data);
          found.entry->bytes_done.store(found.entry->data.size(), std::memory_order_relaxed);
          CompleteEntry(found.offer, *found.entry, true);
          break;
        }
        case XCB_PROPERTY_NOTIFY: {
          xcb_property_notify_event_t* ev = (xcb_property_notify_event_t*)event;
          xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
          if (ev->window == xcb_window && ev->state == XCB_PROPERTY_NEW_VALUE) {
            Found found = FindEntry(ev->atom, 0, XCB_NONE);
            if (found.entry && found.entry->incremental) {
              xcb_atom_t type = XCB_NONE;
              Str chunk = TakeDragData(xcb_window, (xcb_atom_t)found.entry->property, &type);
              if (chunk.empty()) {
                CompleteEntry(found.offer, *found.entry, true);
              } else {
                found.entry->data += chunk;
                found.entry->bytes_done.store(found.entry->data.size(), std::memory_order_relaxed);
                found.offer->WakeToys();
              }
              break;
            }
          }
          if (ev->window == xcb_window && ev->atom == atom::_NET_WM_STATE) {
            WM_STATE wm_state = WM_STATE::Get(xcb_window);
            root.maximized_horizontally = wm_state.MAXIMIZED_HORZ;
            root.maximized_vertically = wm_state.MAXIMIZED_VERT;
            root.always_on_top = wm_state.ABOVE;
          } else if (ev->window == screen->root && ev->atom == atom::_NET_ACTIVE_WINDOW) {
            auto reply =
                xcb::get_property(screen->root, atom::_NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 0, 1);
            automat::os::WindowHandle active = automat::os::kNoWindow;
            if (reply && reply->value_len == 1) {
              active = *(xcb_window_t*)xcb_get_property_value(reply.get());
            }
            xcb::active_window.store(active, std::memory_order_relaxed);
            auto lock = Lock();
            NotifyForegroundChanged(active);
          }
          break;
        }
        case XCB_CLIENT_MESSAGE: {
          xcb_client_message_event_t* cm = (xcb_client_message_event_t*)event;
          if (cm->type == atom::XdndEnter) {
            if constexpr (kDebugDragAndDrop) {
              xcb_window_t source = cm->data.data32[0];
              SmallVec<xcb_atom_t, 3> inline_types;
              for (int i = 2; i <= 4; ++i) {
                if (cm->data.data32[i] != XCB_NONE) {
                  inline_types.push_back(cm->data.data32[i]);
                }
              }
              LOG << f("XdndEnter from 0x{:x}, inline types: {}", source,
                       JoinAtomNames(inline_types.data(), inline_types.size()));
              if (auto reply = get_property(source, atom::XdndTypeList, XCB_ATOM_ATOM, 0, 256);
                  reply && reply->value_len) {
                LOG << f("  XdndTypeList: {}",
                         JoinAtomNames((xcb_atom_t*)xcb_get_property_value(reply.get()),
                                       reply->value_len));
              }
              if (auto reply = get_property(source, atom::XdndActionList, XCB_ATOM_ATOM, 0, 256);
                  reply && reply->value_len) {
                Vec<StrView> descriptions;
                auto desc_reply =
                    get_property(source, atom::XdndActionDescription, XCB_ATOM_STRING, 0, 4096);
                if (desc_reply && desc_reply->value_len) {
                  StrView blob((char*)xcb_get_property_value(desc_reply.get()),
                               (size_t)xcb_get_property_value_length(desc_reply.get()));
                  while (!blob.empty()) {
                    auto len = blob.find('\0');
                    descriptions.push_back(blob.substr(0, len));
                    if (len == StrView::npos) break;
                    blob.remove_prefix(len + 1);
                  }
                }
                auto* actions = (xcb_atom_t*)xcb_get_property_value(reply.get());
                Str line;
                for (uint32_t i = 0; i < reply->value_len; ++i) {
                  if (i) line += ", ";
                  line += atom::ToStr(actions[i]);
                  if (i < descriptions.size() && !descriptions[i].empty()) {
                    line += f(" \"{}\"", descriptions[i]);
                  }
                }
                LOG << f("  XdndActionList: {}", line);
              }
              if (auto reply = get_property(source, atom::XdndDirectSave0,
                                            XCB_GET_PROPERTY_TYPE_ANY, 0, 256);
                  reply && reply->type != XCB_NONE) {
                LOG << f("  XdndDirectSave0 ({}): {}", atom::ToStr(reply->type),
                         StrView((char*)xcb_get_property_value(reply.get()),
                                 (size_t)xcb_get_property_value_length(reply.get())));
              }
            }
            current_offer = MAKE_PTR(DataOffer);
            current_offer->source = cm->data.data32[0];
            if (cm->data.data32[1] & 1) {
              auto reply =
                  get_property(current_offer->source, atom::XdndTypeList, XCB_ATOM_ATOM, 0, 256);
              if (reply) {
                auto* list = (xcb_atom_t*)xcb_get_property_value(reply.get());
                for (uint32_t i = 0; i < reply->value_len; ++i) {
                  current_offer->types.push_back(list[i]);
                }
              }
            } else {
              for (int i = 2; i <= 4; ++i) {
                if (cm->data.data32[i] != XCB_NONE) {
                  current_offer->types.push_back(cm->data.data32[i]);
                }
              }
            }
            break;
          }
          if (cm->type == atom::XdndPosition) {
            if (!current_offer) break;
            DataOffer& offer = *current_offer;
            offer.action = cm->data.data32[4];
            Vec2 screen_px =
                Vec2((int16_t)(cm->data.data32[2] >> 16), (int16_t)(cm->data.data32[2] & 0xffff));
            bool usable = offer.types.Contains(atom::text_uri_list) ||
                          offer.types.Contains(atom::application_octet_stream);
            {
              auto lock = Lock();
              mouse_position_on_screen = screen_px;
              auto& mouse = GetMouse();
              mouse.Move(ScreenToWindowPx(screen_px));
              if (usable && !offer.started) {  // Show the offer card and start the transfers
                offer.started = true;
                auto xds = get_property(offer.source, atom::XdndDirectSave0,
                                        XCB_GET_PROPERTY_TYPE_ANY, 0, 1024);
                if (xds && xds->type != XCB_NONE && xds->value_len) {
                  offer.xds_name = Str((char*)xcb_get_property_value(xds.get()),
                                       (size_t)xcb_get_property_value_length(xds.get()));
                }
                offer_loc = MAKE_PTR(Location);
                offer_loc->InsertHere(offer.AcquirePtr());
                offer_loc->placement = Location::Direct{mouse.PositionOnCanvas()};
                mouse.actions[static_cast<int>(ui::PointerButton::Left)] =
                    std::make_unique<DragLocationAction>(mouse, offer_loc->AcquirePtr());
                live_offers.push_back(offer.AcquireWeakPtr());
                if (offer.types.Contains(atom::text_uri_list)) {
                  uri_probe_property = AcquireTransferProperty();
                  uri_probe_offer = offer.AcquireWeakPtr();
                  xcb_convert_selection(connection, xcb_window, atom::XdndSelection,
                                        atom::text_uri_list, uri_probe_property,
                                        xcb::last_event_time.load(std::memory_order_relaxed));
                  flush();
                } else {
                  StartOctet(current_offer, offer.xds_name);
                  offer.sealed = true;
                }
              }
            }
            xcb_atom_t accepted =
                offer.action == atom::XdndActionMove ? atom::XdndActionMove : atom::XdndActionCopy;
            uint32_t reply[5] = {xcb_window, usable ? 1u : 0u, 0, 0, accepted};
            SendToDragSource(cm->data.data32[0], atom::XdndStatus, reply);
            break;
          }
          if (cm->type == atom::XdndLeave) {
            {
              auto lock = Lock();
              while (auto* drag = Drag()) {
                Ptr<Location> l = drag->RemoveFromGroup(*drag->locations.front());
                if (!l) break;
              }
            }
            offer_loc.reset();
            current_offer.reset();
            std::erase_if(live_offers, [](WeakPtr<DataOffer>& w) { return !w.Lock(); });
            break;
          }
          if (cm->type == atom::XdndDrop) {
            if constexpr (kDebugDragAndDrop) {
              LOG << "XdndDrop";
            }
            if (!current_offer) {
              uint32_t reply[5] = {xcb_window, 0, 0, 0, 0};
              SendToDragSource(cm->data.data32[0], atom::XdndFinished, reply);
              break;
            }
            Ptr<DataOffer> offer = current_offer;
            current_offer.reset();
            offer_loc.reset();
            offer->action =
                offer->action == atom::XdndActionMove ? atom::XdndActionMove : atom::XdndActionCopy;
            offer->dropped = true;
            if (!offer->started) offer->sealed = true;
            {
              auto lock = Lock();
              GetMouse().ButtonUp(ui::PointerButton::Left);
            }
            CheckDone(*offer);
            flush();
            break;
          }
          if (cm->data.data32[0] == atom::WM_DELETE_WINDOW) {
            running = false;
          }
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
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                auto device_it = devices.find(ev->deviceid);
                if (device_it != devices.end() && device_it->second.vertical_scroll) {
                  auto& vertical_scroll = device_it->second.vertical_scroll.value();
                  if (ev->reason == XCB_INPUT_CHANGE_REASON_SLAVE_SWITCH) {
                    xcb_input_device_class_iterator_t it =
                        xcb_input_device_changed_classes_iterator(ev);
                    while (it.rem) {
                      xcb_input_device_class_t* it_class = it.data;
                      if (it_class->type == XCB_INPUT_DEVICE_CLASS_TYPE_VALUATOR) {
                        xcb_input_valuator_class_t* valuator_class =
                            (xcb_input_valuator_class_t*)it_class;
                        if (valuator_class->number == vertical_scroll.valuator_number) {
                          vertical_scroll.last_value = fp3232_to_double(valuator_class->value);
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
                auto ev = (xcb_input_raw_key_press_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                auto lock = Lock();
                root.keyboard.KeyDown(*ev);
                break;
              }
              case XCB_INPUT_KEY_PRESS: {
                auto ev = (xcb_input_key_press_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                ReplaceProperty32(xcb_window, atom::_NET_WM_USER_TIME, XCB_ATOM_CARDINAL, ev->time);
                auto lock = Lock();
                root.keyboard.KeyDown(*ev);
                break;
              }
              case XCB_INPUT_RAW_KEY_RELEASE: {
                auto lock = Lock();
                root.keyboard.KeyUp(*(xcb_input_raw_key_release_event_t*)event);
                break;
              }
              case XCB_INPUT_KEY_RELEASE: {
                auto ev = (xcb_input_key_release_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                auto lock = Lock();
                root.keyboard.KeyUp(*ev);
                break;
              }
              case XCB_INPUT_BUTTON_PRESS: {
                xcb_input_button_press_event_t* ev = (xcb_input_button_press_event_t*)event;
                // Ignore emulated mouse wheel "buttons"
                if (ev->flags & XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED) {
                  break;
                }
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                ReplaceProperty32(xcb_window, atom::_NET_WM_USER_TIME, XCB_ATOM_CARDINAL, ev->time);
                auto lock = Lock();
                GetMouse().ButtonDown(EventDetailToButton(ev->detail));
                break;
              }
              case XCB_INPUT_BUTTON_RELEASE: {
                xcb_input_button_release_event_t* ev = (xcb_input_button_release_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                // Ignore emulated mouse wheel "buttons"
                if (ev->flags & XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED) {
                  break;
                }
                auto lock = Lock();
                GetMouse().ButtonUp(EventDetailToButton(ev->detail));
                break;
              }
              case XCB_INPUT_RAW_BUTTON_PRESS: {
                xcb_input_raw_button_press_event_t* ev = (xcb_input_raw_button_press_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                auto* device = MouseOrNull();
                if (!device) {
                  break;
                }
                auto& pointer = *device;
                auto btn = EventDetailToButton(ev->detail);
                if (btn == automat::ui::PointerButton::Unknown) {
                  break;
                }
                for (auto& logging : pointer.loggings) {
                  logging.logger.PointerLoggerButtonDown(logging, btn);
                }
                break;
              }
              case XCB_INPUT_RAW_BUTTON_RELEASE: {
                xcb_input_raw_button_release_event_t* ev =
                    (xcb_input_raw_button_release_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                auto* device = MouseOrNull();
                if (!device) {
                  break;
                }
                auto& pointer = *device;
                auto btn = EventDetailToButton(ev->detail);
                if (btn == automat::ui::PointerButton::Unknown) {
                  break;
                }
                for (auto& logging : pointer.loggings) {
                  logging.logger.PointerLoggerButtonUp(logging, btn);
                }
                break;
              }
              case XCB_INPUT_RAW_MOTION: {
                xcb_input_raw_motion_event_t* ev = (xcb_input_raw_motion_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                auto lock = Lock();
                auto* device = MouseOrNull();
                if (!device) {
                  break;
                }
                auto& pointer = *device;
                auto valuators = RawButtonValuators(*ev);
                if (auto delta = valuators.GetVerticalScrollDelta(false)) {
                  for (auto& logging : pointer.loggings) {
                    logging.logger.PointerLoggerScrollY(logging, *delta);
                  }
                }
                if (auto xy = valuators.GetRelativeXY()) {
                  for (auto& logging : pointer.loggings) {
                    logging.logger.PointerLoggerMove(logging, *xy);
                  }
                }
                break;
              }

              case XCB_INPUT_MOTION: {
                xcb_input_motion_event_t* ev = (xcb_input_motion_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);

                auto valuators = ButtonValuators(*ev);
                if (auto delta = valuators.GetVerticalScrollDelta(true)) {
                  auto lock = Lock();
                  GetMouse().Wheel(*delta);
                }
                mouse_position_on_screen.x = fp1616_to_float(ev->root_x);
                mouse_position_on_screen.y = fp1616_to_float(ev->root_y);
                auto lock = Lock();
                GetMouse().Move(ScreenToWindowPx(mouse_position_on_screen));
                break;
              }
              case XCB_INPUT_ENTER: {
                // See http://who-t.blogspot.com/2012/06/xi-21-protocol-design-issues.html
                // Instead of ignoring the first update, we're refreshing the
                // last_scroll. It's a bit more expensive than the GTK approach,
                // but gives better UX.
                ScanDevices(*this);
                xcb_input_enter_event_t* ev = (xcb_input_enter_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                mouse_position_on_screen.x = fp1616_to_float(ev->root_x);
                mouse_position_on_screen.y = fp1616_to_float(ev->root_y);
                auto lock = Lock();
                GetMouse().Move(ScreenToWindowPx(mouse_position_on_screen));
                break;
              }
              case XCB_INPUT_LEAVE: {
                xcb_input_leave_event_t* ev = (xcb_input_leave_event_t*)event;
                xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
                if (ev->mode == XCB_INPUT_NOTIFY_MODE_NORMAL && mouse) {
                  auto lock = Lock();
                  mouse->Leave();
                  mouse_away = std::move(mouse);
                }
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
          xcb::last_event_time.store(ev->time, std::memory_order_relaxed);
          auto key = automat::x11::X11KeyCodeToKey((automat::x11::KeyCode)ev->detail);
          // LOG << "Key event: " << dump_struct(*ev) << " " << automat::ui::ToStr(key);
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
          automat::ui::Key key_struct = {
              .physical = key,
              .logical = key,
          };
          bool handled = false;
          for (auto& key_grab : automat::ui::Keyboard::key_grabs) {
            if (key_grab.key == key) {
              if (opcode == XCB_KEY_PRESS) {
                key_grab.grabber.KeyGrabberKeyDown(key_grab);
              } else {
                key_grab.grabber.KeyGrabberKeyUp(key_grab);
              }
              handled = true;
              break;
            }
          }
          if (opcode == XCB_KEY_PRESS) {
            root.keyboard.LogKeyDown(key_struct);
          } else {
            root.keyboard.LogKeyUp(key_struct);
          }
          if (!handled) {
            if (opcode == XCB_KEY_PRESS) {
              root.keyboard.KeyDown(*ev);
            } else {
              root.keyboard.KeyUp(*ev);
            }
          }
          break;
        }
        case 0: {
          xcb_generic_error_t* error = (xcb_generic_error_t*)event;
          LOG << "XCB Error: " << dump_struct(*error);
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

Optional<Vec2> XCBWindow::MousePositionScreenPx() {
  return Optional<Vec2>(mouse_position_on_screen);
}

}  // namespace xcb
