// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "path.hh"
#include "status.hh"

namespace automat {

namespace gui {
struct RootWidget;
}  // namespace gui

Path StatePath();

void SaveState(gui::RootWidget&, Status&);
void LoadState(gui::RootWidget&, Status&);

}  // namespace automat