#include "touchpad.h"

#include <src/base/SkUTF.h>
#include <stdint.h>

#include <deque>
#include <vector>

#include "format.h"
#include "hid.h"
#include "hidapi.h"
#include "log.h"
#include "win_main.h"

// Description of HID protocol:
// https://www.usb.org/sites/default/files/hid1_11.pdf

// Tables of HID usages:
// https://usb.org/sites/default/files/hut1_4.pdf

// Windows prevents user-mode applications from directly reading HID Reports.
// Applications can read HID Reports only through WM_INPUT messages.
// This restriction doesn't apply to HID Report Descriptor.
// https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture

namespace automat::touchpad {

std::string UTF16ToUTF8(const wchar_t* utf16) {
  size_t utf16_len = wcslen(utf16);
  int utf8_cap = utf16_len * 4 + 1;
  char utf8_buffer[utf8_cap];
  int utf8_len = SkUTF::UTF16ToUTF8(utf8_buffer, utf8_cap, (uint16_t*)utf16, utf16_len);
  return std::string(utf8_buffer, utf8_len);
}

std::string HexDump(uint8_t* ptr, size_t size) {
  std::string hex_dump;
  for (int i = 0; i < size; i++) {
    hex_dump += f("%02X ", ptr[i]);
    if (i % 16 == 15) {
      hex_dump += '\n';
    }
  }
  return hex_dump;
}

std::mutex touchpads_mutex;
std::vector<TouchPad*> touchpads;

struct ReportAccessor {
  uint8_t report_id = 0;
  std::optional<hid::Accessor> touch_valid, tip_switch, button1, contact_identifier, contact_count,
      x, y, scan_time;
  void ProcessInput(uint8_t* report, size_t report_bytes) {
    std::string log_message = "Touchpad report " + f("0x%02X", report_id);
    bool is_touch_valid = true;
    if (touch_valid) {  // Palm rejection
      is_touch_valid = touch_valid->Read<bool>(report, report_bytes);
      log_message += " touch_valid=" + f("%d", is_touch_valid);
    }
    bool is_tip_switch = true;
    if (tip_switch) {  // Finger not touching
      is_tip_switch = tip_switch->Read<bool>(report, report_bytes);
      log_message += " tip_switch=" + f("%d", is_tip_switch);
    }
    if (contact_identifier) {
      log_message += " contact_identifier=" +
                     f("%d", contact_identifier->Read<uint32_t>(report, report_bytes));
    }
    if (contact_count) {
      log_message +=
          " contact_count=" + f("%d", contact_count->Read<uint32_t>(report, report_bytes));
    }
    if (x) {
      log_message += " x=" + f("%f", x->Read<double>(report, report_bytes));
    }
    if (y) {
      log_message += " y=" + f("%f", y->Read<double>(report, report_bytes));
    }
    if (scan_time) {
      log_message += " scan_time=" + f("%f", scan_time->Read<double>(report, report_bytes));
    }
    if (button1) {
      log_message += " button1=" + f("%d", button1->Read<bool>(report, report_bytes));
    }
    LOG() << log_message;
  }
};

bool cursor_locked = false;

void LockCursor() {
  if (!cursor_locked) {
    POINT cursor;
    GetCursorPos(&cursor);
    RECT rect = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
    ClipCursor(&rect);
    cursor_locked = true;
  }
}

void UnlockCursor() {
  if (cursor_locked) {
    ClipCursor(nullptr);
    cursor_locked = false;
  }
}

time::point last_pan_time;

bool ShouldIgnoreScrollEvents() { return time::now() < last_pan_time + time::duration(1.0); }

struct TouchPadImpl {
  TouchPad touchpad;
  std::string path;
  HANDLE win32_handle = nullptr;
  std::string error;
  std::vector<ReportAccessor> report_accessors;
  TouchPadImpl(std::string_view path) : path(path) { touchpads.push_back(&touchpad); }
  // Can't be copied because `touchpad` is referenced from `touchpads`.
  TouchPadImpl(const TouchPadImpl&) = delete;
  ~TouchPadImpl() { touchpads.erase(std::find(touchpads.begin(), touchpads.end(), &touchpad)); }
  bool Ok() { return error.empty(); }
  ReportAccessor& GetOrCreateReportProcessor(uint8_t report_id) {
    for (auto& rp : report_accessors) {
      if (rp.report_id == report_id) {
        return rp;
      }
    }
    ReportAccessor& ret = report_accessors.emplace_back();
    ret.report_id = report_id;
    return ret;
  }
  void Init() {
    if (!Ok()) {
      return;
    }

    hid_device* dev = hid_open_path(path.c_str());
    uint8_t report_descriptor[HID_API_MAX_REPORT_DESCRIPTOR_SIZE];
    int report_descriptor_size =
        hid_get_report_descriptor(dev, report_descriptor, HID_API_MAX_REPORT_DESCRIPTOR_SIZE);
    hid_close(dev);
    if (report_descriptor_size < 0) {
      error = "hid_get_report_descriptor failed";
      return;
    }
    hid::ParseReportDescriptor(
        report_descriptor, report_descriptor_size, [&](uint8_t report_id, hid::Accessor& accessor) {
          ReportAccessor& report_processor = GetOrCreateReportProcessor(report_id);
          std::optional<hid::Accessor>* target_field = nullptr;
          if (accessor.usage_page == hid::UsagePage_Digitizer) {
            if (accessor.usage == hid::Usage_Digitizer_TipSwitch) {
              target_field = &report_processor.tip_switch;
            } else if (accessor.usage == hid::Usage_Digitizer_TouchValid) {
              target_field = &report_processor.touch_valid;
            } else if (accessor.usage == hid::Usage_Digitizer_ContactIdentifier) {
              target_field = &report_processor.contact_identifier;
            } else if (accessor.usage == hid::Usage_Digitizer_ContactCount) {
              target_field = &report_processor.contact_count;
            } else if (accessor.usage == hid::Usage_Digitizer_ScanTime) {
              target_field = &report_processor.scan_time;
            }
          } else if (accessor.usage_page == hid::UsagePage_Button) {
            if (accessor.usage == hid::Usage_Button_1) {
              target_field = &report_processor.button1;
              touchpad.buttons.emplace_back(false);
            }
          } else if (accessor.usage_page == hid::UsagePage_GenericDesktop) {
            if (accessor.usage == hid::Usage_GenericDesktop_X) {
              target_field = &report_processor.x;
              touchpad.width_m = (accessor.physical_maximum - accessor.physical_minimum);
            } else if (accessor.usage == hid::Usage_GenericDesktop_Y) {
              target_field = &report_processor.y;
              touchpad.height_m = (accessor.physical_maximum - accessor.physical_minimum);
            }
          }
          if (target_field) {
            target_field->emplace(accessor);
          } else {
            LOG() << "Unknown HID input. Usage Page: " << UsagePageToString(accessor.usage_page)
                  << f(" (0x%04X)", accessor.usage_page)
                  << " Usage: " << UsageToString(accessor.usage_page, accessor.usage);
          }
        });
    // TODO: Handle WM_INPUT_DEVICE_CHANGE events.
  }

