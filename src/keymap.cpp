// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "keymap.hpp"

#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cstring>

#include "format.hpp"
#include "log.hpp"

#if defined(__linux__)
#include <xkbcommon/xkbcommon-x11.h>

#include "xcb.hpp"
#elif defined(_WIN32)
#include "win32.hpp"

#pragma comment(lib, "user32")
#endif

namespace automat {

Keymap keymap;

void Keymap::Reload() {
  if (!ctx) ctx.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
  if (!ctx) return;
  xkb_keymap_ptr next{BuildFromPlatform()};
  if (!next) {
    next.reset(xkb_keymap_new_from_names(ctx.get(), nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS));
  }
  if (!next) {
    ERROR << "Keymap: couldn't build an xkb keymap; keyboard input will not work.";
    return;
  }
  auto lock = std::lock_guard(mutex);
  xkb = std::move(next);
  state.reset(xkb_state_new(xkb.get()));
  std::memset(key_mods, 0, sizeof(key_mods));
  xkb_keycode_t max_keycode = std::min<xkb_keycode_t>(xkb_keymap_max_keycode(xkb.get()), 255);
  for (xkb_keycode_t kc = xkb_keymap_min_keycode(xkb.get()); kc <= max_keycode; ++kc) {
    xkb_state_ptr scratch{xkb_state_new(xkb.get())};
    if (!scratch) break;
    xkb_state_update_key(scratch.get(), kc, XKB_KEY_DOWN);
    key_mods[kc] = (U8)(xkb_state_serialize_mods(scratch.get(), XKB_STATE_MODS_EFFECTIVE) & 0xff);
  }
}

#if defined(__linux__)

Keymap::xkb_keymap_ptr Keymap::BuildFromPlatform() {
  // No usable host X connection (Wayland client, headless): fall back to the default.
  if (!xcb::connection || xcb_connection_has_error(xcb::connection)) return nullptr;
  if (!xkb_x11_setup_xkb_extension(
          xcb::connection, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
          XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, nullptr, nullptr))
    return nullptr;
  int32_t device = xkb_x11_get_core_keyboard_device_id(xcb::connection);
  if (device < 0) return nullptr;
  return xkb_keymap_ptr{xkb_x11_keymap_new_from_device(ctx.get(), xcb::connection, device,
                                                       XKB_KEYMAP_COMPILE_NO_FLAGS)};
}

#elif defined(_WIN32)

namespace {

// A list of scan codes which keyboard layouts may redefine.
constexpr struct {
  U8 scan_code;
  const char* name;
} kLayoutKeys[] = {
    {0x29, "TLDE"}, {0x02, "AE01"}, {0x03, "AE02"}, {0x04, "AE03"}, {0x05, "AE04"}, {0x06, "AE05"},
    {0x07, "AE06"}, {0x08, "AE07"}, {0x09, "AE08"}, {0x0A, "AE09"}, {0x0B, "AE10"}, {0x0C, "AE11"},
    {0x0D, "AE12"}, {0x10, "AD01"}, {0x11, "AD02"}, {0x12, "AD03"}, {0x13, "AD04"}, {0x14, "AD05"},
    {0x15, "AD06"}, {0x16, "AD07"}, {0x17, "AD08"}, {0x18, "AD09"}, {0x19, "AD10"}, {0x1A, "AD11"},
    {0x1B, "AD12"}, {0x1E, "AC01"}, {0x1F, "AC02"}, {0x20, "AC03"}, {0x21, "AC04"}, {0x22, "AC05"},
    {0x23, "AC06"}, {0x24, "AC07"}, {0x25, "AC08"}, {0x26, "AC09"}, {0x27, "AC10"}, {0x28, "AC11"},
    {0x2B, "BKSL"}, {0x56, "LSGT"}, {0x2C, "AB01"}, {0x2D, "AB02"}, {0x2E, "AB03"}, {0x2F, "AB04"},
    {0x30, "AB05"}, {0x31, "AB06"}, {0x32, "AB07"}, {0x33, "AB08"}, {0x34, "AB09"}, {0x35, "AB10"},
};

// "Dead keysyms" are like Unicode "combining characters", except they happen during keyboard layout
// composition (rather than Unicode rendering). Dead keysym is essentially an accent character
// waiting for the next "base" character to attach itself to.
xkb_keysym_t DeadKeysym(U32 codepoint) {
  switch (codepoint) {
    case 0x0060:
      return XKB_KEY_dead_grave;
    case 0x0027:
    case 0x00B4:
      return XKB_KEY_dead_acute;
    case 0x005E:
      return XKB_KEY_dead_circumflex;
    case 0x007E:
      return XKB_KEY_dead_tilde;
    case 0x0022:
    case 0x00A8:
      return XKB_KEY_dead_diaeresis;
    case 0x00AF:
      return XKB_KEY_dead_macron;
    case 0x02D8:
      return XKB_KEY_dead_breve;
    case 0x02D9:
      return XKB_KEY_dead_abovedot;
    case 0x02DA:
      return XKB_KEY_dead_abovering;
    case 0x02DD:
      return XKB_KEY_dead_doubleacute;
    case 0x02C7:
      return XKB_KEY_dead_caron;
    case 0x00B8:
      return XKB_KEY_dead_cedilla;
    case 0x02DB:
      return XKB_KEY_dead_ogonek;
  }
  return XKB_KEY_NoSymbol;
}

constexpr UINT kToUnicodeNoStateChange = 0x4;

xkb_keysym_t Probe(HKL layout, UINT virtual_key, U8 scan_code, bool shift, bool alt_gr,
                   bool caps_lock) {
  BYTE key_state[256] = {};
  if (shift) {
    key_state[VK_SHIFT] = 0x80;
    key_state[VK_LSHIFT] = 0x80;
  }
  if (alt_gr) {
    key_state[VK_CONTROL] = 0x80;
    key_state[VK_LCONTROL] = 0x80;
    key_state[VK_MENU] = 0x80;
    key_state[VK_RMENU] = 0x80;
  }
  if (caps_lock) key_state[VK_CAPITAL] = 0x01;
  wchar_t utf16[8] = {};
  int len =
      ToUnicodeEx(virtual_key, scan_code, key_state, utf16, 8, kToUnicodeNoStateChange, layout);
  if (len == 0) return XKB_KEY_NoSymbol;
  U32 codepoint = utf16[0];
  if (len >= 2 && utf16[0] >= 0xD800 && utf16[0] < 0xDC00 && utf16[1] >= 0xDC00) {
    codepoint = 0x10000 + ((utf16[0] - 0xD800) << 10) + (utf16[1] - 0xDC00);
  }
  if (len < 0) {
    xkb_keysym_t dead = DeadKeysym(codepoint);
    return dead ? dead : xkb_utf32_to_keysym(codepoint);
  }
  if (codepoint < 0x20 || codepoint == 0x7F) return XKB_KEY_NoSymbol;
  return xkb_utf32_to_keysym(codepoint);
}

Str KeysymName(xkb_keysym_t keysym) {
  char name[64];
  if (keysym == XKB_KEY_NoSymbol || xkb_keysym_get_name(keysym, name, sizeof(name)) <= 0) {
    return "NoSymbol";
  }
  return name;
}

Str LayoutName(HKL layout) {
  wchar_t name[LOCALE_NAME_MAX_LENGTH];
  LCID lcid = MAKELCID(LOWORD((UINT_PTR)layout), SORT_DEFAULT);
  if (GetLocaleInfoW(lcid, LOCALE_SLOCALIZEDDISPLAYNAME, name, LOCALE_NAME_MAX_LENGTH)) {
    Str utf8 = win32::WideToUtf8(name);
    if (utf8.find('"') == Str::npos) return utf8;
  }
  return f("Layout {:08X}", (U32)(UINT_PTR)layout);
}

}  // namespace

Keymap::xkb_keymap_ptr Keymap::BuildFromPlatform() {
  HKL installed[4];
  int group_count = GetKeyboardLayoutList(4, installed);
  if (group_count <= 0) return nullptr;
  group_count = std::min(group_count, 4);
  layouts.assign(installed, installed + group_count);

  Str symbols;
  for (int group = 0; group < group_count; ++group) {
    symbols += f("name[Group{}] = \"{}\";\n", group + 1, LayoutName(installed[group]));
  }

  for (auto& [scan_code, key_name] : kLayoutKeys) {
    Str key_body;
    bool mapped_anywhere = false;
    for (int group = 0; group < group_count; ++group) {
      HKL layout = installed[group];
      UINT virtual_key = MapVirtualKeyExW(scan_code, MAPVK_VSC_TO_VK_EX, layout);
      xkb_keysym_t levels[4] = {};
      for (int level = 0; level < 4; ++level) {
        levels[level] = virtual_key
                            ? Probe(layout, virtual_key, scan_code, level & 1, level & 2, false)
                            : XKB_KEY_NoSymbol;
      }
      bool has_alt_gr = levels[2] || levels[3];
      int level_count = has_alt_gr ? 4 : 2;
      if (!levels[0] && !levels[1] && !has_alt_gr) {
        if (!key_body.empty()) key_body += ",\n  ";
        key_body += f("type[Group{}] = \"ONE_LEVEL\",\n  symbols[Group{}] = [ NoSymbol ]",
                      group + 1, group + 1);
        continue;
      }
      mapped_anywhere = true;

      bool caps_shifts = Probe(layout, virtual_key, scan_code, false, false, true) == levels[1] &&
                         levels[0] != levels[1];
      bool caps_shifts_alt_gr =
          Probe(layout, virtual_key, scan_code, false, true, true) == levels[3] &&
          levels[2] != levels[3];
      const char* type;
      if (!has_alt_gr) {
        type = caps_shifts ? "ALPHABETIC" : "TWO_LEVEL";
      } else if (caps_shifts && caps_shifts_alt_gr) {
        type = "FOUR_LEVEL_ALPHABETIC";
      } else if (caps_shifts) {
        type = "FOUR_LEVEL_SEMIALPHABETIC";
      } else {
        type = "FOUR_LEVEL";
      }

      if (!key_body.empty()) key_body += ",\n  ";
      key_body += f("type[Group{}] = \"{}\",\n  symbols[Group{}] = [ ", group + 1, type, group + 1);
      for (int level = 0; level < level_count; ++level) {
        if (level) key_body += ", ";
        key_body += KeysymName(levels[level]);
      }
      key_body += " ]";
    }
    if (!mapped_anywhere) continue;
    symbols += f("key <{}> {{\n  {}\n}};\n", key_name, key_body);
  }

  Str decimal_body;
  for (int group = 0; group < group_count; ++group) {
    xkb_keysym_t decimal = Probe(installed[group], VK_DECIMAL, 0x53, false, false, false);
    if (!decimal) decimal = XKB_KEY_KP_Decimal;
    if (!decimal_body.empty()) decimal_body += ",\n  ";
    decimal_body += f("type[Group{}] = \"KEYPAD\",\n  symbols[Group{}] = [ KP_Delete, {} ]",
                      group + 1, group + 1, KeysymName(decimal));
  }
  symbols += f("key <KPDL> {{\n  {}\n}};\n", decimal_body);

  Str text =
      f("xkb_keymap {{\n"
        "xkb_keycodes {{ include \"evdev\" }};\n"
        "xkb_types {{ include \"complete\" }};\n"
        "xkb_compat {{ include \"complete\" }};\n"
        "xkb_symbols {{\n"
        "include \"pc\"\n"
        "{}"
        "include \"level3(ralt_switch)\"\n"
        "}};\n"
        "}};\n",
        symbols);
  return xkb_keymap_ptr{xkb_keymap_new_from_string(ctx.get(), text.c_str(),
                                                   XKB_KEYMAP_FORMAT_TEXT_V1,
                                                   XKB_KEYMAP_COMPILE_NO_FLAGS)};
}

int Keymap::ActiveGroup() const {
  HKL active = GetKeyboardLayout(0);
  for (size_t i = 0; i < layouts.size(); ++i) {
    if (layouts[i] == active) return (int)i;
  }
  return -1;
}

#else

Keymap::xkb_keymap_ptr Keymap::BuildFromPlatform() { return nullptr; }

#endif

}  // namespace automat
