// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "keymap.hpp"

#include <xkbcommon/xkbcommon.h>

#include <cstdlib>

#include "log.hpp"

#if defined(__linux__)
#include <xkbcommon/xkbcommon-x11.h>

#include "xcb.hpp"
#endif

namespace automat {

Optional<Keymap> keymap;

// Build a keymap from the host windowing system or the OS layout. Returns null when there
// is nothing to read from, in which case Reload() compiles a default.
static xkb_keymap* BuildPlatformKeymap(xkb_context* ctx);

Keymap::Keymap() {
  ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  Reload();
}

Keymap::~Keymap() {
  if (xkb) xkb_keymap_unref(xkb);
  if (ctx) xkb_context_unref(ctx);
}

void Keymap::Reload() {
  if (!ctx) return;
  xkb_keymap* next = BuildPlatformKeymap(ctx);
  if (!next) next = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!next) {
    ERROR << "Keymap: couldn't build an xkb keymap; keyboard input will not work.";
    return;
  }
  if (xkb) xkb_keymap_unref(xkb);
  xkb = next;
  text.clear();
  if (char* s = xkb_keymap_get_as_string(xkb, XKB_KEYMAP_FORMAT_TEXT_V1)) {
    text.assign(s);
    free(s);
  }
}

#if defined(__linux__)

static xkb_keymap* BuildPlatformKeymap(xkb_context* ctx) {
  // No usable host X connection (Wayland client, headless): fall back to the default.
  if (!xcb::connection || xcb_connection_has_error(xcb::connection)) return nullptr;
  if (!xkb_x11_setup_xkb_extension(
          xcb::connection, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
          XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, nullptr, nullptr))
    return nullptr;
  int32_t device = xkb_x11_get_core_keyboard_device_id(xcb::connection);
  if (device < 0) return nullptr;
  return xkb_x11_keymap_new_from_device(ctx, xcb::connection, device, XKB_KEYMAP_COMPILE_NO_FLAGS);
}

#elif defined(_WIN32)

#include <cstring>

#include "win32.hpp"

#pragma comment(lib, "user32")

static xkb_keymap* BuildPlatformKeymap(xkb_context* ctx) {
  // Windows identifies keyboard layouts with 8-hex-digit KLID strings. This
  // table covers the common ones; anything unlisted falls back to the default.
  char klid[KL_NAMELENGTH];
  if (!GetKeyboardLayoutNameA(klid)) return nullptr;
  struct Entry {
    const char* klid;
    const char* layout;
    const char* variant;
  };
  static constexpr Entry kLayouts[] = {
      {"00000401", "ara", nullptr},        // Arabic (101)
      {"00000402", "bg", nullptr},         // Bulgarian
      {"00000405", "cz", nullptr},         // Czech
      {"00000406", "dk", nullptr},         // Danish
      {"00000407", "de", nullptr},         // German
      {"00000408", "gr", nullptr},         // Greek
      {"00000409", "us", nullptr},         // US
      {"0000040A", "es", nullptr},         // Spanish
      {"0000040B", "fi", nullptr},         // Finnish
      {"0000040C", "fr", nullptr},         // French
      {"0000040D", "il", nullptr},         // Hebrew
      {"0000040E", "hu", nullptr},         // Hungarian
      {"0000040F", "is", nullptr},         // Icelandic
      {"00000410", "it", nullptr},         // Italian
      {"00000411", "jp", nullptr},         // Japanese
      {"00000412", "kr", nullptr},         // Korean
      {"00000413", "nl", nullptr},         // Dutch
      {"00000414", "no", nullptr},         // Norwegian
      {"00000415", "pl", nullptr},         // Polish (Programmers)
      {"00000416", "br", nullptr},         // Portuguese (Brazil ABNT)
      {"00000418", "ro", nullptr},         // Romanian (Standard)
      {"00000419", "ru", nullptr},         // Russian
      {"0000041A", "hr", nullptr},         // Croatian
      {"0000041B", "sk", nullptr},         // Slovak
      {"0000041C", "al", nullptr},         // Albanian
      {"0000041D", "se", nullptr},         // Swedish
      {"0000041E", "th", nullptr},         // Thai (Kedmanee)
      {"0000041F", "tr", nullptr},         // Turkish Q
      {"00000422", "ua", nullptr},         // Ukrainian
      {"00000423", "by", nullptr},         // Belarusian
      {"00000424", "si", nullptr},         // Slovenian
      {"00000425", "ee", nullptr},         // Estonian
      {"00000426", "lv", nullptr},         // Latvian
      {"00000427", "lt", "ibm"},           // Lithuanian IBM
      {"00000429", "ir", nullptr},         // Persian
      {"0000042A", "vn", nullptr},         // Vietnamese
      {"0000042F", "mk", nullptr},         // Macedonian
      {"00000437", "ge", nullptr},         // Georgian
      {"00000438", "fo", nullptr},         // Faroese
      {"00000439", "in", nullptr},         // Devanagari-INSCRIPT
      {"0000043F", "kz", nullptr},         // Kazakh
      {"00000807", "ch", nullptr},         // Swiss German
      {"00000809", "gb", nullptr},         // UK
      {"0000080A", "latam", nullptr},      // Latin American
      {"0000080C", "be", nullptr},         // Belgian French
      {"00000813", "be", nullptr},         // Belgian (Period)
      {"00000816", "pt", nullptr},         // Portuguese
      {"0000081A", "rs", "latin"},         // Serbian (Latin)
      {"00000C0C", "ca", nullptr},         // Canadian French (Legacy)
      {"00000C1A", "rs", nullptr},         // Serbian (Cyrillic)
      {"00001009", "ca", nullptr},         // Canadian French
      {"0000100C", "ch", "fr"},            // Swiss French
      {"00001809", "ie", nullptr},         // Irish
      {"00010405", "cz", "qwerty"},        // Czech (QWERTY)
      {"00010409", "us", "dvorak"},        // US Dvorak
      {"00010415", "pl", "qwertz"},        // Polish (214)
      {"00010416", "br", nullptr},         // Portuguese (Brazil ABNT2)
      {"00010419", "ru", "typewriter"},    // Russian (Typewriter)
      {"0001041B", "sk", "qwerty"},        // Slovak (QWERTY)
      {"0001041F", "tr", "f"},             // Turkish F
      {"00010427", "lt", nullptr},         // Lithuanian (Standard)
      {"00020409", "us", "intl"},          // US International
  };
  for (const Entry& e : kLayouts) {
    if (strcmp(klid, e.klid) != 0) continue;
    xkb_rule_names names = {};  // null fields mean xkbcommon defaults
    names.layout = e.layout;
    names.variant = e.variant;
    return xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  }
  return nullptr;
}

#else

static xkb_keymap* BuildPlatformKeymap(xkb_context*) { return nullptr; }

#endif

}  // namespace automat
