#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "path.hpp"
#include "status.hpp"

namespace automat {

namespace ui {
struct RootWidget;
}  // namespace ui

Path StatePath();

void SaveState(ui::RootWidget&, Status&);
void LoadState(ui::RootWidget&, Status&);

}  // namespace automat