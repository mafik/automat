// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "path.hh"
#include "status.hh"

namespace automat {

namespace ui {
struct RootWidget;
}  // namespace ui

Path StatePath();

void SaveState(ui::RootWidget&, Status&);
void LoadState(ui::RootWidget&, Status&);

}  // namespace automat