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

#else

static xkb_keymap* BuildPlatformKeymap(xkb_context*) { return nullptr; }

#endif

}  // namespace automat