  int contact_count = 0;
  int contact_i = 0;
  std::vector<Touch> old_touches;
  void ScanComplete() {
    std::vector<Touch>& new_touches = touchpad.touches;
    if (old_touches.size() == 2 && new_touches.size() == 2) {
      last_pan_time = time::now();
      touchpad.panning = true;
      LockCursor();
      float old_d = Length(old_touches[0].pos - old_touches[1].pos);
      float new_d = Length(new_touches[0].pos - new_touches[1].pos);
      // Finger distance when touching with two fingers is ~1.5cm.
      constexpr float kMinDistanceToZoom = 0.020f;  // 2cm
      // Beta parameter controls the size of the smooth transition region.
      // We want the transition zone to have ~a couple milimeters.
      // The specific value was chosen experimentally to give a nice feel.
      old_d = SoftPlus(old_d - kMinDistanceToZoom, 1000) + kMinDistanceToZoom;
      new_d = SoftPlus(new_d - kMinDistanceToZoom, 1000) + kMinDistanceToZoom;
      touchpad.zoom *= powf(new_d / old_d, 0.5f);
      Vec2 d0 = new_touches[0].pos - old_touches[0].pos;
      Vec2 d1 = new_touches[1].pos - old_touches[1].pos;
      Vec2 delta = LengthSquared(d0) < LengthSquared(d1) ? d0 : d1;
      touchpad.pan.x -= delta.x;
      touchpad.pan.y += delta.y;
    } else {
      touchpad.panning = false;
      UnlockCursor();
    }
    old_touches = new_touches;
  }

