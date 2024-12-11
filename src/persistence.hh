// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "path.hh"
#include "status.hh"

namespace automat {

namespace gui {
struct RootWidget;
}  // namespace gui

maf::Path StatePath();

void SaveState(gui::RootWidget&, maf::Status&);
void LoadState(gui::RootWidget&, maf::Status&);

}  // namespace automat