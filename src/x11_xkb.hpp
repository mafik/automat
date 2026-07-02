#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Automat's own XKEYBOARD extension, served from the shared keymap (keymap.hpp), so embedded
// X11 clients get the layout regardless of the platform Automat runs on. See x11_xkb.cpp and
// docs/parrots/X11 Server.md.

#include "int.hpp"

namespace automat::ui {
struct Key;
}  // namespace automat::ui

namespace automat::x11 {
struct Client;
}  // namespace automat::x11

namespace automat::x11::xkb {

constexpr U8 kMajorOpcode = 135;
constexpr U8 kFirstEvent = 88;   // one slot; XKB multiplexes its events by an XkbType byte
constexpr U8 kFirstError = 150;  // XKB defines a single "Keyboard" error

// Translate between ui::Key's modifier bools and the 8-bit X11/xkb real-modifier mask:
// Shift, Lock, Control, Mod1..Mod5, with the Mods bound the way every evdev keymap binds
// them (Mod1=alt, Mod2=num_lock, Mod3=level5, Mod4=windows, Mod5=alt_gr).
void FillModifiers(ui::Key&, U32 mask);
U8 ModifierMask(const ui::Key&);

// Handle one XKB request (minor opcode `minor`); `raw`/`raw_len` are its bytes verbatim.
bool Dispatch(Client&, U8 minor, const U8* raw, size_t raw_len);

}  // namespace automat::x11::xkb
