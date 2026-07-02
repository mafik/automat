#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <string>

#include "optional.hpp"

struct xkb_context;
struct xkb_keymap;

namespace automat {

// The process-wide keyboard layout: the single source of truth for what Automat tells its
// embedded clients — both the X11 server and the Wayland compositor — the keyboard produces.
// It is built from whatever the platform offers (the host X server, the host Wayland
// compositor, the OS layout on Windows and macOS, or a compiled default), so no server code
// assumes a particular windowing system is present.
struct Keymap {
  xkb_context* ctx = nullptr;
  xkb_keymap* xkb = nullptr;  // the compiled layout; the servers read it directly

  Keymap();
  ~Keymap();
  Keymap(const Keymap&) = delete;

  // Rebuild from the best available source. Call when the OS reports a layout change.
  void Reload();

  // The layout serialized as an XKB text keymap, handed to Wayland clients over a memfd.
  std::string text;
};

extern Optional<Keymap> keymap;

}  // namespace automat
