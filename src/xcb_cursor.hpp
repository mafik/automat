#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <xcb/xcb.h>

namespace automat::ui {
enum class Cursor;
}  // namespace automat::ui

namespace xcb {

xcb_cursor_t LoadCursor(automat::ui::Cursor);

}  // namespace xcb
