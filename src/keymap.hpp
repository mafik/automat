#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <xkbcommon/xkbcommon.h>

#include "int.hpp"
#include "ptr.hpp"
#include "unique_ptr.hpp"
#include "vec.hpp"

struct HKL__;

namespace automat {

// The process-wide keyboard layout (shared by all Keyboards).
struct Keymap : ReferenceCounted {
  using xkb_context_ptr = std::unique_ptr<xkb_context, DeleteWith<xkb_context_unref>>;
  using xkb_keymap_ptr = std::unique_ptr<xkb_keymap, DeleteWith<xkb_keymap_unref>>;
  using xkb_state_ptr = std::unique_ptr<xkb_state, DeleteWith<xkb_state_unref>>;

  xkb_keymap_ptr xkb;
  U8 key_mods[256] = {};

#if defined(_WIN32)
  Vec<HKL__*> layouts;

  int ActiveGroup() const;
#endif
};

extern Ptr<Keymap> keymap;

void ReloadKeymap();

}  // namespace automat
