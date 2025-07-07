// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <functional>

namespace automat::hid {

enum UsagePage : uint16_t {
  UsagePage_Undefined = 0x00,
  UsagePage_GenericDesktop = 0x01,
  UsagePage_Button = 0x09,
  UsagePage_Digitizer = 0x0D
};

const char* UsagePageToString(UsagePage usage_page);

enum Usage : uint16_t {
  Usage_Undefined = 0x00,

  Usage_GenericDesktop_Mouse = 0x02,
  Usage_GenericDesktop_Keyboard = 0x06,

  Usage_GenericDesktop_X = 0x30,
  Usage_GenericDesktop_Y = 0x31,

  Usage_Button_1 = 0x01,

  Usage_Digitizer_TouchPad = 0x05,
  Usage_Digitizer_TipSwitch = 0x42,
  Usage_Digitizer_TouchValid = 0x47,
  Usage_Digitizer_ContactIdentifier = 0x51,
  Usage_Digitizer_ContactCount = 0x54,
  Usage_Digitizer_ScanTime = 0x56,
};

const char* UsageToString(UsagePage usage_page, Usage usage);

enum class Unit : uint32_t {
  None = 0x00,
  Centimeter = 0x11,
  Inch = 0x13,
  Second = 0x1001,
};

struct Accessor {
  UsagePage usage_page;
  Usage usage;
  uint32_t bit_offset;
  uint32_t bit_width;
  uint32_t logical_minimum;
  uint32_t logical_maximum;
  double physical_minimum;
  double physical_maximum;

  Accessor(UsagePage usage_page, Usage usage, uint32_t bit_offset, uint32_t bit_width,
           uint32_t hid_logical_minimum, uint32_t hid_logical_maximum,
           uint32_t hid_physical_minimum, uint32_t hid_physical_maximum, int8_t hid_exponent,
           Unit unit);

  template <class T>
  T Read(const uint8_t* report, size_t report_bytes) const;
};

template <>
bool Accessor::Read<bool>(const uint8_t* report, size_t report_bytes) const;

template <>
uint32_t Accessor::Read<uint32_t>(const uint8_t* report, size_t report_bytes) const;

template <>
double Accessor::Read<double>(const uint8_t* report, size_t report_bytes) const;

void ParseReportDescriptor(const uint8_t* report_descriptor, size_t report_descriptor_bytes,
                           std::function<void(uint8_t report_id, Accessor& accessor)> callback);

}  // namespace automat::hid