#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "path.hpp"
#include "status.hpp"
#include "str.hpp"

namespace automat {

Path AutomatDir();

Path CreateUniqueFile(const Path& dir, StrView basename, Status&);

Path PathFromFileURI(StrView uri, Status&);

Str FileURIFromPath(StrView path);

}  // namespace automat
