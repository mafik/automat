#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <xkbcommon/xkbcommon.h>

#include <memory>
#include <mutex>

#include "int.hpp"
#include "str.hpp"
#include "unique_ptr.hpp"
#include "vec.hpp"

struct HKL__;

namespace automat {

// The process-wide keyboard layout (shared by all Keyboards).
struct Keymap {
  using xkb_context_ptr = std::unique_ptr<xkb_context, DeleteWith<xkb_context_unref>>;
  using xkb_keymap_ptr = std::unique_ptr<xkb_keymap, DeleteWith<xkb_keymap_unref>>;
  using xkb_state_ptr = std::unique_ptr<xkb_state, DeleteWith<xkb_state_unref>>;

  xkb_context_ptr ctx;
  xkb_keymap_ptr xkb;
  xkb_state_ptr state;
  U8 key_mods[256] = {};
  std::mutex mutex;

  Keymap() = default;
  Keymap(const Keymap&) = delete;

  // Rebuild from the best available source. Call when the OS reports a layout change.
  void Reload();

#if defined(_WIN32)
  Vec<HKL__*> layouts;
  int ActiveGroup() const;
#endif

  xkb_keymap_ptr BuildFromPlatform();
};

extern Keymap keymap;

}  // namespace automat
