// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "path.hh"
#include "status.hh"

namespace automat {

namespace gui {
struct Window;
}  // namespace gui

maf::Path StatePath();

void SaveState(gui::Window&, maf::Status&);
void LoadState(gui::Window&, maf::Status&);

}  // namespace automat