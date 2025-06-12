// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "hid.hh"

#include <cmath>
#include <deque>

#include "format.hh"
#include "log.hh"

namespace automat::hid {

const char* UsagePageToString(UsagePage usage_page) {
  switch ((uint16_t)usage_page) {
    case 0x00:
      return "Undefined";
    case 0x01:
      return "Generic Desktop Page";
    case 0x02:
      return "Simulation Controls Page";
    case 0x03:
      return "VR Controls Page";
    case 0x04:
      return "Sport Controls Page";
    case 0x05:
      return "Game Controls Page";
    case 0x06:
      return "Generic Device Controls Page";
    case 0x07:
      return "Keyboard/Keypad Page";
    case 0x08:
      return "LED Page";
    case 0x09:
      return "Button Page";
    case 0x0A:
      return "Ordinal Page";
    case 0x0B:
      return "Telephony Device Page";
    case 0x0C:
      return "Consumer Page";
    case 0x0D:
      return "Digitizers Page";
    case 0x0E:
      return "Haptics Page";
    case 0x0F:
      return "Physical Input Device Page";
    case 0x10:
      return "Unicode Page";
    case 0x11:
      return "SoC Page";
    case 0x12:
      return "Eye and Head Trackers Page";
    case 0x13:
      return "Reserved";
    case 0x14:
      return "Auxiliary Display Page";
    case 0x20:
      return "Sensors Page";
    case 0x40:
      return "Medical Instrument Page";
    case 0x41:
      return "Braille Display Page";
    case 0x59:
      return "Lighting And Illumination Page";
    case 0x80:
      return "Monitor Page";
    case 0x81:
      return "Monitor Enumerated Page";
    case 0x82:
      return "VESA Virtual Controls Page";
    case 0x84:
      return "Power Page";
    case 0x85:
      return "Battery System Page";
    case 0x8C:
      return "Barcode Scanner Page";
    case 0x8D:
      return "Scales Page";
    case 0x8E:
      return "Magnetic Stripe Reader Page";
    case 0x90:
      return "Camera Control Page";
    case 0x91:
      return "Arcade Page";
    case 0x92:
      return "Gaming Device Page";
    case 0xF1D0:
      return "FIDO Alliance Page";
    default:
      if ((uint16_t)usage_page >= 0xFF00 && (uint16_t)usage_page <= 0xFFFF) {
        return "Vendor-defined";
      } else {
        return "Reserved";
      }
  }
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch"
const char* UsageToString(UsagePage usage_page, Usage usage) {
  switch ((uint16_t)usage_page) {
    case 0x01:  // Generic Desktop Page
      switch (usage) {
        case 0x00:
          return "Undefined";
        case 0x01:
          return "Pointer";
        case 0x02:
          return "Mouse";
        case 0x04:
          return "Joystick";
        case 0x05:
          return "Gamepad";
        case 0x06:
          return "Keyboard";
        case 0x07:
          return "Keypad";
        case 0x08:
          return "Multi-axis Controller";
        case 0x09:
          return "Tablet PC System Controls";
        case 0x0A:
          return "Water Cooling Device";
        case 0x0B:
          return "Computer Chassis Device";
        case 0x0C:
          return "Wireless Radio Controls";
        case 0x0D:
          return "Portable Device Control";
        case 0x0E:
          return "System Multi-Axis Controller";
        case 0x0F:
          return "Spatial Controller";
        case 0x10:
          return "Assistive Control";
        case 0x11:
          return "Device Dock";
        case 0x12:
          return "Dockable Device";
        case 0x13:
          return "Call State Management Control";
        case 0x30:
          return "X";
        case 0x31:
          return "Y";
        case 0x32:
          return "Z";
        case 0x33:
          return "Rx";
        case 0x34:
          return "Ry";
        case 0x35:
          return "Rz";
        case 0x36:
          return "Slider";
        case 0x37:
          return "Dial";
        case 0x38:
          return "Wheel";
        case 0x39:
          return "Hat Switch";
        case 0x3A:
          return "Counted Buffer";
        case 0x3B:
          return "Byte Count";
        case 0x3C:
          return "Motion Wakeup";
        case 0x3D:
          return "Start";
        case 0x3E:
          return "Select";
        case 0x40:
          return "Vx";
        case 0x41:
          return "Vy";
        case 0x42:
          return "Vz";
        case 0x43:
          return "Vbrx";
        case 0x44:
          return "Vbry";
        case 0x45:
          return "Vbrz";
        case 0x46:
          return "Vno";
        case 0x47:
          return "Feature Notification";
        case 0x48:
          return "Resolution Multiplier";
        case 0x49:
          return "Qx";
        case 0x4A:
          return "Qy";
        case 0x4B:
          return "Qz";
        case 0x4C:
          return "Qw";
        case 0x80:
          return "System Control";
        case 0x81:
          return "System Power Down";
        case 0x82:
          return "System Sleep";
        case 0x83:
          return "System Wake Up";
        case 0x84:
          return "System Context Menu";
        case 0x85:
          return "System Main Menu";
        case 0x86:
          return "System App Menu";
        case 0x87:
          return "System Menu Help";
        case 0x88:
          return "System Menu Exit";
        case 0x89:
          return "System Menu Select";
        case 0x8A:
          return "System Menu Right";
        case 0x8B:
          return "System Menu Left";
        case 0x8C:
          return "System Menu Up";
        case 0x8D:
          return "System Menu Down";
        case 0x8E:
          return "System Cold Restart";
        case 0x8F:
          return "System Warm Restart";
        case 0x90:
          return "D-pad Up";
        case 0x91:
          return "D-pad Down";
        case 0x92:
          return "D-pad Right";
        case 0x93:
          return "D-pad Left";
        case 0x94:
          return "Index Trigger";
        case 0x95:
          return "Palm Trigger";
        case 0x96:
          return "Thumbstick";
        case 0x97:
          return "System Function Shift";
        case 0x98:
          return "System Function Shift Lock";
        case 0x99:
          return "System Function Shift Lock Indicator";
        case 0x9A:
          return "System Dismiss Notification";
        case 0x9B:
          return "System Do Not Disturb";
        case 0xA0:
          return "System Dock";
        case 0xA1:
          return "System Undock";
        case 0xA2:
          return "System Setup";
        case 0xA3:
          return "System Break";
        case 0xA4:
          return "System Debugger Break";
        case 0xA5:
          return "Application Break";
        case 0xA6:
          return "Application Debugger Break";
        case 0xA7:
          return "System Speaker Mute";
        case 0xA8:
          return "System Hibernate";
        case 0xA9:
          return "System Microphone Mute";
        case 0xB0:
          return "System Display Invert";
        case 0xB1:
          return "System Display Internal";
        case 0xB2:
          return "System Display External";
        case 0xB3:
          return "System Display Both";
        case 0xB4:
          return "System Display Dual";
        case 0xB5:
          return "System Display Toggle Int/Ext Mode";
        case 0xB6:
          return "System Display Swap Primary/Secondary";
        case 0xB7:
          return "System Display Toggle LCD Autoscale";
        case 0xC0:
          return "Sensor Zone";
        case 0xC1:
          return "RPM";
        case 0xC2:
          return "Coolant Level";
        case 0xC3:
          return "Coolant Critical Level";
        case 0xC4:
          return "Coolant Pump";
        case 0xC5:
          return "Chassis Enclosure";
        case 0xC6:
          return "Wireless Radio Button";
        case 0xC7:
          return "Wireless Radio LED";
        case 0xC8:
          return "Wireless Radio Slider Switch";
        case 0xC9:
          return "System Display Rotation Lock Button";
        case 0xCA:
          return "System Display Rotation Lock Slider Switch";
        case 0xCB:
          return "Control Enable";
        case 0xD0:
          return "Dockable Device Unique ID";
        case 0xD1:
          return "Dockable Device Vendor ID";
        case 0xD2:
          return "Dockable Device Primary Usage Page";
        case 0xD3:
          return "Dockable Device Primary Usage ID";
        case 0xD4:
          return "Dockable Device Docking State";
        case 0xD5:
          return "Dockable Device Display Occlusion";
        case 0xD6:
          return "Dockable Device Object Type";
        case 0xE0:
          return "Call Active LED";
        case 0xE1:
          return "Call Mute Toggle";
        case 0xE2:
          return "Call Mute LED";
        default:
          return "Reserved";
      }
    case 0x0D:  // Digitizers Page
      switch (usage) {
        case 0x00:
          return "Undefined";
        case 0x01:
          return "Digitizer";
        case 0x02:
          return "Pen";
        case 0x03:
          return "Light Pen";
        case 0x04:
          return "Touch Screen";
        case 0x05:
          return "Touch Pad";
        case 0x06:
          return "Whiteboard";
        case 0x07:
          return "Coordinate Measuring Machine";
        case 0x08:
          return "3D Digitizer";
        case 0x09:
          return "Stereo Plotter";
        case 0x0A:
          return "Articulated Arm";
        case 0x0B:
          return "Armature";
        case 0x0C:
          return "Multiple Point Digitizer";
        case 0x0D:
          return "Free Space Wand";
        case 0x0E:
          return "Device Configuration";
        case 0x0F:
          return "Capacitive Heat Map Digitizer";
        case 0x20:
          return "Stylus";
        case 0x21:
          return "Puck";
        case 0x22:
          return "Finger";
        case 0x23:
          return "Device settings";
        case 0x24:
          return "Character Gesture";
        case 0x30:
          return "Tip Pressure";
        case 0x31:
          return "Barrel Pressure";
        case 0x32:
          return "In Range";
        case 0x33:
          return "Touch";
        case 0x34:
          return "Untouch";
        case 0x35:
          return "Tap";
        case 0x36:
          return "Quality";
        case 0x37:
          return "Data Valid";
        case 0x38:
          return "Transducer Index";
        case 0x39:
          return "Tablet Function Keys";
        case 0x3A:
          return "Program Change Keys";
        case 0x3B:
          return "Battery Strength";
        case 0x3C:
          return "Invert";
        case 0x3D:
          return "X Tilt";
        case 0x3E:
          return "Y Tilt";
        case 0x3F:
          return "Azimuth";
        case 0x40:
          return "Altitude";
        case 0x41:
          return "Twist";
        case 0x42:
          return "Tip Switch";
        case 0x43:
          return "Secondary Tip Switch";
        case 0x44:
          return "Barrel Switch";
        case 0x45:
          return "Eraser";
        case 0x46:
          return "Tablet Pick";
        case 0x47:
          return "Touch Valid";
        case 0x48:
          return "Width";
        case 0x49:
          return "Height";
        case 0x51:
          return "Contact Identifier";
        case 0x52:
          return "Device Mode";
        case 0x53:
          return "Device Identifier";
        case 0x54:
          return "Contact Count";
        case 0x55:
          return "Contact Count Maximum";
        case 0x56:
          return "Scan Time";
        case 0x57:
          return "Surface Switch";
        case 0x58:
          return "Button Switch";
        case 0x59:
          return "Pad Type";
        case 0x5A:
          return "Secondary Barrel Switch";
        case 0x5B:
          return "Transducer Serial Number";
        case 0x5C:
          return "Preferred Color";
        case 0x5D:
          return "Preferred Color is Locked";
        case 0x5E:
          return "Preferred Line Width";
        case 0x5F:
          return "Preferred Line Width is Locked";
        case 0x60:
          return "Latency Mode";
        case 0x61:
          return "Gesture Character Quality";
        case 0x62:
          return "Character Gesture Data Length";
        case 0x63:
          return "Character Gesture Data";
        case 0x64:
          return "Gesture Character Encoding";
        case 0x65:
          return "UTF8 Character Gesture Encoding";
        case 0x66:
          return "UTF16 Little Endian Character Gesture Encoding";
        case 0x67:
          return "UTF16 Big Endian Character Gesture Encoding";
        case 0x68:
          return "UTF32 Little Endian Character Gesture Encoding";
        case 0x69:
          return "UTF32 Big Endian Character Gesture Encoding";
        case 0x6A:
          return "Capacitive Heat Map Protocol Vendor ID";
        case 0x6B:
          return "Capacitive Heat Map Protocol Version";
        case 0x6C:
          return "Capacitive Heat Map Frame Data";
        case 0x6D:
          return "Gesture Character Enable";
        case 0x6E:
          return "Transducer Serial Number Part 2";
        case 0x6F:
          return "No Preferred Color";
        case 0x70:
          return "Preferred Line Style NAry";
        case 0x71:
          return "Preferred Line Style is Locked";
        case 0x72:
          return "Ink";
        case 0x73:
          return "Pencil";
        case 0x74:
          return "Highlighter";
        case 0x75:
          return "Chisel Marker";
        case 0x76:
          return "Brush";
        case 0x77:
          return "No Preference";
        case 0x80:
          return "Digitizer Diagnostic";
        case 0x81:
          return "Digitizer Error";
        case 0x82:
          return "Err Normal Status";
        case 0x83:
          return "Err Transducers Exceeded";
        case 0x84:
          return "Err Full Trans Features Unavailable";
        case 0x85:
          return "Err Charge Low";
        case 0x90:
          return "Transducer Software Info";
        case 0x91:
          return "Transducer Vendor Id";
        case 0x92:
          return "Transducer Product Id";
        case 0x93:
          return "Device Supported Protocols";
        case 0x94:
          return "Transducer Supported Protocols";
        case 0x95:
          return "No Protocol";
        case 0x96:
          return "Wacom AES Protocol";
        case 0x97:
          return "USI Protocol";
        case 0x98:
          return "Microsoft Pen Protocol";
        case 0xA0:
          return "Supported Report Rates";
        case 0xA1:
          return "Report Rate";
        case 0xA2:
          return "Transducer Connected";
        case 0xA3:
          return "Switch Disabled";
        case 0xA4:
          return "Switch Unimplemented";
        case 0xA5:
          return "Transducer Switches";
        case 0xA6:
          return "Transducer Index Selector";
        case 0xB0:
          return "Button Press Threshold";
        default:
          return "Reserved";
      }
    default:
      return "Consult https://usb.org/sites/default/files/hut1_4.pdf";
  }
}
#pragma clang diagnostic pop

Accessor::Accessor(UsagePage usage_page, Usage usage, uint32_t bit_offset, uint32_t bit_width,
                   uint32_t hid_logical_minimum, uint32_t hid_logical_maximum,
                   uint32_t hid_physical_minimum, uint32_t hid_physical_maximum,
                   int8_t hid_exponent, Unit unit)
    : usage_page(usage_page),
      usage(usage),
      bit_offset(bit_offset),
      bit_width(bit_width),
      logical_minimum(hid_logical_minimum),
      logical_maximum(hid_logical_maximum) {
  if ((hid_physical_maximum == UINT32_MAX) || (hid_physical_minimum == UINT32_MAX) ||
      ((hid_physical_maximum == 0) && (hid_physical_minimum == 0))) {
    physical_maximum = logical_maximum;
    physical_minimum = logical_minimum;
  } else {
    physical_maximum = hid_physical_maximum;
    physical_minimum = hid_physical_minimum;
  }
  double exponent = (hid_exponent == UINT32_MAX) ? 1 : pow(10, hid_exponent);
  if (unit == Unit::Centimeter) {
    exponent *= 0.01;
  } else if (unit == Unit::Inch) {
    exponent *= 0.0254;
  }
  physical_maximum *= exponent;
  physical_minimum *= exponent;
}

template <>
bool Accessor::Read<bool>(const uint8_t* report, size_t report_bytes) const {
  if (bit_offset >= report_bytes * 8) return false;
  return (report[bit_offset / 8] >> (bit_offset % 8)) & 1;
}

template <>
uint32_t Accessor::Read<uint32_t>(const uint8_t* report, size_t report_bytes) const {
  if (bit_offset + bit_width > report_bytes * 8) return 0;
  if ((bit_offset & 7) || (bit_width & 7)) {
    // Read bit by bit.
    uint32_t ret = 0;
    for (uint32_t i = 0; i < bit_width; ++i) {
      uint32_t total_bit_offset = bit_offset + i;
      uint32_t byte = total_bit_offset / 8;
      uint32_t bit = total_bit_offset % 8;
      ret |= ((report[byte] >> bit) & 1) << i;
    }
    return ret;
  } else {
    // Read byte by byte.
    uint32_t bytes = bit_width / 8;
    uint32_t byte_offset = bit_offset / 8;
    uint32_t ret = 0;
    for (uint32_t i = 0; i < bytes; ++i) {
      ret |= report[byte_offset + i] << (i * 8);
    }
    return ret;
  }
}

template <>
double Accessor::Read<double>(const uint8_t* report, size_t report_bytes) const {
  uint32_t raw_value = Read<uint32_t>(report, report_bytes);
  if (raw_value < logical_minimum) return physical_minimum;
  if (raw_value > logical_maximum) return physical_maximum;
  return physical_minimum + (physical_maximum - physical_minimum) * (raw_value - logical_minimum) /
                                (logical_maximum - logical_minimum);
}

static std::string HexDump(const uint8_t* ptr, size_t size) {
  std::string hex_dump;
  for (int i = 0; i < size; i++) {
    hex_dump += f("{:02X} ", ptr[i]);
    if (i % 16 == 15) {
      hex_dump += '\n';
    }
  }
  return hex_dump;
}

void ParseReportDescriptor(const uint8_t* report_descriptor, size_t report_descriptor_bytes,
                           std::function<void(uint8_t report_id, Accessor& accessor)> callback) {
  enum class Contents : bool { Data = 0, Constant = 1 };
  enum class Encoding : bool { Array = 0, Variable = 1 };
  enum class Origin : bool { Absolute = 0, Relative = 1 };
  enum class Wrapping : bool { NoWrap = 0, Wrap = 1 };
  enum class Linear : bool { Linear = 0, NonLinear = 1 };
  enum class PreferredState : bool { PreferredState = 0, NoPreferredState = 1 };
  enum class NullPosition : bool { NoNullPosition = 0, NullState = 1 };
  enum class Volatile : bool { NonVolatile = 0, Volatile = 1 };
  enum class BitField : bool { BitField = 0, BufferedBytes = 1 };

  struct Options {
    Contents contents : 1;
    Encoding encoding : 1;
    Origin origin : 1;
    Wrapping wrapping : 1;
    Linear linear : 1;
    PreferredState preferred_state : 1;
    NullPosition null_position : 1;
    Volatile volatile_ : 1;
    BitField bit_field : 1;
  };

  int pos = 0;
  hid::UsagePage usage_page = hid::UsagePage_Undefined;
  uint32_t logical_minimum = 0;
  uint32_t logical_maximum = 0;
  uint32_t physical_minimum = 0;
  uint32_t physical_maximum = 0;
  uint8_t report_id = 0;
  uint32_t report_size = 0;
  uint32_t report_count = 0;
  int8_t exponent = 0;
  hid::Unit unit = hid::Unit::None;
  std::deque<hid::Usage> usages;
  uint32_t bit_offset = 0;
  while (pos < report_descriptor_bytes) {
    enum ItemCategory : uint8_t {
      kItemCategoryMain = 0,
      kItemCategoryGlobal = 1,
      kItemCategoryLocal = 2,
      kItemCategoryReserved = 3,
    };
    uint8_t bSize = report_descriptor[pos] & 0b11;
    ItemCategory category = (ItemCategory)((report_descriptor[pos] >> 2) & 0b11);
    uint8_t bTag = (report_descriptor[pos] >> 4) & 0b1111;
    pos += 1;
    uint32_t data = 0;
    switch (bSize) {
      case 3:
        data = *(uint32_t*)(report_descriptor + pos);
        pos += 4;
        break;
      case 2:
        data = *(uint16_t*)(report_descriptor + pos);
        pos += 2;
        break;
      case 1:
        data = report_descriptor[pos++];
        break;
      case 0:
        break;
    }
    // Report error for long tags.
    if (bTag == 0b1111 && category == kItemCategoryReserved && bSize == 0b10) {
      ERROR << "Encountered a \"long tag\" when parsing HID report descriptor. "
               "This is not implemented yet. Full HID report descriptor:\n"
            << HexDump(report_descriptor, report_descriptor_bytes);
      return;
    }
    std::string opts = "";
    if (category == kItemCategoryMain) {
      switch (bTag) {
        case 0b1000: {  // Input
          Options opts = *(Options*)&data;
          if (opts.contents == Contents::Constant) {
            bit_offset += report_size * report_count;
          } else if (opts.contents == Contents::Data) {
            if (opts.encoding == Encoding::Array) {
              ERROR << "TODO: Array encoding is not (yet) supported.";
            } else if (opts.encoding == Encoding::Variable) {
              hid::Usage usage = hid::Usage_Undefined;
              for (int i = 0; i < report_count; ++i) {
                if (!usages.empty()) {
                  usage = usages.front();
                  usages.pop_front();
                }
                hid::Accessor accessor = hid::Accessor(
                    usage_page, usage, bit_offset, report_size, logical_minimum, logical_maximum,
                    physical_minimum, physical_maximum, exponent, unit);
                callback(report_id, accessor);
                bit_offset += report_size;
              }
            }
          }
          break;
        }
        case 0b1001:  // Output
          break;
        case 0b1011:  // Feature
          break;
        case 0b1010:  // Begin collection
          switch (data) {
            case 0x00:  // "Physical - group of axes";
              break;
            case 0x01:  // "Application - mouse, keyboard";
              break;
            case 0x02:  // "Logical - interrelated data";
              break;
            case 0x03:  // "Report";
              break;
            case 0x04:  // "Named Array";
              break;
            case 0x05:  // "Usage Switch";
              break;
            case 0x06:  // "Usage Modifier";
              break;
            default:  // "Reserved or Vendor-defined";
              break;
          }
          break;
        case 0b1100:  // End collection
          break;
        default:  // "Reserved" Main Item;
          break;
      }
    } else if (category == kItemCategoryGlobal) {
      switch (bTag) {
        case 0b0000:
          usage_page = hid::UsagePage(data);
          break;
        case 0b0001:
          logical_minimum = data;
          break;
        case 0b0010:
          logical_maximum = data;
          break;
        case 0b0011:
          physical_minimum = data;
          break;
        case 0b0100:
          physical_maximum = data;
          break;
        case 0b0101:
          if (data >= 8 && data <= 15) {
            exponent = ((int8_t)data) - 16;
          } else {
            exponent = data;
          }
          break;
        case 0b0110:
          unit = (hid::Unit)data;
          break;
        case 0b0111:
          report_size = data;
          break;
        case 0b1000:
          report_id = data;
          bit_offset = 0;
          usages.clear();
          break;
        case 0b1001:
          report_count = data;
          break;
        default:
          // Push & Pop are not implemented because I couldn't find any
          // reference devices that would use them.
          ERROR << "Unknown global tag: " << f("{:#02x}", bTag)
                << ". See \"Global items\" in "
                   "https://www.usb.org/sites/default/files/hid1_11.pdf";
      }
    } else if (category == kItemCategoryLocal) {
      switch (bTag) {
        case 0b0000:
          usages.push_back((hid::Usage)data);
          break;
        default:
          ERROR << "Unknown local tag: " << f("0x02x", bTag)
                << ". See \"Local items\" in "
                   "https://www.usb.org/sites/default/files/hid1_11.pdf";
      }
    }
  }
}

}  // namespace automat::hid