  void ProcessInputReport(uint8_t* data, size_t len) {
    if (len < 2) {
      return;
    }
    uint8_t report_id = data[0];
    uint8_t* report = data + 1;
    size_t report_bytes = len - 1;
    for (auto& report_accessor : report_accessors) {
      if (report_accessor.report_id != report_id) {
        continue;
      }
      if (report_accessor.button1) {
        touchpad.buttons[0] = report_accessor.button1->Read<bool>(report, report_bytes);
      }
      if (report_accessor.contact_count) {
        // When "Precision Touchpad" reports touches, only the first report
        // contains number of contacts. All subsequent reports have 0 contacts
        // and the same "Scan Time" as the first report.
        uint32_t cc = report_accessor.contact_count->Read<uint32_t>(report, report_bytes);
        if (cc) {
          contact_count = cc;
          contact_i = 0;
        }
      }
      if (report_accessor.contact_identifier) {
        uint32_t touch_id =
            report_accessor.contact_identifier->Read<uint32_t>(report, report_bytes);
        int touch_i = touchpad.touches.size();
        for (int i = 0; i < touchpad.touches.size(); i++) {
          if (touchpad.touches[i].id == touch_id) {
            touch_i = i;
            break;
          }
        }
        bool touch_valid = true;
        if (report_accessor.touch_valid) {
          touch_valid = report_accessor.touch_valid->Read<bool>(report, report_bytes);
        }
        if (touch_valid) {
          bool tip_switch = true;
          if (report_accessor.tip_switch) {
            tip_switch = report_accessor.tip_switch->Read<bool>(report, report_bytes);
          }
          if (tip_switch) {
            // Touch tip detected. If not present, add a new Touch object.
            if (touch_i == touchpad.touches.size()) {
              touchpad.touches.emplace_back().id = touch_id;
            }
            if (report_accessor.x) {
              touchpad.touches[touch_i].pos.x =
                  report_accessor.x->Read<double>(report, report_bytes);
            }
            if (report_accessor.y) {
              touchpad.touches[touch_i].pos.y =
                  report_accessor.y->Read<double>(report, report_bytes);
            }
          } else {
            // Touch tip disconnected. If present, remove the Touch object.
            if (touch_i < touchpad.touches.size()) {
              touchpad.touches.erase(touchpad.touches.begin() + touch_i);
            }
          }
        } else {
          // Palm detected. If present, remove the Touch object.
          if (touch_i < touchpad.touches.size()) {
            // Note: this should cancel any actions started by this touch.
            touchpad.touches.erase(touchpad.touches.begin() + touch_i);
          }
        }
      }
      if (++contact_i >= contact_count) {
        ScanComplete();
      }
      return;
    }
    ERROR() << "Unknown report: " << HexDump(data, len);
  }
};

std::list<TouchPadImpl> touchpad_impls;

void Init() {
  // Register for WM_INPUT messages on Win32
  RAWINPUTDEVICE rid = {.usUsagePage = hid::UsagePage_Digitizer,
                        .usUsage = hid::Usage_Digitizer_TouchPad,
                        .dwFlags = RIDEV_DEVNOTIFY,  // Request WM_INPUT_DEVICE_CHANGE
                        .hwndTarget = main_window};

  BOOL register_result = RegisterRawInputDevices(&rid, 1, sizeof(rid));
  if (!register_result) {
    ERROR() << "Failed to register raw input device";
  }
}

std::string GetRawInputDeviceName(HANDLE device) {
  // Calling GetRawInputDeviceInfoA produces inconsistent results. The
  // `device_name_copied` (copied bytes according to MSDN) is half of
  // `pcbSize` (number of characters according to MSDN) which doesn't make
  // sense. We're using the W version for safety.
  UINT device_name_chars = 0;
  UINT ret = GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, NULL, &device_name_chars);
  if (ret) {
    ERROR() << "Error when retrieving device name size. Error code: " << GetLastError();
    return "";
  }
  wchar_t device_name_utf16[device_name_chars];
  UINT device_name_copied =
      GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, device_name_utf16, &device_name_chars);
  if (device_name_chars != device_name_copied) {
    ERROR() << "Error when retrieving device name. Requested size=" << device_name_chars
            << ", Copied size=" << device_name_copied << ". Error code: " << GetLastError();
    return "";
  }
  return UTF16ToUTF8(device_name_utf16);
}

