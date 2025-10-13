// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "object.hh"

namespace automat {

// Objects in Automat can be fairly large. Iconification is a mechanism that allows players to
// shrink them so that they fit in a 1x1cm square.
bool IsIconified(Object*);
void Iconify(Object&);
void Deiconify(Object&);
void SetIconified(Object&, bool iconified);

}  // namespace automat
