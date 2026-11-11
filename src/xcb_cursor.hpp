#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <xcb/xcb.h>

namespace xcb {

xcb_cursor_t LoadCursor(const char* name);

}  // namespace xcb