std::optional<LRESULT> ProcessEvent(UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_ACTIVATE: {
      uint16_t active = LOWORD(wParam);
      uint16_t minimized = HIWORD(wParam);
      if (!active) {
        std::lock_guard<std::mutex> lock(touchpads_mutex);
        for (auto& impl : touchpad_impls) {
          impl.contact_count = 0;
          impl.contact_i = 0;
          impl.touchpad.touches.clear();
          impl.ScanComplete();
        }
      }
      return 0;
    }
    case WM_INPUT: {
      HRAWINPUT hRawInput = (HRAWINPUT)lParam;
      UINT size = 0;
      UINT ret = GetRawInputData(hRawInput, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
      if (ret == (UINT)-1) {  // error when retrieving size of buffer
        ERROR() << "Error when retrieving size of buffer. Error code: " << GetLastError();
        return DefWindowProc(main_window, msg, wParam, lParam);
      }
      alignas(8) uint8_t raw_input_buffer[size];
      RAWINPUT* raw_input = (RAWINPUT*)raw_input_buffer;
      UINT size_copied =
          GetRawInputData(hRawInput, RID_INPUT, raw_input_buffer, &size, sizeof(RAWINPUTHEADER));
      if (size != size_copied) {  // error when retrieving buffer
        ERROR() << "Error when retrieving buffer. Size=" << size
                << " Error code: " << GetLastError();
        return DefWindowProc(main_window, msg, wParam, lParam);
      }
      // This shouldn't happen because we're only registered for HID devices
      if (raw_input->header.dwType != RIM_TYPEHID) {
        ERROR() << "Unexpected RAWINPUTHEADER.dwType: " << raw_input->header.dwType;
        return DefWindowProc(main_window, msg, wParam, lParam);
      }
      std::lock_guard<std::mutex> lock(touchpads_mutex);
      TouchPadImpl* touchpad = nullptr;
      // Look up TouchPad by Win32 HANDLE.
      for (TouchPadImpl& impl : touchpad_impls) {
        if (impl.win32_handle == raw_input->header.hDevice) {
          touchpad = &impl;
          break;
        }
      }
      // Look up TouchPad by it's device path.
      if (!touchpad) {
        std::string device_name = GetRawInputDeviceName(raw_input->header.hDevice);
        for (TouchPadImpl& impl : touchpad_impls) {
          if (impl.path == device_name) {
            impl.win32_handle = raw_input->header.hDevice;  // for faster lookup
            touchpad = &impl;
            break;
          }
        }
      }
      if (!touchpad) {
        std::string device_name = GetRawInputDeviceName(raw_input->header.hDevice);
        touchpad = &touchpad_impls.emplace_back(device_name);
        touchpad->win32_handle = raw_input->header.hDevice;
        touchpad->Init();
      }
      RAWHID& hid = raw_input->data.hid;
      uint8_t* ptr = hid.bRawData;
      for (int i = 0; i < hid.dwCount; ++i) {
        touchpad->ProcessInputReport(ptr, hid.dwSizeHid);
        ptr += hid.dwSizeHid;
      }
      return DefWindowProc(main_window, msg, wParam, lParam);
    }
    default:
      return std::nullopt;
  }  // switch (msg)
}

}  // namespace automat::touchpad